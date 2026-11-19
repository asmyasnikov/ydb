#pragma once

#include <ydb/core/kqp/host/new_parser/rbo/kqp_rbo_lowering.h>
#include <ydb/public/api/protos/ydb_value.pb.h>

namespace NYql {
class TKikimrSessionContext;
}

namespace NKikimr::NKqp::NParsedQueryBench {

// Immutable catalog for offline syntax-to-logical-plan measurements.
class TOfflineCatalog {
public:
    static TOfflineCatalog Load(const TString& path);

    const TString& GetDatabase() const { return Database; }
    void ConfigureSession(NYql::TKikimrSessionContext& session) const;
    NNewYql::TNameResolutionSettings NameSettings() const;
    NNewYql::TTableMetadataPtr Resolve(const TString& cluster, const TString& path) const;
    NYql::EStorageType Storage(const NNewYql::TTableMetadata& metadata) const;
    NNewYql::TRboTable Lookup(NYql::TExprContext& ctx, const NNewYql::TTableMetadata& metadata) const;

private:
    struct TEntry {
        NNewYql::TTableMetadataPtr Metadata;
        NYql::EStorageType Storage;
    };

    TString Cluster = "benchmark";
    TString Database = "/Root";
    TString PathPrefix = "/Root";
    THashMap<TString, TEntry> Tables;
};

NNewYql::TAstTypeAnnotationSettings MakeOfflineTypeSettings(
    const google::protobuf::Map<TString, Ydb::TypedValue>& parameters, NYql::TExprContext& ctx);

} // namespace NKikimr::NKqp::NParsedQueryBench
