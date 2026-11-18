#include "catalog.h"
#include "native_codec.h"

#include <ydb/core/kqp/host/new_parser/kqp_new_parser.h>
#include <ydb/core/kqp/host/new_parser/rbo/kqp_rbo_lowering.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider.h>
#include <ydb/tests/tools/parsed_query_bench/measurement.h>
#include <ydb/tests/tools/parsed_query_bench/proto/execute_query.pb.h>
#include <ydb/tests/tools/parsed_query_bench/syntax/antlr_ast_codec.h>
#include <ydb/tests/tools/parsed_query_bench/syntax/cst_codec.h>
#include <ydb/tests/tools/parsed_query_bench/syntax/script_codec.h>

#include <yql/essentials/ast/yql_type_string.h>
#include <yql/essentials/core/yql_type_annotation.h>

#include <google/protobuf/util/message_differencer.h>

#include <util/generic/yexception.h>
#include <util/string/cast.h>

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <variant>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using TRequest = Ydb::Query::Benchmark::ExecuteQueryRequest;

enum class EStage { Text, AntlrAst, AntlrCst, Script, NativeQuery, Count };

constexpr std::array<TStringBuf, static_cast<size_t>(EStage::Count)> StageNames = {
    "text", "antlr_ast", "antlr_cst", "script", "native_query",
};

EStage GetStage(const TRequest& request) {
    if (request.has_query_content()) {
        Y_ENSURE(request.query_content().syntax() == Ydb::Query::SYNTAX_YQL_V1, "Expected YQL v1 query content");
        return EStage::Text;
    }
    switch (request.parsed_query().stage_case()) {
        case Ydb::Query::ParsedQuery::kAntlrAst: return EStage::AntlrAst;
        case Ydb::Query::ParsedQuery::kAntlrCst: return EStage::AntlrCst;
        case Ydb::Query::ParsedQuery::kScript: return EStage::Script;
        case Ydb::Query::ParsedQuery::kNativeQuery: return EStage::NativeQuery;
        default: ythrow yexception() << "Missing or unsupported ParsedQuery stage";
    }
}

TString ReadFile(const char* name) {
    std::ifstream file(name, std::ios::binary | std::ios::ate);
    Y_ENSURE(file, "Cannot read " << name);
    const std::streamoff length = file.tellg();
    Y_ENSURE(length >= 0 && length <= static_cast<std::streamoff>(MaxRequestBytes), "Request file size limit exceeded");
    TString bytes(static_cast<size_t>(length), '\0');
    file.seekg(0);
    Y_ENSURE(file.read(bytes.Detach(), bytes.size()), "Could not read " << name);
    return bytes;
}

void WriteFile(const char* name, const TString& bytes) {
    Y_ENSURE(bytes.size() <= MaxRequestBytes, "Request file size limit exceeded");
    std::ofstream file(name, std::ios::binary);
    Y_ENSURE(file && file.write(bytes.data(), bytes.size()), "Could not write " << name);
}

template <class TMessage>
TString Encode(const TMessage& message) {
    TString bytes;
    Y_ENSURE(message.SerializeToString(&bytes) && bytes.size() <= MaxRequestBytes,
        "Cannot serialize protobuf message within the file size limit");
    return bytes;
}

std::unique_ptr<NNewYql::TScript> ParseTextInput(const TString& sql) {
    NSQLTranslation::TTranslationSettings settings;
    settings.InferSyntaxVersion = true;
    auto parsed = ParseQueryWithNewParser(sql, settings, false);
    Y_ENSURE(parsed.IsOk(), parsed.Issues.ToString());
    return std::move(parsed.Ast);
}

TNativeSourceOrdinals SourceOrdinals(const NNewYql::TScript& script) {
    TNativeSourceOrdinals ordinals;
    for (size_t i = 0; i < script.Statements.size(); ++i) {
        if (std::holds_alternative<NNewYql::TQueryStatement>(script.Statements[i].Value)) {
            ordinals.Statements.push_back(static_cast<ui32>(i));
        }
    }
    return ordinals;
}

TNativeSourceOrdinals SourceOrdinals(const Ydb::Query::NativeQuery& query) {
    TNativeSourceOrdinals ordinals;
    for (const auto& root : query.roots()) {
        ordinals.Statements.push_back(root.statement_index());
    }
    return ordinals;
}

struct TLogicalRoot {
    TString Plan;
    TVector<TString> ResultTypes;
    TVector<std::pair<TInfoUnitId, TString>> Columns;
    TVector<TString> QueryColumns;

    bool operator==(const TLogicalRoot&) const = default;
};

struct TLogicalPlan {
    TVector<TLogicalRoot> Roots;
    TVector<TString> Tables;

    bool operator==(const TLogicalPlan&) const = default;
};

std::shared_ptr<TNativeQuery> LowerScript(const NNewYql::TScript& script, TExprContext& ctx, TTypeAnnotationContext& types,
    const TOfflineCatalog& catalog, const TNativeParameters& parameters, TLogicalPlan* signature = nullptr)
{
    for (const auto& statement : script.Statements) {
        Y_ENSURE(statement.Explain == NNewYql::EExplainMode::None
            && !std::holds_alternative<NNewYql::TWriteStatement>(statement.Value),
            "Offline native benchmark supports read-only queries");
    }
    const auto names = NNewYql::ResolveNames(script, catalog.NameSettings(),
        [&](const TString& cluster, const TString& path) { return catalog.Resolve(cluster, path); });
    Y_ENSURE(names.IsOk(), names.Issues.ToString());
    Y_ENSURE(names.Names->ProviderPragmas.empty(), "Offline catalog cannot apply provider configuration pragmas");
    const auto annotations = NNewYql::AnnotateAst(script, *names.Names, ctx, types, MakeOfflineTypeSettings(parameters, ctx));
    Y_ENSURE(annotations, ctx.IssueManager.GetIssues().ToString());
    const auto lookup = [&](const NNewYql::TTableMetadata& metadata) { return catalog.Lookup(ctx, metadata); };

    auto result = std::make_shared<TNativeQuery>();
    std::set<TString> tablePaths;
    for (const auto& [source, metadata] : names.Names->Tables) {
        Y_UNUSED(source);
        tablePaths.insert(metadata->Path);
    }
    for (const auto& path : tablePaths) {
        auto& read = result->Execution.TableOperations.emplace_back();
        read.SetTable(path);
        read.SetOperation(static_cast<ui32>(NYql::TYdbOperation::Select));
        if (signature) {
            signature->Tables.push_back(path);
        }
    }
    for (const auto& statement : script.Statements) {
        const auto* queryStatement = std::get_if<NNewYql::TQueryStatement>(&statement.Value);
        if (!queryStatement) {
            continue;
        }
        const auto& query = *queryStatement->Query;
        auto lowered = NNewYql::LowerToRbo(query, script, *names.Names, *annotations, ctx, lookup, false);
        TVector<TString> columns;
        for (const auto& column : lowered->GetColumns().Items()) {
            columns.push_back(column.second);
        }
        auto root = MakeIntrusive<TOpRoot>(lowered->GetInput(), lowered->Pos, lowered->GetColumns(), columns);
        root->PlanProps = std::move(lowered->PlanProps);
        root->PlanProps.ResultIndex = result->Roots.size();
        for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) {
            item.Current->BindExpressionPlanProps(&root->PlanProps);
        }
        root->RecomputeOutputIUsSubtree();
        root->ComputeParents();
        if (signature) {
            auto& detail = signature->Roots.emplace_back();
            detail.Plan = root->PlanToString(ctx);
            const auto& orderedColumns = root->GetColumns().Items();
            detail.Columns.assign(orderedColumns.begin(), orderedColumns.end());
            detail.QueryColumns = root->GetQueryColumns();
            const auto& outputIds = names.Names->QueryOutputs.at(&query);
            const auto& resultTypes = annotations->QueryOutputs.at(&query);
            TVector<size_t> order(outputIds.size());
            std::iota(order.begin(), order.end(), 0);
            const auto* select = std::get_if<NNewYql::TSelect>(&query.Body);
            if (select && select->Columns.size() == 1 && names.Names->Stars.contains(select->Columns.front().Expression.get())) {
                // LowerToRbo sorts the result of a plain SELECT * when ordered columns are disabled.
                std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
                    return names.Names->Columns[outputIds[left]].Name < names.Names->Columns[outputIds[right]].Name;
                });
            }
            for (const auto index : order) {
                detail.ResultTypes.push_back(NYql::FormatType(resultTypes[index]));
            }
        }
        result->Roots.push_back(std::move(root));
        result->Results.push_back({std::move(columns), 0});
        result->Execution.DiscardResults.push_back(false);
    }
    Y_ENSURE(!result->Roots.empty(), "Offline benchmark requires a SELECT result");
    return result;
}

void CheckLambdas(const TNativeQuery& query) {
    for (const auto& root : query.Roots) {
        for (const auto& item : IterateSubtreeWithSubplans(root.Get(), root->PlanProps)) {
            for (const auto& expression : item.Current->GetExpressions()) {
                Y_ENSURE(expression.get().PlanProps == &root->PlanProps, "Invalid NativeQuery PlanProps ownership");
                NYql::CheckArguments(*expression.get().GetLambda());
            }
        }
    }
}

void CheckRoundTrip(const Ydb::Query::NativeQuery& wire, const TOfflineCatalog* catalog = nullptr,
    const TNativeParameters* parameters = nullptr)
{
    const auto bytes = Encode(wire);
    Ydb::Query::NativeQuery decoded;
    DecodeRequest(bytes, decoded);
    TExprContext ctx;
    const auto imported = ImportNativeQuery(decoded, ctx, catalog, nullptr, parameters);
    CheckLambdas(*imported);
    Y_ENSURE(Encode(ExportNativeQuery(*imported, SourceOrdinals(decoded), catalog, parameters)) == bytes, "NativeQuery changed across the round-trip");
}

void CheckRejected(const Ydb::Query::NativeQuery& wire) {
    TExprContext ctx;
    try {
        ImportNativeQuery(wire, ctx);
    } catch (const yexception&) {
        return;
    }
    ythrow yexception() << "Malformed NativeQuery was accepted";
}

struct TSample {
    TMeasurementTime Start;
    TMeasurementTime Decoded;
    TMeasurementTime Imported;
    TMeasurementTime Done;
};

struct TBenchmarkContext {
    TOfflineCatalog Catalog;
    bool HasCatalog = false;

    const TOfflineCatalog* NativeCatalog() const {
        return HasCatalog ? &Catalog : nullptr;
    }
};

TString Prepare(EStage stage, const TString& text, const TRequest& envelope, const TBenchmarkContext& benchmark) {
    TRequest request(envelope);
    request.clear_query();
    switch (stage) {
        case EStage::Text: {
            auto* content = request.mutable_query_content();
            content->set_syntax(Ydb::Query::SYNTAX_YQL_V1);
            content->set_text(text);
            break;
        }
        case EStage::AntlrAst:
            *request.mutable_parsed_query()->mutable_antlr_ast() = ExportAntlrAst(text, {});
            break;
        case EStage::AntlrCst:
            *request.mutable_parsed_query()->mutable_antlr_cst() = ExportAntlrCst(text, {});
            break;
        case EStage::Script:
            *request.mutable_parsed_query()->mutable_script() = ExportSyntaxScript(text, {});
            break;
        case EStage::NativeQuery: {
            TExprContext ctx;
            TTypeAnnotationContext types;
            const auto script = ParseTextInput(text);
            const auto query = LowerScript(*script, ctx, types, benchmark.Catalog, request.parameters());
            *request.mutable_parsed_query()->mutable_native_query() =
                ExportNativeQuery(*query, SourceOrdinals(*script), benchmark.NativeCatalog(), &request.parameters());
            break;
        }
        default:
            ythrow yexception() << "Missing or unsupported input format";
    }
    return Encode(request);
}

std::unique_ptr<NNewYql::TScript> ToScript(const TRequest& request, EStage stage) {
    switch (stage) {
        case EStage::Text: return ParseTextInput(request.query_content().text());
        case EStage::AntlrAst: return ImportAntlrAst(request.parsed_query().antlr_ast());
        case EStage::AntlrCst: return ImportAntlrCst(request.parsed_query().antlr_cst());
        case EStage::Script: return ImportScript(request.parsed_query().script());
        default: ythrow yexception() << "Expected a syntax input";
    }
}

void ValidateEnvelope(const TRequest& request) {
    Y_ENSURE(request.exec_mode() == Ydb::Query::EXEC_MODE_EXECUTE,
        "Offline benchmark requires execute mode");
}

std::shared_ptr<TNativeQuery> ToNative(const TRequest& request, EStage stage, TExprContext& ctx,
    std::optional<TTypeAnnotationContext>& types, const TBenchmarkContext& benchmark, std::unique_ptr<NNewYql::TScript>& script,
    TMeasurementTime* imported = nullptr, TLogicalPlan* signature = nullptr)
{
    ValidateEnvelope(request);
    std::shared_ptr<TNativeQuery> plan;
    if (stage == EStage::NativeQuery) {
        TNativeResultTypes outputTypes;
        plan = ImportNativeQuery(request.parsed_query().native_query(), ctx, benchmark.NativeCatalog(),
            signature ? &outputTypes : nullptr, &request.parameters());
        if (signature) {
            for (const auto& row : outputTypes) {
                auto& root = signature->Roots.emplace_back();
                for (const auto* type : row) {
                    root.ResultTypes.push_back(NYql::FormatType(type));
                }
            }
        }
    } else {
        script = ToScript(request, stage);
    }
    if (imported) {
        *imported = TMeasurementTime::Now();
    }
    if (plan) {
        return plan;
    }
    return LowerScript(*script, ctx, types.emplace(), benchmark.Catalog, request.parameters(), signature);
}

struct TInput {
    EStage Stage;
    TString Bytes;
};

TRequest ExecutionEnvelope() {
    TRequest request;
    request.set_exec_mode(Ydb::Query::EXEC_MODE_EXECUTE);
    return request;
}

TVector<TInput> MakeInputs(const TString& sql) {
    TVector<TInput> inputs;
    for (size_t i = 0; i < StageNames.size(); ++i) {
        const auto stage = static_cast<EStage>(i);
        inputs.push_back({stage, Prepare(stage, sql, ExecutionEnvelope(), {})});
    }
    return inputs;
}

TVector<TInput> ReadInputs(int count, const char** paths) {
    TVector<TInput> inputs;
    for (int i = 0; i < count; ++i) {
        auto bytes = ReadFile(paths[i]);
        TRequest request;
        DecodeRequest(bytes, request);
        inputs.push_back({GetStage(request), std::move(bytes)});
    }
    return inputs;
}

void Verify(const TVector<TInput>& inputs, const TBenchmarkContext& benchmark) {
    const bool hasNative = inputs.size() == StageNames.size();
    Y_ENSURE(hasNative || inputs.size() == StageNames.size() - 1, "Provide four syntax requests or all five requests");
    const auto equals = google::protobuf::util::MessageDifferencer::Equals;
    std::array<bool, StageNames.size()> seen{};
    TRequest expectedEnvelope;
    Ydb::Query::NativeQuery expectedPlan;
    std::optional<Ydb::Query::SyntaxScript> expectedScript;
    TLogicalPlan expectedLogicalPlan;
    TVector<TVector<TString>> expectedTypes;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& [stage, bytes] = inputs[i];
        const auto index = static_cast<size_t>(stage);
        const auto name = StageNames[index];
        Y_ENSURE(!seen[index], "Duplicate benchmark stage: " << name);
        Y_ENSURE(hasNative || stage != EStage::NativeQuery, "Four-input comparisons require the four syntax stages");
        seen[index] = true;
        TRequest request;
        DecodeRequest(bytes, request);
        TExprContext ctx;
        std::optional<TTypeAnnotationContext> types;
        std::unique_ptr<NNewYql::TScript> script;
        TLogicalPlan logicalPlan;
        const auto plan = ToNative(request, stage, ctx, types, benchmark, script, nullptr,
            &logicalPlan);
        CheckLambdas(*plan);
        Y_ENSURE(logicalPlan.Roots.size() == plan->Roots.size(), "Missing logical result types for " << name);
        TVector<TVector<TString>> actualTypes;
        for (const auto& root : logicalPlan.Roots) {
            actualTypes.push_back(root.ResultTypes);
        }
        if (i != 0) {
            Y_ENSURE(expectedTypes == actualTypes, "Logical result types differ for " << name);
        } else {
            expectedTypes = std::move(actualTypes);
        }
        if (script) {
            auto wire = ExportScript(*script);
            if (expectedScript) {
                Y_ENSURE(equals(*expectedScript, wire), "Syntax tree differs for " << name);
            } else {
                expectedScript = std::move(wire);
            }
        }
        if (hasNative) {
            const auto ordinals = script ? SourceOrdinals(*script) : SourceOrdinals(request.parsed_query().native_query());
            auto wire = ExportNativeQuery(*plan, ordinals, benchmark.NativeCatalog(), &request.parameters());
            if (stage == EStage::NativeQuery) {
                Y_ENSURE(equals(request.parsed_query().native_query(), wire), "NativeQuery changed across import and re-export");
            }
            if (i != 0) {
                Y_ENSURE(equals(expectedPlan, wire), "Logical plan differs for " << name);
            } else {
                expectedPlan = std::move(wire);
            }
        } else if (i != 0) {
            Y_ENSURE(expectedLogicalPlan == logicalPlan, "Types or logical plan differ for " << name);
        } else {
            expectedLogicalPlan = std::move(logicalPlan);
        }
        request.clear_query();
        if (i != 0) {
            Y_ENSURE(equals(expectedEnvelope, request), "Request envelope differs for " << name);
        } else {
            expectedEnvelope = std::move(request);
        }
    }
    if (hasNative) {
        CheckRoundTrip(expectedPlan, benchmark.NativeCatalog(), &expectedEnvelope.parameters());
    }
}

TSample Measure(const TInput& input, const TBenchmarkContext& benchmark) {
    TSample sample;
    sample.Start = TMeasurementTime::Now();
    TRequest request;
    DecodeRequest(input.Bytes, request);
    sample.Decoded = TMeasurementTime::Now();
    const auto stage = GetStage(request);
    TExprContext ctx;
    std::optional<TTypeAnnotationContext> types;
    std::unique_ptr<NNewYql::TScript> script;
    const auto plan = ToNative(request, stage, ctx, types, benchmark, script, &sample.Imported);
    sample.Done = TMeasurementTime::Now();
    if (stage == EStage::NativeQuery) {
        sample.Imported = sample.Done;
    }
    Y_ENSURE(!plan->Roots.empty(), "No native query results");
    return sample;
}

template <class TCases, class TMeasure, class TPrint>
void RunRounds(const TCases& cases, size_t warmup, size_t repeats, TMeasure&& measure, TPrint&& print) {
    TVector<decltype(measure(cases.front()))> samples;
    samples.reserve(repeats * cases.size());
    const auto index = [&](size_t round, size_t slot) {
        const auto size = cases.size();
        return (round + ((round / size) % 2 ? size - 1 - slot : slot)) % size;
    };
    for (size_t round = 0; round < warmup + repeats; ++round) {
        for (size_t j = 0; j < cases.size(); ++j) {
            auto sample = measure(cases[index(round, j)]);
            if (round >= warmup) {
                samples.push_back(std::move(sample));
            }
        }
    }
    for (size_t round = 0; round < repeats; ++round) {
        for (size_t j = 0; j < cases.size(); ++j) {
            print(cases[index(round + warmup, j)], round, samples[round * cases.size() + j]);
        }
    }
}

void Bench(const TVector<TInput>& inputs, const TBenchmarkContext& benchmark, size_t warmup, size_t repeats) {
    Verify(inputs, benchmark);
    std::cout << "stage,iteration,bytes,decode_wall_ns,decode_cpu_ns,parse_or_import_wall_ns,parse_or_import_cpu_ns,"
        "frontend_wall_ns,frontend_cpu_ns,total_wall_ns,total_cpu_ns\n";
    RunRounds(inputs, warmup, repeats, [&](const TInput& input) {
        return Measure(input, benchmark);
    }, [](const TInput& input, size_t round, const TSample& sample) {
        const auto& [start, decoded, imported, done] = sample;
        std::cout << StageNames[static_cast<size_t>(input.Stage)] << ',' << round << ',' << input.Bytes.size()
            << ',' << decoded.Wall - start.Wall << ',' << decoded.Cpu - start.Cpu
            << ',' << imported.Wall - decoded.Wall << ',' << imported.Cpu - decoded.Cpu
            << ',' << done.Wall - imported.Wall << ',' << done.Cpu - imported.Cpu
            << ',' << done.Wall - start.Wall << ',' << done.Cpu - start.Cpu << '\n';
    });
}

struct TClientSample {
    size_t Bytes;
    TMeasurementTime Start;
    TMeasurementTime Done;
};

void Client(const TString& sql, const TRequest& envelope, const TBenchmarkContext& benchmark, const TVector<EStage>& stages,
    size_t warmup, size_t repeats)
{
    ValidateEnvelope(envelope);
    std::cout << "stage,iteration,bytes,client_wall_ns,client_cpu_ns\n";
    RunRounds(stages, warmup, repeats, [&](EStage stage) {
        const auto start = TMeasurementTime::Now();
        const auto bytes = Prepare(stage, sql, envelope, benchmark);
        const auto done = TMeasurementTime::Now();
        return TClientSample{bytes.size(), start, done};
    }, [](EStage stage, size_t round, const TClientSample& sample) {
        std::cout << StageNames[static_cast<size_t>(stage)] << ',' << round << ',' << sample.Bytes
            << ',' << sample.Done.Wall - sample.Start.Wall << ',' << sample.Done.Cpu - sample.Start.Cpu << '\n';
    });
}

void Export(const TString& sql, const char** paths, size_t stages, const TRequest& envelope, const TBenchmarkContext& benchmark) {
    TVector<TInput> inputs;
    for (size_t i = 0; i < stages; ++i) {
        const auto stage = static_cast<EStage>(i);
        inputs.push_back({stage, Prepare(stage, sql, envelope, benchmark)});
    }
    Verify(inputs, benchmark);
    for (size_t i = 0; i < inputs.size(); ++i) {
        WriteFile(paths[i], inputs[i].Bytes);
    }
}

void SelfTest() {
    for (const auto* text : {"SELECT 1", "SELECT true AS present, 21u AS n WHERE true",
        "SELECT 1; SELECT 2l", "DECLARE $unused AS Uint64; SELECT 1; SELECT 2",
        "SELECT 1 WHERE CAST('0' AS Decimal(7,0)) = CAST('0' AS Decimal(7,0))"}) {
        const auto inputs = MakeInputs(text);
        Verify(inputs, {});
    }
    TExprContext ctx;
    TTypeAnnotationContext types;
    const auto script = ParseTextInput("SELECT 1");
    const auto valid = ExportNativeQuery(*LowerScript(*script, ctx, types, {}, {}), SourceOrdinals(*script));
    Y_ENSURE(valid.results_size() == 1 && valid.roots(0).columns_size() == 1
        && valid.functions_size() == 1 && valid.functions(0).name() == "Int32",
        "Unexpected logical result contract for SELECT 1");
    {
        TExprContext gapCtx;
        TTypeAnnotationContext gapTypes;
        const auto gapScript = ParseTextInput("DECLARE $unused AS Uint64; SELECT 1; SELECT 2");
        const auto gapQuery = LowerScript(*gapScript, gapCtx, gapTypes, {}, {});
        auto gapWire = ExportNativeQuery(*gapQuery, SourceOrdinals(*gapScript));
        Y_ENSURE(gapWire.roots(0).statement_index() == 1 && gapWire.roots(1).statement_index() == 2,
            "SELECT ordinals lost the preceding DECLARE");
        CheckRoundTrip(gapWire);
        gapWire.mutable_roots(1)->set_statement_index(1);
        CheckRejected(gapWire);
    }
    {
        const TString derived =
            "SELECT l.id, r.id AS rhs FROM (SELECT 1 AS id) AS l LEFT JOIN (SELECT 2 AS id) AS r ON l.id = r.id";
        const TString excluded =
            "SELECT l.id FROM (SELECT 1 AS id) AS l LEFT ONLY JOIN (SELECT 2 AS id) AS r ON l.id = r.id";
        const auto inputs = MakeInputs(derived);
        Verify(inputs, {});
        Verify(MakeInputs(excluded), {});

        TRequest request;
        DecodeRequest(inputs.back().Bytes, request);
        auto malformedHub = request.parsed_query().native_query();
        auto* plan = malformedHub.mutable_roots(0)->mutable_plan();
        Y_ENSURE(plan->replicate_hubs_size() == 2, "Derived join did not preserve both Replicate hubs");
        plan->mutable_replicate_hubs(0)->set_input_operator_id(plan->input_operator_id());
        CheckRejected(malformedHub);

        auto malformedPort = request.parsed_query().native_query();
        auto* operators = malformedPort.mutable_roots(0)->mutable_plan()->mutable_operators();
        const auto port = std::find_if(operators->begin(), operators->end(),
            [](const auto& op) { return op.has_replicate_port(); });
        Y_ENSURE(port != operators->end(), "Missing NativeQuery Replicate port");
        port->mutable_replicate_port()->set_port_index(1);
        CheckRejected(malformedPort);
    }
    {
        Verify(MakeInputs("SELECT COUNT(*)"), {});
        const auto inputs = MakeInputs("SELECT COUNT(DISTINCT q.id) FROM (SELECT 1 AS id) AS q");
        Verify(inputs, {});
        TRequest request;
        DecodeRequest(inputs.back().Bytes, request);
        auto malformedCount = request.parsed_query().native_query();
        auto* operators = malformedCount.mutable_roots(0)->mutable_plan()->mutable_operators();
        const auto aggregate = std::find_if(operators->begin(), operators->end(),
            [](const auto& op) { return op.has_aggregate(); });
        Y_ENSURE(aggregate != operators->end() && aggregate->aggregate().aggregations_size() == 1
            && aggregate->aggregate().aggregations(0).distinct(), "NativeQuery lost COUNT DISTINCT");
        aggregate->mutable_aggregate()->mutable_aggregations(0)->set_unwrap(true);
        CheckRejected(malformedCount);
    }
    auto malformed = valid;
    malformed.mutable_functions(0)->set_name("Unregistered");
    CheckRejected(malformed);

    malformed = valid;
    auto* plan = malformed.mutable_roots(0)->mutable_plan();
    auto* map = plan->mutable_operators(plan->operators_size() - 1)->mutable_map();
    map->mutable_definitions(0)->set_output_iu_id(0);
    CheckRejected(malformed);

    auto memberPlan = valid;
    auto* root = memberPlan.mutable_roots(0);
    auto* member = root->mutable_plan();
    auto* iu = member->add_information_units();
    iu->set_id(member->information_units_size());
    iu->set_column_name("projected");
    const auto addExpression = [&] {
        auto* node = member->add_expressions();
        node->set_id(member->expressions_size());
        return node;
    };
    auto* argument = addExpression();
    argument->mutable_argument()->set_debug_name("row");
    auto* arguments = addExpression();
    arguments->mutable_arguments()->add_arguments(argument->id());
    auto* column = addExpression();
    column->mutable_column()->set_row_argument_expr_id(argument->id());
    column->mutable_column()->set_iu_id(root->columns(0).iu_id());
    auto* lambda = addExpression();
    lambda->mutable_lambda()->set_arguments_expr_id(arguments->id());
    lambda->mutable_lambda()->add_body(column->id());
    auto* op = member->add_operators();
    op->set_id(member->operators_size());
    op->mutable_map()->set_input(member->input_operator_id());
    auto* definition = op->mutable_map()->add_definitions();
    definition->set_lambda_expr_id(lambda->id());
    definition->set_output_iu_id(iu->id());
    member->set_input_operator_id(op->id());
    root->mutable_columns(0)->set_iu_id(iu->id());
    CheckRoundTrip(memberPlan);
    column->mutable_column()->set_row_argument_expr_id(0);
    CheckRejected(memberPlan);

    std::cout << "five-stage logical equivalence and native round-trip checks passed\n";
}

std::pair<size_t, size_t> Iterations(const char* warmupArg, const char* repeatsArg) {
    const auto warmup = FromString<size_t>(warmupArg);
    const auto repeats = FromString<size_t>(repeatsArg);
    Y_ENSURE(warmup <= 100000 && repeats > 0 && repeats <= 100000, "Invalid iteration count");
    return {warmup, repeats};
}

TRequest ReadEnvelope(const std::optional<TString>& templatePath) {
    auto envelope = ExecutionEnvelope();
    if (templatePath) {
        DecodeRequest(ReadFile(templatePath->c_str()), envelope);
    }
    envelope.clear_query();
    return envelope;
}

TVector<EStage> ReadStages(int count, const char** names) {
    TVector<EStage> stages;
    std::array<bool, StageNames.size()> seen{};
    for (int i = 0; i < count; ++i) {
        const auto stage = std::find(StageNames.begin(), StageNames.end(), TStringBuf(names[i]));
        Y_ENSURE(stage != StageNames.end(), "Unknown benchmark stage: " << names[i]);
        const auto index = stage - StageNames.begin();
        Y_ENSURE(!seen[index], "Duplicate benchmark stage: " << names[i]);
        seen[index] = true;
        stages.push_back(static_cast<EStage>(index));
    }
    return stages;
}

int Run(int argc, const char** argv) {
    TBenchmarkContext benchmark;
    std::optional<TString> templatePath;
    int first = 1;
    while (first + 1 < argc) {
        const TStringBuf option = argv[first];
        if (option == "--catalog") {
            Y_ENSURE(!benchmark.HasCatalog, "Specify one offline catalog");
            benchmark.Catalog = TOfflineCatalog::Load(argv[first + 1]);
            benchmark.HasCatalog = true;
        } else if (option == "--template") {
            Y_ENSURE(!templatePath, "Specify one request template");
            templatePath = argv[first + 1];
        } else {
            break;
        }
        first += 2;
    }
    const TStringBuf mode = first < argc ? argv[first] : "";
    const auto validStageCount = [](int count) {
        return count == static_cast<int>(StageNames.size()) || count == static_cast<int>(StageNames.size() - 1);
    };
    const int args = argc - first - 1;
    Y_ENSURE(!templatePath || mode == "--export" || mode == "--client", "Request template is only used for export and client timing");
    if (mode == "--self-test" && args == 0 && !benchmark.HasCatalog) {
        SelfTest();
        return 0;
    }
    if (mode == "--export" && validStageCount(args - 1)) {
        Export(ReadFile(argv[first + 1]), argv + first + 2, args - 1, ReadEnvelope(templatePath), benchmark);
        return 0;
    }
    if (mode == "--verify" && validStageCount(args)) {
        Verify(ReadInputs(args, argv + first + 1), benchmark);
        std::cout << args << "-stage logical equivalence verified\n";
        return 0;
    }
    if (mode == "--bench" && validStageCount(args - 2)) {
        const auto [warmup, repeats] = Iterations(argv[first + 1], argv[first + 2]);
        Bench(ReadInputs(args - 2, argv + first + 3), benchmark, warmup, repeats);
        return 0;
    }
    if (mode == "--client" && args >= 4) {
        const auto [warmup, repeats] = Iterations(argv[first + 1], argv[first + 2]);
        Client(ReadFile(argv[first + 3]), ReadEnvelope(templatePath), benchmark,
            ReadStages(args - 3, argv + first + 4), warmup, repeats);
        return 0;
    }
    std::cerr << "Usage:\n"
        << "  parsed_query_native_bench --self-test\n"
        << "  parsed_query_native_bench [--catalog CATALOG.json] [--template REQUEST.pb]"
           " --export QUERY.sql TEXT.pb ANTLR_AST.pb ANTLR_CST.pb SCRIPT.pb [NATIVE.pb]\n"
        << "  parsed_query_native_bench [--catalog CATALOG.json]"
           " --verify TEXT.pb ANTLR_AST.pb ANTLR_CST.pb SCRIPT.pb [NATIVE.pb]\n"
        << "  parsed_query_native_bench [--catalog CATALOG.json]"
           " --bench WARMUP REPEATS TEXT.pb ANTLR_AST.pb ANTLR_CST.pb SCRIPT.pb [NATIVE.pb]\n"
        << "  parsed_query_native_bench [--catalog CATALOG.json] [--template REQUEST.pb]"
           " --client WARMUP REPEATS QUERY.sql STAGE...\n"
        << "  Include NATIVE.pb for inputs supported by the native codec.\n";
    return 2;
}

} // namespace
} // namespace NKikimr::NKqp::NParsedQueryBench

int main(int argc, const char** argv) {
    try {
        return NKikimr::NKqp::NParsedQueryBench::Run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
