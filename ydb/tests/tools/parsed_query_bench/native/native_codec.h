#pragma once

#include <ydb/core/kqp/opt/rbo/kqp_native_query.h>
#include <ydb/public/api/protos/ydb_value.pb.h>
#include <ydb/tests/tools/parsed_query_bench/proto/logical_plan.pb.h>

namespace NKikimr::NKqp::NParsedQueryBench {

class TOfflineCatalog;

using TNativeResultTypes = TVector<TVector<const NYql::TTypeAnnotationNode*>>;
using TNativeParameters = google::protobuf::Map<TString, Ydb::TypedValue>;

struct TNativeSourceOrdinals {
    TVector<ui32> Statements;
};

// Offline read-only logical plans. Table reads require the canonical catalog.
Ydb::Query::NativeQuery ExportNativeQuery(const TNativeQuery& query, const TNativeSourceOrdinals& source,
    const TOfflineCatalog* catalog = nullptr, const TNativeParameters* parameters = nullptr);
std::shared_ptr<TNativeQuery> ImportNativeQuery(const Ydb::Query::NativeQuery& query,
    NYql::TExprContext& ctx, const TOfflineCatalog* catalog = nullptr, TNativeResultTypes* resultTypes = nullptr,
    const TNativeParameters* parameters = nullptr);

Ydb::Query::LogicalPlan ExportLogicalPlan(const TNativeQuery& query, const TNativeSourceOrdinals& source,
    const TOfflineCatalog* catalog, const TNativeParameters* parameters);
std::shared_ptr<TNativeQuery> ImportLogicalPlan(const Ydb::Query::LogicalPlan& query, NYql::TExprContext& ctx,
    const TOfflineCatalog* catalog, TNativeResultTypes* resultTypes, const TNativeParameters* parameters);

TString LogicalSignature(TOpRoot& root);

} // namespace NKikimr::NKqp::NParsedQueryBench
