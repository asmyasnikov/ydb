#include "catalog.h"

#include <ydb/core/kqp/expr_nodes/kqp_expr_nodes.h>
#include <ydb/core/kqp/host/new_parser/kqp_new_parser.h>
#include <ydb/core/kqp/provider/yql_kikimr_provider.h>

#include <library/cpp/json/json_reader.h>
#include <util/generic/hash_set.h>
#include <util/generic/yexception.h>
#include <util/string/builder.h>
#include <util/string/cast.h>
#include <yql/essentials/public/udf/udf_data_type.h>

#include <fstream>
#include <limits>
#include <set>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using NJson::TJsonValue;

constexpr std::streamoff MaxCatalogBytes = 16 << 20;

TString ReadCatalogFile(const TString& path) {
    std::ifstream stream(path.c_str(), std::ios::binary | std::ios::ate);
    Y_ENSURE(stream, "Cannot read offline catalog: " << path);
    const std::streamoff size = stream.tellg();
    Y_ENSURE(size > 0 && size <= MaxCatalogBytes, "Invalid offline catalog size");
    TString bytes(size, '\0');
    stream.seekg(0);
    Y_ENSURE(stream.read(bytes.Detach(), size), "Cannot read offline catalog contents");
    return bytes;
}

const TString& NonemptyString(const TJsonValue& object, TStringBuf field) {
    const auto& value = object[field];
    Y_ENSURE(value.IsString() && !value.GetString().empty() && value.GetString().find('\0') == TString::npos,
        "Invalid offline catalog string: " << field);
    return value.GetString();
}

ui64 PositiveInteger(const TJsonValue& object, TStringBuf field) {
    const auto& value = object[field];
    if (value.IsUInteger()) {
        const auto result = value.GetUInteger();
        Y_ENSURE(result > 0, "Invalid offline catalog integer: " << field);
        return result;
    }
    Y_ENSURE(value.IsInteger() && value.GetInteger() > 0, "Invalid offline catalog integer: " << field);
    return value.GetInteger();
}

bool BoolField(const TJsonValue& object, TStringBuf field) {
    const auto& value = object[field];
    Y_ENSURE(value.IsBoolean(), "Invalid offline catalog boolean: " << field);
    return value.GetBoolean();
}

const TJsonValue::TArray& ArrayField(const TJsonValue& object, TStringBuf field) {
    const auto& value = object[field];
    Y_ENSURE(value.IsArray(), "Invalid offline catalog array: " << field);
    return value.GetArray();
}

bool AbsolutePath(TStringBuf path) {
    return path.StartsWith('/') && (!path.EndsWith('/') || path == "/")
        && path.find("//") == TStringBuf::npos && path.find("/./") == TStringBuf::npos
        && path.find("/../") == TStringBuf::npos && !path.EndsWith("/.") && !path.EndsWith("/..");
}

bool UnderPath(const TString& path, const TString& parent) {
    return path.size() > parent.size()
        && (parent == "/" ? path.StartsWith('/') : path.StartsWith(parent + '/'));
}

const NYql::TTypeAnnotationNode* ParameterType(const Ydb::Type& type, NYql::TExprContext& ctx, unsigned depth) {
    if (depth > 32) {
        ythrow TNewYqlParserFallback() << "Unsupported offline parameter type depth";
    }
    switch (type.type_case()) {
        case Ydb::Type::kTypeId: {
            const auto id = static_cast<ui32>(type.type_id());
            if (id > std::numeric_limits<NKikimr::NUdf::TDataTypeId>::max()) {
                ythrow TNewYqlParserFallback() << "Unsupported offline parameter primitive type ID " << id;
            }
            if (const auto slot = NKikimr::NUdf::FindDataSlot(static_cast<NKikimr::NUdf::TDataTypeId>(id));
                slot && NKikimr::NUdf::GetDataTypeInfo(*slot).ParamsCount == 0) {
                return ctx.MakeType<NYql::TDataExprType>(*slot);
            }
            break;
        }
        case Ydb::Type::kDecimalType: {
            const auto precision = type.decimal_type().precision();
            const auto scale = type.decimal_type().scale();
            Y_ENSURE(precision > 0 && precision <= 35 && scale <= precision, "Invalid Decimal parameter type");
            return ctx.MakeType<NYql::TDataExprParamsType>(NYql::NUdf::EDataSlot::Decimal, ToString(precision), ToString(scale));
        }
        case Ydb::Type::kOptionalType:
            return ctx.MakeType<NYql::TOptionalExprType>(ParameterType(type.optional_type().item(), ctx, depth + 1));
        case Ydb::Type::kListType:
            return ctx.MakeType<NYql::TListExprType>(ParameterType(type.list_type().item(), ctx, depth + 1));
        default:
            break;
    }
    ythrow TNewYqlParserFallback() << "Unsupported offline parameter type case " << static_cast<int>(type.type_case());
}

} // namespace

TOfflineCatalog TOfflineCatalog::Load(const TString& path) {
    TJsonValue catalog;
    Y_ENSURE(NJson::ReadJsonTree(ReadCatalogFile(path), &catalog) && catalog.IsMap(), "Invalid offline catalog JSON");
    Y_ENSURE(PositiveInteger(catalog, "format_version") == 1, "Unsupported offline catalog format");
    TOfflineCatalog result;
    result.Cluster = NonemptyString(catalog, "cluster");
    result.Database = NonemptyString(catalog, "database");
    const auto& database = result.Database;
    result.PathPrefix = NonemptyString(catalog, "table_path_prefix");
    Y_ENSURE(AbsolutePath(database) && AbsolutePath(result.PathPrefix)
        && (result.PathPrefix == database || UnderPath(result.PathPrefix, database)), "Invalid offline catalog path prefix");
    const auto& identity = NonemptyString(catalog, "identity");
    Y_ENSURE(identity == "fixture" || identity == "snapshot", "Invalid offline catalog identity kind");

    std::set<std::pair<ui64, ui64>> tablesById;
    for (const auto& table : ArrayField(catalog, "tables")) {
        auto metadata = std::make_shared<NNewYql::TTableMetadata>();
        metadata->Cluster = result.Cluster;
        metadata->Path = NonemptyString(table, "path");
        Y_ENSURE(AbsolutePath(metadata->Path) && UnderPath(metadata->Path, database), "Invalid offline catalog table path");
        metadata->OwnerId = PositiveInteger(table, "owner_id");
        metadata->TableId = PositiveInteger(table, "table_id");
        metadata->SchemaVersion = PositiveInteger(table, "schema_version");
        Y_ENSURE(tablesById.emplace(metadata->OwnerId, metadata->TableId).second, "Duplicate offline catalog table ID");
        const auto& kind = NonemptyString(table, "storage");
        if (kind != "row" && kind != "column") {
            ythrow TNewYqlParserFallback() << "Unsupported offline catalog table storage: " << kind;
        }
        const auto storage = kind == "row" ? NYql::EStorageType::RowStorage : NYql::EStorageType::ColumnStorage;
        metadata->HasWriteIndexes = BoolField(table, "has_write_indexes");
        THashSet<TString> columnNames;
        THashSet<ui32> columnIds;
        for (const auto& column : ArrayField(table, "columns")) {
            auto& info = metadata->Columns.emplace_back();
            info.Name = NonemptyString(column, "name");
            const auto id = PositiveInteger(column, "id");
            Y_ENSURE(id <= std::numeric_limits<ui32>::max(), "Offline catalog column ID exceeds uint32");
            info.Id = id;
            Y_ENSURE(columnNames.insert(info.Name).second && columnIds.insert(info.Id).second, "Duplicate offline catalog column");
            info.Type = NonemptyString(column, "type");
            info.NotNull = BoolField(column, "not_null");
            const bool generated = BoolField(column, "generated");
            info.HasDefault = generated || BoolField(column, "has_default");
            metadata->HasDefaults |= info.HasDefault;
            metadata->HasGeneratedColumns |= generated;
        }
        Y_ENSURE(!columnNames.empty(), "Offline catalog table has no columns");
        THashSet<TString> keyColumns;
        for (const auto& key : ArrayField(table, "key_columns")) {
            Y_ENSURE(key.IsString() && columnNames.contains(key.GetString()) && keyColumns.insert(key.GetString()).second,
                "Invalid offline catalog key column");
            metadata->KeyColumns.push_back(key.GetString());
        }
        Y_ENSURE(result.Tables.emplace(metadata->Path, TEntry{metadata, storage}).second, "Duplicate offline catalog table path");
    }
    return result;
}

void TOfflineCatalog::ConfigureSession(NYql::TKikimrSessionContext& session) const {
    session.SetCluster(Cluster);
    session.SetDatabase(Database);
    for (const auto& [path, entry] : Tables) {
        auto& table = session.Tables().GetOrAddTable(Cluster, Database, path);
        table.Metadata = MakeIntrusive<NYql::TKikimrTableMetadata>();
        table.Metadata->DoesExist = true;
        table.Metadata->Kind = entry.Storage == NYql::EStorageType::ColumnStorage
            ? NYql::EKikimrTableKind::Olap : NYql::EKikimrTableKind::Datashard;
    }
}

NNewYql::TNameResolutionSettings TOfflineCatalog::NameSettings() const {
    return {Cluster, PathPrefix, {}};
}

NNewYql::TTableMetadataPtr TOfflineCatalog::Resolve(const TString& cluster, const TString& path) const {
    if (cluster != Cluster) {
        ythrow TNewYqlParserFallback() << "Offline catalog does not support cluster: " << cluster;
    }
    if (const auto* entry = Tables.FindPtr(path)) {
        return entry->Metadata;
    }
    ythrow TNewYqlParserFallback() << "Table is outside the offline catalog snapshot: " << path;
}

NYql::EStorageType TOfflineCatalog::Storage(const NNewYql::TTableMetadata& metadata) const {
    const auto* entry = Tables.FindPtr(metadata.Path);
    if (metadata.Cluster != Cluster || !entry || entry->Metadata.get() != &metadata) {
        ythrow TNewYqlParserFallback() << "Offline catalog identity changed before lowering: " << metadata.Path;
    }
    return entry->Storage;
}

NNewYql::TRboTable TOfflineCatalog::Lookup(NYql::TExprContext& ctx, const NNewYql::TTableMetadata& metadata) const {
    const auto storage = Storage(metadata);
    auto node = NYql::NNodes::Build<NYql::NNodes::TKqpTable>(ctx, NYql::TPositionHandle())
        .Path().Build(metadata.Path)
        .PathId().Build(TStringBuilder() << metadata.OwnerId << ':' << metadata.TableId)
        .SysView().Build("")
        .Version().Build(ToString(metadata.SchemaVersion))
        .Done().Ptr();
    return {std::move(node), storage};
}

NNewYql::TAstTypeAnnotationSettings MakeOfflineTypeSettings(
    const google::protobuf::Map<TString, Ydb::TypedValue>& parameters, NYql::TExprContext& ctx)
{
    NNewYql::TAstTypeAnnotationSettings settings;
    settings.Expressions.FlexibleTypes = true;
    settings.Expressions.AnsiIn = true;
    for (const auto& [name, parameter] : parameters) {
        Y_ENSURE(!name.empty() && name != "$" && parameter.has_type(), "Invalid offline runtime parameter");
        const TString sqlName = name.StartsWith('$') ? name : "$" + name;
        const auto* type = ParameterType(parameter.type(), ctx, 0);
        Y_ENSURE(settings.Parameters.emplace(sqlName, type).second, "Duplicate normalized offline runtime parameter");
    }
    return settings;
}

} // namespace NKikimr::NKqp::NParsedQueryBench
