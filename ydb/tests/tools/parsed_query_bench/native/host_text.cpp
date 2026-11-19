#include "host_text.h"

#include <ydb/core/kqp/common/kqp_user_request_context.h>
#include <ydb/core/kqp/common/simple/query_ref.h>
#include <ydb/core/kqp/host/kqp_host.h>
#include <ydb/core/kqp/opt/rbo/kqp_rbo_context.h>
#include <ydb/core/kqp/provider/yql_kikimr_results.h>
#include <ydb/core/kqp/provider/yql_kikimr_settings.h>
#include <ydb/core/scheme/scheme_type_info.h>
#include <ydb/core/testlib/basics/appdata.h>
#include <ydb/tests/tools/parsed_query_bench/measurement.h>
#include <ydb/tests/tools/parsed_query_bench/proto/execute_query.pb.h>
#include <yql/essentials/ast/yql_type_string.h>
#include <yql/essentials/core/services/mounts/yql_mounts.h>

#include <library/cpp/json/json_writer.h>

#include <algorithm>
#include <iostream>
#include <thread>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using namespace NYql;

class THostCatalog final : public IKqpGateway::IKqpTableMetadataLoader {
public:
    THostCatalog(const TOfflineCatalog& catalog, const NScheme::TTypeRegistry& registry)
        : Catalog(catalog), Registry(registry)
    {}

    NThreading::TFuture<IKqpGateway::TTableMetadataResult> LoadTableMetadata(const TString& cluster, const TString& path,
        const IKqpGateway::TLoadTableMetadataSettings&, const TString& database, const TIntrusiveConstPtr<NACLib::TUserToken>&) override
    {
        Y_ENSURE(database == Catalog.GetDatabase(), "Unexpected catalog database");
        auto& table = Tables[path];
        if (!table) {
            const auto source = Catalog.Resolve(cluster, path);
            table = MakeIntrusive<TKikimrTableMetadata>(cluster, path);
            table->DoesExist = true;
            table->PathId = TKikimrPathId(source->OwnerId, source->TableId);
            table->SchemaVersion = source->SchemaVersion;
            const bool columnStore = Catalog.Storage(*source) == NYql::EStorageType::ColumnStorage;
            table->Kind = columnStore ? EKikimrTableKind::Olap : EKikimrTableKind::Datashard;
            table->StoreType = columnStore ? EStoreType::Column : EStoreType::Row;
            for (const auto& column : source->Columns) {
                NScheme::TTypeInfo info;
                TString error;
                Y_ENSURE(NScheme::GetTypeInfo(Registry.GetType(column.Type), {}, column.Type, column.Name, info, error), error);
                table->Columns.emplace(column.Name, TKikimrColumnMetadata(column.Name, column.Id, column.Type, column.NotNull, info));
                table->ColumnOrder.push_back(column.Name);
            }
            table->KeyColumnNames = source->KeyColumns;
        }
        Y_ENSURE(table->Cluster == cluster, "Unexpected catalog cluster");
        IKqpGateway::TTableMetadataResult result;
        result.Metadata = table;
        result.SetSuccess();
        return NThreading::MakeFuture(std::move(result));
    }

    TVector<NKikimrKqp::TKqpTableMetadataProto> GetCollectedSchemeData() override { return {}; }

private:
    const TOfflineCatalog& Catalog;
    const NScheme::TTypeRegistry& Registry;
    THashMap<TString, TKikimrTableMetadataPtr> Tables;
};

bool SameExpression(const TExprNode* left, const TExprNode* right) {
    return left == right || (left && right && CompareExprTrees(left, right));
}

void CompareRoots(const TVector<TIntrusivePtr<TOpRoot>>& expected, TExprContext& expectedContext,
    const TVector<TIntrusivePtr<TOpRoot>>& actual, TExprContext& ctx)
{
    Y_ENSURE(expected.size() == actual.size(), "Stock host root count differs from offline text");
    for (size_t i = 0; i < actual.size(); ++i) {
        const auto& a = actual[i];
        const auto& e = expected[i];
        Y_ENSURE(a->PlanProps.ResultIndex == e->PlanProps.ResultIndex && a->PlanProps.PhysicalTxGroup == e->PlanProps.PhysicalTxGroup
            && a->PlanProps.UnorderedResult == e->PlanProps.UnorderedResult && a->GetQueryColumns() == e->GetQueryColumns()
            && a->GetColumns().Items() == e->GetColumns().Items() && a->PlanToString(ctx) == e->PlanToString(expectedContext),
            "Stock host RBO result differs from offline text at root " << i);
        auto reference = e->begin();
        for (const auto& op : *a) {
            Y_ENSURE(reference != e->end(), "Stock host has additional operators");
            const auto expressions = op.Current->GetExpressions();
            const auto other = (*reference).Current->GetExpressions();
            Y_ENSURE(op.Current->Kind == (*reference).Current->Kind && expressions.size() == other.size(),
                "Stock host operator differs");
            if (op.Current->Kind == EOperator::EmptySource) {
                const auto* source = CastOperator<TOpEmptySource>(op.Current);
                const auto* expectedSource = CastOperator<TOpEmptySource>((*reference).Current);
                Y_ENSURE(SameExpression(source->Input.Get(), expectedSource->Input.Get()), "Stock host source expression differs");
            } else if (op.Current->Kind == EOperator::Source) {
                const auto* read = CastOperator<TOpRead>(op.Current);
                const auto* expectedRead = CastOperator<TOpRead>((*reference).Current);
                Y_ENSURE(SameExpression(read->TableCallable.Get(), expectedRead->TableCallable.Get()), "Stock host table binding differs");
            }
            for (size_t k = 0; k < expressions.size(); ++k) {
                const auto lambda = expressions[k].get().GetLambda();
                const auto ref = other[k].get().GetLambda();
                Y_ENSURE(SameExpression(lambda.Get(), ref.Get()), "Stock host RBO expression differs");
                const auto* type = lambda->Tail().GetTypeAnn().Get();
                const auto* refType = ref->Tail().GetTypeAnn().Get();
                Y_ENSURE((!type && !refType) || (type && refType && FormatType(type) == FormatType(refType)),
                    "Stock host RBO expression type differs");
            }
            ++reference;
        }
        Y_ENSURE(reference == e->end(), "Stock host has fewer operators");
    }
}

void VerifyResultColumns(const TVector<TIntrusivePtr<TOpRoot>>& roots, TRBOContext& rbo, const TVector<TVector<TString>>& expected) {
    Y_ENSURE(roots.size() == expected.size(), "Stock host result count differs");
    TVector<NJson::TJsonValue> results(roots.size());
    for (const auto& root : roots) {
        Y_ENSURE(root->ComputeTypes(rbo) == IGraphTransformer::TStatus::Ok, rbo.ExprCtx.IssueManager.GetIssues().ToString());
        const auto index = root->PlanProps.ResultIndex;
        Y_ENSURE(index && *index < results.size() && !results[*index].IsDefined(), "Invalid result index");
        const auto* row = root->GetInput()->Type->Cast<TListExprType>()->GetItemType()->Cast<TStructExprType>();
        auto& columns = results[*index];
        columns.SetType(NJson::JSON_ARRAY);
        const auto& ordered = root->GetColumns().Items();
        Y_ENSURE(ordered.size() == expected[*index].size(), "Stock host result arity differs");
        size_t j = 0;
        for (const auto& [iu, name] : ordered) {
            const auto* field = row->FindItemType(rbo.ExprCtx.GetIndexAsString(iu));
            Y_ENSURE(field, "Stock host result field is missing");
            const auto type = FormatType(field);
            Y_ENSURE(type == expected[*index][j++], "Stock host result type differs for " << name << ": " << type);
            NJson::TJsonValue column(NJson::JSON_ARRAY);
            column.AppendValue(name);
            column.AppendValue(type);
            columns.AppendValue(std::move(column));
        }
    }
    NJson::TJsonValue output(NJson::JSON_ARRAY);
    for (auto& result : results) {
        output.AppendValue(std::move(result));
    }
    std::cerr << "BENCH_RESULT_COLUMNS=" << NJson::WriteJson(output, false) << '\n';
}

} // namespace

void BenchHostText(const TString& bytes, const TOfflineCatalog& catalog, const TVector<TIntrusivePtr<TOpRoot>>& expected,
    TExprContext& expectedContext, const TVector<TVector<TString>>& expectedTypes, size_t warmup, size_t repeats)
{
    NActors::TTestActorRuntime runtime;
    runtime.Initialize(TAppPrepare(TAppPrepare::TLightweightTag{}).Unwrap());
    const auto names = catalog.NameSettings();
    TModuleResolverState modules;
    Y_ENSURE(GetYqlDefaultModuleResolver(modules.ExprCtx, modules.ModuleResolver), modules.ExprCtx.IssueManager.GetIssues().ToString());
    auto counters = MakeIntrusive<TKqpRequestCounters>();
    const auto dynamic = runtime.GetDynamicCounters();
    counters->Counters = new TKqpCounters(dynamic);
    counters->TxProxyMon = new NTxProxy::TTxProxyMon(dynamic);
    auto gateway = CreateKikimrIcGateway(names.DefaultCluster, NKikimrKqp::QUERY_TYPE_SQL_GENERIC_QUERY,
        catalog.GetDatabase(), catalog.GetDatabase(), std::make_shared<THostCatalog>(catalog, *runtime.GetAppData().TypeRegistry),
        runtime.GetActorSystem(0), runtime.GetNodeId(), counters);
    TKqpSettings defaults;
    struct TSample { TMeasurementTime Start, Decoded, Done; };
    TVector<TSample> samples;
    for (size_t round = 0; round < 1 + warmup + repeats; ++round) {
        const auto sample = runtime.RunCall([&] {
            auto config = MakeIntrusive<TKikimrConfiguration>();
            config->Init(defaults.DefaultSettings.GetDefaultSettings(), names.DefaultCluster, defaults.Settings, false);
            config->_KqpTablePathPrefix = names.TablePathPrefix;
            config->SetEnableNewRBO(true);
            config->SetEnableNewYqlParser(true);
            config->DisableBlockExecution = true;
            config->FreezeDefaults();
            const auto host = CreateKqpHost(gateway, names.DefaultCluster, catalog.GetDatabase(), config, modules.ModuleResolver,
                std::nullopt, nullptr, std::make_shared<TGUCSettings>(), {}, Nothing(), runtime.GetAppData().FunctionRegistry);
            const auto thread = std::this_thread::get_id();
            TSample result{};
            result.Start = TMeasurementTime::Now();
            Ydb::Query::Benchmark::ExecuteQueryRequest request;
            DecodeRequest(bytes, request);
            result.Decoded = TMeasurementTime::Now();
            Y_ENSURE(request.exec_mode() == Ydb::Query::EXEC_MODE_EXECUTE && request.has_query_content()
                && request.query_content().syntax() == Ydb::Query::SYNTAX_YQL_V1, "Stock host requires a YQL text execute request");
            bool reached = false;
            IKqpHost::TPrepareSettings settings;
            settings.YqlSelect = NSQLTranslation::EYqlSelect::Force;
            settings.CompileTrace = MakeIntrusive<TCompileTrace>(NWilson::TTraceId{}, nullptr);
            settings.CompileTrace->CaptureRboRoots = [&](const auto& roots, const TExprNode& input, TExprContext& ctx, auto makeRboContext) {
                result.Done = TMeasurementTime::Now();
                Y_ENSURE(std::this_thread::get_id() == thread, "Stock host changed CPU timing thread");
                Y_ENSURE(input.IsWorld(), "Stock host fell back to the legacy frontend");
                Y_ENSURE(!roots.empty() && std::ranges::none_of(roots, [](const auto& root) { return root->PlanProps.WithEffects; }),
                    "Stock host requires read-only result roots");
                if (!round) {
                    CompareRoots(expected, expectedContext, roots, ctx);
                    auto rbo = makeRboContext();
                    VerifyResultColumns(roots, rbo, expectedTypes);
                }
                reached = true;
            };
            TKqpQueryRef query(request.query_content().text());
            if (!request.parameters().empty()) {
                query.ParameterTypes = std::make_shared<std::map<TString, Ydb::Type>>();
                for (const auto& [name, value] : request.parameters()) {
                    Y_ENSURE(value.has_type(), "Missing type for " << name);
                    query.ParameterTypes->emplace(name, value.type());
                }
            }
            const auto compilation = host->PrepareGenericQuery(query, settings);
            while (!reached && !compilation->HasResult()) {
                const auto ready = compilation->Continue();
                Y_ENSURE(ready.IsReady(), "Stock host requested an unavailable offline service");
                if (ready.GetValue()) { break; }
            }
            Y_ENSURE(reached, "Stock NewParser host did not reach the verified RBO boundary: " << compilation->GetResult().Issues().ToString());
            return result;
        });
        if (round > warmup) { samples.push_back(sample); }
    }
    std::cout << "stage,iteration,bytes,decode_wall_ns,decode_cpu_ns,total_wall_ns,total_cpu_ns\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& [start, decoded, done] = samples[i];
        std::cout << "host_text," << i << ',' << bytes.size() << ',' << decoded.Wall - start.Wall << ',' << decoded.Cpu - start.Cpu
            << ',' << done.Wall - start.Wall << ',' << done.Cpu - start.Cpu << '\n';
    }
}

} // namespace NKikimr::NKqp::NParsedQueryBench
