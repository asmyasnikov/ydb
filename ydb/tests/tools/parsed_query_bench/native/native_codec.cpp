#include "native_codec.h"
#include "catalog.h"

#include <ydb/core/kqp/provider/yql_kikimr_provider.h>
#include <yql/essentials/core/type_ann/type_ann_expr.h>
#include <yql/essentials/core/yql_expr_optimize.h>
#include <yql/essentials/core/yql_expr_type_annotation.h>
#include <yql/essentials/core/yql_type_annotation.h>

#include <util/generic/hash.h>
#include <util/generic/hash_set.h>
#include <util/generic/scope.h>
#include <util/generic/yexception.h>
#include <util/string/cast.h>
#include <util/string/builder.h>

#include <algorithm>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using P = Ydb::Query::NativeQuery;
using L = Ydb::Query::LogicalPlan;

constexpr TStringBuf SemanticsVersion = "kqp-new-parser-721425cc-v1";
constexpr size_t MaxNodes = 1024;
constexpr size_t MaxOperatorDepth = 256;
constexpr size_t MaxExpressionDepth = 64;
constexpr size_t MaxInformationUnits = 16 * 1024;
constexpr size_t MaxExpressions = 64 * 1024;
constexpr size_t MaxSpelling = 1024;

template <class TMessage>
void CheckFields(const TMessage& message) {
    Y_ENSURE(message.GetReflection()->GetUnknownFields(message).empty(), "Unknown NativeQuery field");
}

TString CopyString(TStringBuf value) {
    Y_ENSURE(value.size() <= MaxSpelling, "NativeQuery spelling limit exceeded");
    return TString(value);
}

std::optional<EDataSlot> LiteralSlot(TStringBuf name) {
    if (name == "Bool") { return EDataSlot::Bool; }
    if (name == "Int32") { return EDataSlot::Int32; }
    if (name == "Int64") { return EDataSlot::Int64; }
    if (name == "Uint32") { return EDataSlot::Uint32; }
    if (name == "Uint64") { return EDataSlot::Uint64; }
    if (name == "String") { return EDataSlot::String; }
    return std::nullopt;
}

bool SupportedScalar(TStringBuf name) {
    static const THashSet<TStringBuf> Calls = {
        "==", "!=", "<", ">=", "And", "Not", "Exists", "Coalesce", "If", "SqlIn", "Just", "Unwrap", "Nothing", "Null",
        "Convert", "SafeCast", "StrictCast",
    };
    return LiteralSlot(name).has_value() || Calls.contains(name);
}

bool ValidLiteral(EDataSlot slot, TStringBuf value) {
    if (slot == EDataSlot::String) {
        return value.size() <= MaxSpelling;
    }
    if (value.empty() || value.size() > 21) {
        return false;
    }
    i32 signed32;
    i64 signed64;
    ui32 unsigned32;
    ui64 unsigned64;
    switch (slot) {
        case EDataSlot::Bool: return value == "true" || value == "false";
        case EDataSlot::Int32: return TryFromString(value, signed32);
        case EDataSlot::Int64: return TryFromString(value, signed64);
        case EDataSlot::Uint32: return TryFromString(value, unsigned32);
        case EDataSlot::Uint64: return TryFromString(value, unsigned64);
        default: return false;
    }
}

const TTypeAnnotationNode* DataType(TExprContext& ctx, TStringBuf slotName, TStringBuf precision = {}, TStringBuf scale = {}) {
    const auto slot = NUdf::FindDataSlot(slotName);
    Y_ENSURE(slot, "Unsupported NativeQuery data type: " << slotName);
    if (*slot == EDataSlot::Decimal) {
        ui32 p, s;
        Y_ENSURE(TryFromString(precision, p) && TryFromString(scale, s) && p > 0 && p <= 35 && s <= p,
            "Invalid NativeQuery Decimal type");
        return ctx.MakeType<TDataExprParamsType>(*slot, precision, scale);
    }
    Y_ENSURE(precision.empty() && scale.empty() && NUdf::GetDataTypeInfo(*slot).ParamsCount == 0,
        "Unsupported NativeQuery type parameters");
    return ctx.MakeType<TDataExprType>(*slot);
}

P::Join::Kind WireJoinKind(TStringBuf kind) {
    if (kind == "Inner") { return P::Join::INNER; }
    if (kind == "Left") { return P::Join::LEFT; }
    if (kind == "LeftOnly") { return P::Join::LEFT_ONLY; }
    if (kind == "Cross") { return P::Join::CROSS; }
    ythrow yexception() << "Unsupported NativeQuery JOIN kind: " << kind;
}

TString ReadJoinKind(P::Join::Kind kind) {
    switch (kind) {
        case P::Join::INNER: return "Inner";
        case P::Join::LEFT: return "Left";
        case P::Join::LEFT_ONLY: return "LeftOnly";
        case P::Join::CROSS: return "Cross";
        default: ythrow yexception() << "Unsupported NativeQuery JOIN kind: " << static_cast<int>(kind);
    }
}

const TTypeAnnotationNode& OptionalItemType(const TTypeAnnotationNode& type) {
    return type.GetKind() == ETypeAnnotationKind::Optional ? *type.Cast<TOptionalExprType>()->GetItemType() : type;
}

bool IsSupportedJoinKeyType(const TTypeAnnotationNode& left, const TTypeAnnotationNode& right) {
    const auto& key = OptionalItemType(left);
    return key.GetKind() == ETypeAnnotationKind::Data && key.IsEquatable()
        && IsSameAnnotation(key, OptionalItemType(right));
}

using TUnitTypes = THashMap<TInfoUnitId, const TTypeAnnotationNode*>;
using TParameterTypes = THashMap<TString, const TTypeAnnotationNode*>;

void NullExtend(TExprContext& ctx, TUnitTypes& types, const TUnorderedIUs& units) {
    for (const auto id : units) {
        auto& type = types.at(id);
        if (!type->IsOptionalOrNull()) {
            type = ctx.MakeType<TOptionalExprType>(type);
        }
    }
}

const TTypeAnnotationNode* ColumnType(TExprContext& ctx, const NNewYql::TColumnMetadata& column) {
    TStringBuf name(column.Type);
    const TTypeAnnotationNode* base;
    if (name.StartsWith("Decimal(") && name.EndsWith(')')) {
        auto params = name.SubStr(8, name.size() - 9);
        const auto comma = params.find(',');
        Y_ENSURE(comma != TStringBuf::npos, "Invalid offline Decimal column");
        auto scale = params.SubStr(comma + 1);
        while (scale.StartsWith(' ')) {
            scale.Skip(1);
        }
        base = DataType(ctx, "Decimal", params.Head(comma), scale);
    } else {
        base = DataType(ctx, name);
    }
    return column.NotNull ? base : ctx.MakeType<TOptionalExprType>(base);
}

ui32 TypeId(P& query, const TTypeAnnotationNode& type) {
    P::Type wire;
    if (type.GetKind() == ETypeAnnotationKind::Optional) {
        wire.set_optional_item(TypeId(query, *type.Cast<TOptionalExprType>()->GetItemType()));
    } else if (type.GetKind() == ETypeAnnotationKind::List) {
        wire.set_list_item(TypeId(query, *type.Cast<TListExprType>()->GetItemType()));
    } else if (type.GetKind() == ETypeAnnotationKind::Type) {
        wire.set_type_of(TypeId(query, *type.Cast<TTypeExprType>()->GetType()));
    } else if (type.GetKind() == ETypeAnnotationKind::Tuple) {
        auto* tuple = wire.mutable_tuple();
        for (const auto* item : type.Cast<TTupleExprType>()->GetItems()) {
            tuple->add_type_ids(TypeId(query, *item));
        }
    } else if (type.GetKind() == ETypeAnnotationKind::Unit) {
        wire.mutable_unit();
    } else if (type.GetKind() == ETypeAnnotationKind::Null) {
        wire.mutable_null_type();
    } else {
        Y_ENSURE(type.GetKind() == ETypeAnnotationKind::Data, "Unsupported NativeQuery type");
        const auto slot = type.Cast<TDataExprType>()->GetSlot();
        const auto& info = NUdf::GetDataTypeInfo(slot);
        auto* data = wire.mutable_data();
        data->set_slot(info.Name.data(), info.Name.size());
        if (slot == EDataSlot::Decimal) {
            const auto* decimal = type.Cast<TDataExprParamsType>();
            data->add_parameters(TString(decimal->GetParamOne()));
            data->add_parameters(TString(decimal->GetParamTwo()));
        } else {
            Y_ENSURE(info.ParamsCount == 0, "Unsupported NativeQuery data type parameters");
        }
    }
    for (const auto& existing : query.types()) {
        if (existing.kind_case() != wire.kind_case()) {
            continue;
        }
        if (wire.has_unit() || wire.has_null_type()
            || (wire.has_optional_item() && wire.optional_item() == existing.optional_item())
            || (wire.has_list_item() && wire.list_item() == existing.list_item())
            || (wire.has_type_of() && wire.type_of() == existing.type_of())
            || (wire.has_tuple() && wire.tuple().type_ids_size() == existing.tuple().type_ids_size()
                && std::equal(wire.tuple().type_ids().begin(), wire.tuple().type_ids().end(), existing.tuple().type_ids().begin()))
            || (wire.has_data() && wire.data().slot() == existing.data().slot()
                && wire.data().parameters_size() == existing.data().parameters_size()
                && std::equal(wire.data().parameters().begin(), wire.data().parameters().end(), existing.data().parameters().begin()))) {
            return existing.id();
        }
    }
    Y_ENSURE(static_cast<size_t>(query.types_size()) < MaxNodes, "NativeQuery type registry limit exceeded");
    wire.set_id(query.types_size() + 1);
    const ui32 id = wire.id();
    *query.add_types() = std::move(wire);
    return id;
}

bool IsCountInput(const TTypeAnnotationNode& type, bool distinct) {
    const auto& item = OptionalItemType(type);
    return item.GetKind() == ETypeAnnotationKind::Data && (!distinct || (item.IsHashable() && item.IsEquatable()));
}

bool IsGeneratedAggregatePrefix(TStringBuf prefix) {
    return prefix == "aggregate" || prefix == "aggregate_argument";
}

ui32 FunctionId(P& query, TStringBuf name, P::Function::Kind kind = P::Function::YQL_CALLABLE) {
    for (const auto& function : query.functions()) {
        if (function.kind() == kind && TStringBuf(function.name()) == name) {
            return function.id();
        }
    }
    auto* function = query.add_functions();
    function->set_id(query.functions_size());
    function->set_kind(kind);
    function->set_name(name.data(), name.size());
    return function->id();
}

bool SameWireType(const TTypeAnnotationNode& left, const TTypeAnnotationNode& right) {
    return IsSameAnnotation(left, right)
        || (left.GetHash() == right.GetHash() && FormatType(&left) == FormatType(&right));
}

void CheckExpressionType(const TExprNode& node, const TTypeAnnotationNode& expected) {
    const auto* actual = node.GetTypeAnn().Get();
    Y_ENSURE(!actual || SameWireType(*actual, expected), "NativeQuery type does not match expression");
}

const TTypeAnnotationNode* AnnotateScalarLambda(const TExprNode& lambda, TExprContext& source, TExprContext& ctx,
    TTypeAnnotationContextPtr& annotationContext, const TUnitTypes& unitTypes, IOperator& input)
{
    const auto& row = lambda.Head().Head();
    TVector<const TItemExprType*> fields;
    THashSet<TInfoUnitId> bound;
    VisitExpr(lambda.Tail(), [&](const TExprNode& node) {
        if (node.IsCallable("Member")) {
            Y_ENSURE(&node.Head() == &row, "Out-of-scope NativeQuery lambda argument");
            const auto id = GetMemberId(node);
            Y_ENSURE(unitTypes.contains(id) && input.GetOutputIUs().Contains(id), "Unavailable NativeQuery input IU");
            const auto* type = unitTypes.at(id);
            CheckExpressionType(node, *type);
            if (bound.insert(id).second) {
                fields.push_back(ctx.MakeType<TItemExprType>(TString(node.Tail().Content()), type));
            }
        }
        return true;
    });

    // Reannotate a copy so stock type checking cannot rewrite the plan itself.
    TNodeOnNodeOwnedMap copies;
    auto typed = ctx.DeepCopy(lambda, source, copies, &source != &ctx, false);
    typed->Head().SetTypeAnn(ctx.MakeType<TUnitExprType>());
    typed->Head().Head().SetTypeAnn(ctx.MakeType<TStructExprType>(fields));
    const auto repeats = ctx.RepeatTransformCounter;
    const auto steps = ctx.Step;
    Y_DEFER { ctx.RepeatTransformCounter = repeats; ctx.Step = steps; };
    ctx.RepeatTransformCounter = 0;
    if (!annotationContext) {
        annotationContext = MakeIntrusive<TTypeAnnotationContext>();
    }
    auto transformer = CreateTypeAnnotationTransformer(
        CreateExtCallableTypeAnnotationTransformer(*annotationContext, true), *annotationContext);
    Y_ENSURE(InstantTransform(*transformer, typed, ctx) == IGraphTransformer::TStatus::Ok,
        "NativeQuery scalar type annotation failed");
    const auto* type = typed->Tail().GetTypeAnn().Get();
    Y_ENSURE(type, "NativeQuery lambda result has no type");
    CheckExpressionType(lambda.Tail(), *type);
    for (const auto& [original, copy] : copies) {
        if (const auto* actual = copy->GetTypeAnn().Get()) {
            CheckExpressionType(*original, *actual);
        }
    }
    return type;
}

void CheckLogicalOperator(const IOperator& op) {
    const auto& props = op.Props;
    Y_ENSURE(!op.Type && !props.StageId && props.StageInputEdges.empty() && !props.Algorithm && !props.OrderEnforcer
        && !props.Metadata && !props.Statistics && !props.JoinAlgo && !props.UseBlockHashJoin && !props.Cost
        && !props.LeftShuffleBy && !props.RightShuffleBy, "Unsupported NativeQuery operator analysis or physical properties");
}

using TColumnMetadata = NNewYql::TColumnMetadata;
using TTableMetadata = NNewYql::TTableMetadata;

class TTableWriter {
public:
    TTableWriter(P& query, const TOfflineCatalog* catalog, const TNativeParameters* parameters)
        : Query(query)
        , Catalog(catalog)
    {
        if (parameters) {
            ParameterTypes = MakeOfflineTypeSettings(*parameters, Ctx).Parameters;
        }
    }

    const TTypeAnnotationNode* WriteParameter(const TExprNode& node) {
        Y_ENSURE(node.ChildrenSize() == 2 && node.Head().IsAtom(), "Invalid NativeQuery Parameter");
        const auto name = CopyString(node.Head().Content());
        const auto* bound = ParameterTypes.FindPtr(name);
        const auto* argument = node.Tail().GetTypeAnn().Get();
        Y_ENSURE(bound && argument && argument->GetKind() == ETypeAnnotationKind::Type
            && SameWireType(**bound, *argument->Cast<TTypeExprType>()->GetType()),
            "NativeQuery parameter type differs from the request");
        if (ParametersWritten.insert(name).second) {
            auto* parameter = Query.add_parameters();
            parameter->set_name(name);
            parameter->set_type_id(TypeId(Query, **bound));
        }
        return *bound;
    }

    const TTableMetadata& Resolve(const TOpRead& read) const {
        Y_ENSURE(Catalog && read.GetTable(), "NativeQuery table read requires an offline catalog");
        const NYql::NNodes::TKqpTable node(read.GetTable());
        const auto metadata = Catalog->Resolve(Catalog->NameSettings().DefaultCluster, TString(node.Path().Value()));
        Y_ENSURE(node.PathId().Value() == TStringBuilder() << metadata->OwnerId << ':' << metadata->TableId,
            "NativeQuery read path ID differs from the offline catalog");
        Y_ENSURE(node.Version().Value() == ToString(metadata->SchemaVersion) && node.SysView().Value().empty(),
            "NativeQuery read schema version differs from the offline catalog");
        Y_ENSURE(read.GetTableStorageType() == Catalog->Storage(*metadata),
            "NativeQuery read storage differs from the offline catalog");
        return *metadata;
    }

    const TTypeAnnotationNode* TypeOf(const TColumnMetadata& column) {
        return ColumnType(Ctx, column);
    }

    TExprContext& TypeContext() {
        return Ctx;
    }

    ui32 TableId(const TTableMetadata& metadata) {
        if (const auto* id = FindId(metadata.Path)) {
            return *id;
        }
        auto* table = Query.add_tables();
        table->set_id(Query.tables_size());
        table->set_cluster(metadata.Cluster);
        table->set_path(metadata.Path);
        table->set_owner_id(metadata.OwnerId);
        table->set_local_path_id(metadata.TableId);
        table->set_schema_version(metadata.SchemaVersion);
        for (const auto& column : metadata.Columns) {
            auto* entry = table->add_columns();
            entry->set_id(column.Id);
            entry->set_name(column.Name);
            entry->set_type_id(TypeId(Query, *TypeOf(column)));
        }
        Ids.emplace(metadata.Path, table->id());
        return table->id();
    }

    const ui32* FindId(const TString& path) const {
        return Ids.FindPtr(path);
    }

private:
    P& Query;
    const TOfflineCatalog* Catalog;
    THashMap<TString, ui32> Ids;
    TExprContext Ctx;
    TParameterTypes ParameterTypes;
    THashSet<TString> ParametersWritten;
};

class TPlanWriter {
public:
    TPlanWriter(P& query, P::Plan& plan, TTableWriter& tables, L::Graph* logical)
        : Query(query)
        , Plan(plan)
        , Tables(tables)
        , Logical(logical)
    {}

    void Write(const TOpRoot& root) {
        const auto& registry = root.PlanProps.InfoUnitRegistry;
        Registry = &registry;
        Y_ENSURE(registry.Size() <= MaxInformationUnits && root.PlanProps.Subplans.Empty()
            && root.PlanProps.StageGraph.Stages().empty() && !root.Node, "Unsupported NativeQuery root");
        THashMap<TString, ui32> generated;
        for (TInfoUnitId id = 0; id < registry.Size(); ++id) {
            const auto& source = registry.Get(id);
            auto* unit = Plan.add_information_units();
            unit->set_id(id + 1);
            if (Logical) {
                Logical->add_information_units()->set_id(id + 1);
            }
            const auto alias = source.GetAlias();
            const auto column = source.GetColumnName();
            unit->set_alias(alias.data(), alias.size());
            unit->set_column_name(column.data(), column.size());
            if (!Logical && registry.IsGenerated(id)) {
                const TString prefix = column.StartsWith("aggregate_argument") ? "aggregate_argument" : "aggregate";
                Y_ENSURE(alias.empty() && column == TStringBuilder() << prefix << ++generated[prefix],
                    "Unsupported generated NativeQuery IU: " << column);
                unit->set_generated_name_prefix(prefix);
            }
        }
        Plan.set_input_operator_id(WriteOperator(*root.GetInput(), 0));
    }

private:
    bool Define(TInfoUnitId id, const TTypeAnnotationNode* type) {
        if (Logical) {
            Logical->mutable_information_units(id)->set_defined_type_id(TypeId(Query, *type));
        }
        return UnitTypes.emplace(id, type).second;
    }

    ui32 WriteExpression(const TExprNode& expression, size_t depth) {
        Y_ENSURE(depth < MaxExpressionDepth, "NativeQuery expression nesting limit exceeded");
        if (const auto* existing = ExpressionIds.FindPtr(&expression)) {
            return *existing;
        }
        P::Expression out;
        const auto* type = expression.GetTypeAnn().Get();
        if (type && type->GetKind() == ETypeAnnotationKind::Type) {
            out.set_type_literal(TypeId(Query, *type->Cast<TTypeExprType>()->GetType()));
        } else if (expression.IsAtom()) {
            auto* atom = out.mutable_atom();
            const auto text = expression.Content();
            atom->set_content(text.data(), text.size());
            atom->set_flags(expression.Flags());
        } else if (expression.IsArgument()) {
            const auto name = expression.Content();
            out.mutable_argument()->set_debug_name(name.data(), name.size());
        } else if (expression.Type() == TExprNode::Arguments) {
            Y_ENSURE(expression.ChildrenSize() == 1 && expression.Head().IsArgument(), "Unsupported NativeQuery lambda parameters");
            out.mutable_arguments()->add_arguments(WriteExpression(expression.Head(), depth + 1));
        } else if (expression.IsLambda()) {
            Y_ENSURE(expression.ChildrenSize() == 2 && expression.Head().Type() == TExprNode::Arguments,
                "Unsupported NativeQuery lambda");
            auto* lambda = out.mutable_lambda();
            lambda->set_arguments_expr_id(WriteExpression(expression.Head(), depth + 1));
            lambda->add_body(WriteExpression(expression.Tail(), depth + 1));
        } else if (expression.IsCallable("Member")) {
            Y_ENSURE(expression.ChildrenSize() == 2 && expression.Head().IsArgument() && expression.Tail().IsAtom(),
                "Unsupported NativeQuery Member");
            auto* column = out.mutable_column();
            column->set_row_argument_expr_id(WriteExpression(expression.Head(), depth + 1));
            const auto id = GetMemberId(expression);
            Y_ENSURE(id < static_cast<size_t>(Plan.information_units_size()), "Unknown NativeQuery member");
            column->set_iu_id(id + 1);
        } else if (expression.IsCallable("Parameter")) {
            const auto* bound = Tables.WriteParameter(expression);
            Y_ENSURE(!type || SameWireType(*type, *bound), "Invalid NativeQuery Parameter annotation");
            out.set_parameter(CopyString(expression.Head().Content()));
        } else if (expression.IsList()) {
            auto* list = out.mutable_list();
            for (const auto& child : expression.Children()) {
                list->add_items(WriteExpression(*child, depth + 1));
            }
        } else if (expression.IsCallable()) {
            const auto name = expression.Content();
            Y_ENSURE(SupportedScalar(name), "Unsupported NativeQuery scalar callable: " << name);
            if (const auto slot = LiteralSlot(name)) {
                Y_ENSURE(expression.ChildrenSize() == 1 && expression.Head().IsAtom()
                    && ValidLiteral(*slot, expression.Head().Content()), "Unsupported NativeQuery literal");
            }
            auto* call = out.mutable_call();
            call->set_function_id(FunctionId(Query, name));
            for (const auto& child : expression.Children()) {
                call->add_arguments(WriteExpression(*child, depth + 1));
            }
        } else {
            ythrow yexception() << "Unsupported NativeQuery expression";
        }
        if (type) {
            Y_ENSURE((out.has_atom() && type->GetKind() == ETypeAnnotationKind::Unit)
                || out.has_column() || out.has_call() || out.has_list() || out.has_parameter() || out.has_type_literal(),
                "Unsupported NativeQuery expression annotation");
            out.set_type_id(TypeId(Query, *type));
        }
        Y_ENSURE(static_cast<size_t>(Plan.expressions_size()) < MaxExpressions, "NativeQuery expression limit exceeded");
        const ui32 id = Plan.expressions_size() + 1;
        out.set_id(id);
        *Plan.add_expressions() = std::move(out);
        ExpressionIds.emplace(&expression, id);
        return id;
    }

    ui32 WriteOperator(const IOperator& op, size_t depth) {
        Y_ENSURE(depth < MaxOperatorDepth && Operators.size() < MaxNodes, "NativeQuery operator limit exceeded");
        Y_ENSURE(Operators.insert(&op).second, "NativeQuery operator has multiple consumers");
        CheckLogicalOperator(op);
        P::Operator out;
        switch (op.Kind) {
            case EOperator::EmptySource: {
                const auto& source = static_cast<const TOpEmptySource&>(op);
                Y_ENSURE(!source.Input && source.GetChildCount() == 0, "Unsupported rows on NativeQuery empty source");
                out.mutable_empty_source();
                break;
            }
            case EOperator::Source: {
                const auto& read = static_cast<const TOpRead&>(op);
                Y_ENSURE(!read.OlapFilterLambda && !read.Limit && !read.OriginalPredicate && !read.RangeInfo
                    && read.SortDir == ESortDir::None && !read.RequireSortedRead && read.GetChildCount() == 0,
                    "Unsupported NativeQuery read shape; expected a full scan");
                const auto& metadata = Tables.Resolve(read);
                auto* node = out.mutable_read();
                node->set_table_id(Tables.TableId(metadata));
                node->set_alias(read.Alias);
                THashMap<TString, const TColumnMetadata*> columns;
                for (const auto& column : metadata.Columns) {
                    columns.emplace(column.Name, &column);
                }
                Y_ENSURE(Logical || read.GetColumns().Size() == metadata.Columns.size(), "Expected the complete NativeQuery scan schema");
                THashSet<ui32> seen;
                for (const auto id : read.GetColumns()) {
                    const auto& unit = Registry->Get(id);
                    const auto* metadataColumn = columns.FindPtr(unit.GetColumnName());
                    Y_ENSURE(metadataColumn && unit.GetAlias() == read.Alias && seen.insert((*metadataColumn)->Id).second
                        && Define(id, Tables.TypeOf(**metadataColumn)), "Invalid NativeQuery read binding");
                    auto* column = node->add_columns();
                    column->set_column_id((*metadataColumn)->Id);
                    column->set_output_iu_id(id + 1);
                }
                break;
            }
            case EOperator::Map: {
                const auto& map = static_cast<const TOpMap&>(op);
                Y_ENSURE(!map.NeedToPush && map.GetChildCount() == 1 && map.GetInput(), "Unsupported NativeQuery Map");
                auto* node = out.mutable_map();
                node->set_input(WriteOperator(*map.GetInput(), depth + 1));
                for (const auto& [id, definition] : map.GetMapElements().Items()) {
                    auto* item = node->add_definitions();
                    item->set_output_iu_id(id + 1);
                    const auto& expression = definition.GetExpression();
                    item->set_lambda_expr_id(WriteExpression(*expression.GetLambda(), 0));
                    const auto body = expression.GetExpressionBody();
                    const TTypeAnnotationNode* type = nullptr;
                    if (body->IsCallable("Member")) {
                        const auto sourceId = GetMemberId(*body);
                        Y_ENSURE(map.GetInput()->GetOutputIUs().Contains(sourceId), "Unavailable NativeQuery Map input IU");
                        type = UnitTypes.at(sourceId);
                    } else if (body->IsCallable()) {
                        if (body->IsCallable("Parameter")) {
                            type = Tables.WriteParameter(*body);
                        } else if (const auto slot = LiteralSlot(body->Content())) {
                            type = Tables.TypeContext().MakeType<TDataExprType>(*slot);
                        } else {
                            Y_ENSURE(expression.Ctx, "Missing NativeQuery expression context");
                            type = AnnotateScalarLambda(*expression.GetLambda(), *expression.Ctx, Tables.TypeContext(),
                                AnnotationContext, UnitTypes, *map.GetInput());
                        }
                    }
                    Y_ENSURE(type, "NativeQuery Map expression has no type");
                    Y_ENSURE(Define(id, type), "Duplicate NativeQuery IU definition");
                }
                break;
            }
            case EOperator::Filter: {
                const auto& filter = static_cast<const TOpFilter&>(op);
                Y_ENSURE(!filter.PartiallyPushedDown && filter.GetChildCount() == 1 && filter.GetInput(), "Unsupported NativeQuery Filter");
                auto* node = out.mutable_filter();
                node->set_input(WriteOperator(*filter.GetInput(), depth + 1));
                node->set_predicate_lambda_expr_id(WriteExpression(*filter.GetFilterExpression().GetLambda(), 0));
                break;
            }
            case EOperator::Aggregate: {
                const auto& aggregate = static_cast<const TOpAggregate&>(op);
                const auto& input = aggregate.GetInput();
                const auto& traits = aggregate.GetAggregationTraits().Items();
                Y_ENSURE(input && aggregate.GetAggregationPhase() == EOpPhase::Undefined && !aggregate.IsDistinctAll()
                    && !aggregate.IsPushedToOlap() && aggregate.GetKeyColumns().Items().empty() && !traits.empty(),
                    "NativeQuery supports logical COUNT without grouping keys");
                auto* node = out.mutable_aggregate();
                node->set_input(WriteOperator(*input, depth + 1));
                for (const auto& [id, trait] : traits) {
                    Y_ENSURE(trait.AggFunction == "count" && !trait.Unwrap, "Unsupported NativeQuery aggregate function");
                    Y_ENSURE(input->GetOutputIUs().Contains(trait.Input) && IsCountInput(*UnitTypes.at(trait.Input), trait.Distinct),
                        "Unsupported NativeQuery COUNT input");
                    auto* binding = node->add_aggregations();
                    binding->set_output_iu_id(id + 1);
                    binding->set_input_iu_id(trait.Input + 1);
                    binding->set_function_id(FunctionId(Query, trait.AggFunction, P::Function::AGGREGATE));
                    binding->set_distinct(trait.Distinct);
                    const auto* type = Tables.TypeContext().MakeType<TDataExprType>(EDataSlot::Uint64);
                    Y_ENSURE(Define(id, type), "Duplicate NativeQuery COUNT output IU");
                }
                break;
            }
            case EOperator::Join: {
                const auto& join = static_cast<const TOpJoin&>(op);
                const auto& left = join.GetLeftInput();
                const auto& right = join.GetRightInput();
                Y_ENSURE(left && right, "Missing NativeQuery JOIN input");
                const auto& keys = join.JoinKeys.Items();
                const auto kind = WireJoinKind(join.JoinKind);
                Y_ENSURE(join.JoinFilters.empty() && ((kind == P::Join::CROSS) == keys.empty()),
                    "NativeQuery JOIN requires ordinary equi-keys or a CROSS join");
                Y_ENSURE(!left->GetOutputIUs().HasAny(right->GetOutputIUs()), "Overlapping NativeQuery JOIN input bindings");
                auto* node = out.mutable_join();
                node->set_left(WriteOperator(*left, depth + 1));
                node->set_right(WriteOperator(*right, depth + 1));
                node->set_kind(kind);
                for (const auto& key : keys) {
                    Y_ENSURE(!key.EqualNulls && left->GetOutputIUs().Contains(key.first) && right->GetOutputIUs().Contains(key.second),
                        "Invalid NativeQuery JOIN key binding");
                    Y_ENSURE(IsSupportedJoinKeyType(*UnitTypes.at(key.first), *UnitTypes.at(key.second)),
                        "NativeQuery JOIN keys require matching comparable data item types");
                    auto* binding = node->add_keys();
                    binding->set_left_iu_id(key.first + 1);
                    binding->set_right_iu_id(key.second + 1);
                }
                if (kind == P::Join::LEFT) {
                    NullExtend(Tables.TypeContext(), UnitTypes, right->GetOutputIUs());
                }
                break;
            }
            case EOperator::Replicate: {
                const auto& port = static_cast<const TOpReplicate&>(op);
                Y_ENSURE(port.IsPrimary() && port.GetInput(), "NativeQuery supports a single primary port per Replicate hub");
                const auto input = WriteOperator(*port.GetInput(), depth + 1);
                auto* hub = Plan.add_replicate_hubs();
                hub->set_id(Plan.replicate_hubs_size());
                hub->set_input_operator_id(input);
                out.mutable_replicate_port()->set_hub_id(hub->id());
                break;
            }
            default:
                ythrow yexception() << "Unsupported NativeQuery operator: " << op.GetExplainName();
        }
        const ui32 id = Plan.operators_size() + 1;
        out.set_id(id);
        *Plan.add_operators() = std::move(out);
        return id;
    }

    P& Query;
    P::Plan& Plan;
    TTableWriter& Tables;
    L::Graph* Logical;
    const TInfoUnitRegistry* Registry = nullptr;
    THashSet<const IOperator*> Operators;
    TUnitTypes UnitTypes;
    TTypeAnnotationContextPtr AnnotationContext;
    THashMap<const TExprNode*, ui32> ExpressionIds;
};

struct TBoundColumn {
    const TColumnMetadata* Metadata;
    const TTypeAnnotationNode* Type;
};

struct TBoundTable {
    NNewYql::TTableMetadataPtr Metadata;
    THashMap<ui32, TBoundColumn> Columns;
};

TVector<TBoundTable> BindTables(const P& query, const TOfflineCatalog* catalog, TExprContext& ctx,
    const TVector<const TTypeAnnotationNode*>& types)
{
    TVector<TBoundTable> tables;
    THashSet<TString> paths;
    Y_ENSURE(static_cast<size_t>(query.tables_size()) <= MaxNodes && (!query.tables_size() || catalog), "NativeQuery tables require an offline catalog");
    for (const auto& wire : query.tables()) {
        CheckFields(wire);
        Y_ENSURE(wire.id() == tables.size() + 1 && paths.insert(wire.path()).second, "Invalid NativeQuery table reference ID or path");
        auto metadata = catalog->Resolve(wire.cluster(), wire.path());
        Y_ENSURE(metadata->OwnerId == wire.owner_id() && metadata->TableId == wire.local_path_id()
            && metadata->SchemaVersion == wire.schema_version() && static_cast<size_t>(wire.columns_size()) == metadata->Columns.size(),
            "NativeQuery table identity or schema changed");
        auto& table = tables.emplace_back(TBoundTable{std::move(metadata), {}});
        for (int i = 0; i < wire.columns_size(); ++i) {
            const auto& column = wire.columns(i);
            CheckFields(column);
            const auto& canonical = table.Metadata->Columns[i];
            const auto* type = ColumnType(ctx, canonical);
            Y_ENSURE(canonical.Id == column.id() && canonical.Name == column.name()
                && column.type_id() > 0 && column.type_id() <= types.size()
                && IsSameAnnotation(*type, *types[column.type_id() - 1]), "NativeQuery catalog column changed");
            table.Columns.emplace(canonical.Id, TBoundColumn{&canonical, type});
        }
    }
    return tables;
}

struct TExpressionEntry {
    const P::Expression* Wire = nullptr;
    TExprNode::TPtr Node;
    size_t Depth = 0;
    bool Used = false;
};

struct TOperatorEntry {
    TIntrusivePtr<IOperator> Node;
    size_t Depth;
};

struct TNativeReadBindings {
    const TVector<TBoundTable>& Tables;
    THashSet<ui32>& ReadTables;
};

struct TLogicalReadBindings {
    const L::Graph& Graph;
    THashSet<TString>& ReadPaths;
};

template <class TBindings>
class TPlanReader {
public:
    TPlanReader(TExprContext& ctx, const TVector<const TTypeAnnotationNode*>& types, const TVector<const P::Function*>& functions,
        const TParameterTypes& parameters, THashSet<TString>& usedParameters, const TOfflineCatalog* catalog,
        TBindings bindings)
        : Ctx(ctx)
        , Types(types)
        , Functions(functions)
        , Parameters(parameters)
        , UsedParameters(usedParameters)
        , Catalog(catalog)
        , Bindings(bindings)
    {}

    template <class TRoot, class TGraph>
    TIntrusivePtr<TOpRoot> Read(const TRoot& root, const TGraph& plan, ui32 position, TVector<const TTypeAnnotationNode*>* resultTypes) {
        CheckFields(plan);
        Y_ENSURE(plan.replicate_hubs_size() <= plan.operators_size()
            && static_cast<size_t>(plan.information_units_size()) <= MaxInformationUnits
            && static_cast<size_t>(plan.expressions_size()) <= MaxExpressions
            && static_cast<size_t>(plan.operators_size()) <= MaxNodes && !plan.operators().empty(),
            "Unsupported plan size or features");
        ReadUnits(plan);
        for (const auto& expression : plan.expressions()) {
            ReadExpression(expression);
        }
        ReplicateHubs.reserve(plan.replicate_hubs_size());
        for (const auto& hub : plan.replicate_hubs()) {
            CheckFields(hub);
            Y_ENSURE(hub.id() == ReplicateHubs.size() + 1 && !hub.has_location(), "Invalid NativeQuery Replicate hub");
            ReplicateHubs.push_back(&hub);
        }
        Operators.reserve(plan.operators_size());
        for (const auto& op : plan.operators()) {
            Y_ENSURE(op.id() == Operators.size() + 1, "Invalid NativeQuery operator order");
            size_t depth = 1;
            auto decoded = ReadOperator(op, depth);
            Y_ENSURE(depth <= MaxOperatorDepth, "NativeQuery operator nesting limit exceeded");
            decoded->InitializeOutputIUs();
            Operators.push_back({std::move(decoded), depth});
        }
        auto input = TakeOperator(plan.input_operator_id()).Node;
        Y_ENSURE(std::ranges::none_of(Operators, [](const TOperatorEntry& op) { return bool(op.Node); })
            && std::ranges::none_of(ReplicateHubs, [](const auto* hub) { return hub != nullptr; })
            && UnitTypes.size() == Props.InfoUnitRegistry.Size(), "Unused NativeQuery operator, Replicate hub or IU");
        for (const auto& expression : Expressions) {
            Y_ENSURE(expression.Used, "Unused NativeQuery expression");
        }

        TOrderedIUs<TString> columns;
        for (const auto& column : root.columns()) {
            CheckFields(column);
            const auto id = AvailableUnit(column.iu_id(), *input);
            columns.Append(id, CopyString(column.name()));
            if (resultTypes) {
                resultTypes->push_back(UnitTypes.at(id));
            }
        }
        TVector<TString> queryColumns;
        bool unordered;
        if constexpr (std::is_same_v<TRoot, L::Root>) {
            for (const auto& column : root.columns()) {
                queryColumns.push_back(CopyString(column.name()));
            }
            unordered = !root.ordered_rows();
        } else {
            for (const auto& name : root.query_columns()) {
                queryColumns.push_back(CopyString(name));
            }
            unordered = root.unordered_result();
        }
        auto result = MakeIntrusive<TOpRoot>(std::move(input), TPositionHandle(), std::move(columns), std::move(queryColumns));
        result->PlanProps = std::move(Props);
        result->PlanProps.ResultIndex = position;
        result->PlanProps.UnorderedResult = unordered;
        for (const auto& entry : IterateSubtreeWithSubplans(result.Get(), result->PlanProps)) {
            entry.Current->BindExpressionPlanProps(&result->PlanProps);
        }
        result->ComputeParents();
        return result;
    }

private:
    void ReadUnits(const P::Plan& plan) {
        Y_ENSURE(plan.subplans().empty(), "Unsupported NativeQuery subplans");
        for (const auto& iu : plan.information_units()) {
            CheckFields(iu);
            Y_ENSURE(iu.id() == Props.InfoUnitRegistry.Size() + 1, "Invalid NativeQuery IU registry");
            if (iu.has_generated_name_prefix()) {
                const auto& prefix = iu.generated_name_prefix();
                Y_ENSURE(iu.alias().empty() && IsGeneratedAggregatePrefix(prefix), "Unsupported generated NativeQuery IU");
                const auto id = Props.InfoUnitRegistry.AddGenerated(prefix);
                Y_ENSURE(Props.InfoUnitRegistry.Get(id).GetColumnName() == iu.column_name(),
                    "Generated NativeQuery IU name does not match its canonical prefix and ordinal");
            } else {
                Props.InfoUnitRegistry.Add(TInfoUnit(CopyString(iu.alias()), CopyString(iu.column_name())));
            }
        }
    }

    void ReadUnits(const L::Graph& graph) {
        Y_ENSURE(graph.subqueries().empty(), "Unsupported LogicalPlan subqueries");
        TVector<TString> names(graph.information_units_size());
        for (const auto& op : graph.operators()) {
            for (const auto& column : op.read().columns()) {
                Y_ENSURE(column.iu_id() && column.iu_id() <= names.size(), "Unknown LogicalPlan scan IU");
                names[column.iu_id() - 1] = CopyString(column.column());
            }
        }
        for (const auto& unit : graph.information_units()) {
            CheckFields(unit);
            Y_ENSURE(unit.id() == Props.InfoUnitRegistry.Size() + 1 && unit.defined_type_id() && unit.defined_type_id() <= Types.size(),
                "Invalid LogicalPlan IU definition");
            const auto& name = names[unit.id() - 1];
            Props.InfoUnitRegistry.Add(TInfoUnit("", name.empty() ? ToString(unit.id()) : name));
        }
    }

    bool Define(TInfoUnitId id, const TTypeAnnotationNode* type) {
        if constexpr (std::is_same_v<TBindings, TLogicalReadBindings>) {
            const auto declared = Bindings.Graph.information_units(id).defined_type_id();
            Y_ENSURE(declared && declared <= Types.size() && IsSameAnnotation(*type, *Types[declared - 1]),
                "LogicalPlan IU definition type mismatch");
        }
        return UnitTypes.emplace(id, type).second;
    }

    TExpressionEntry& Reference(ui32 id, P::Expression::NodeCase kind) {
        Y_ENSURE(id > 0 && id <= Expressions.size(), "Invalid or forward NativeQuery expression reference");
        auto& entry = Expressions[id - 1];
        Y_ENSURE(entry.Wire->node_case() == kind, "Wrong NativeQuery expression node kind");
        entry.Used = true;
        return entry;
    }

    TExpressionEntry& ScalarReference(ui32 id) {
        Y_ENSURE(id > 0 && id <= Expressions.size(), "Invalid NativeQuery scalar reference");
        const auto& wire = *Expressions[id - 1].Wire;
        Y_ENSURE(wire.has_atom() || wire.has_list() || wire.has_call() || wire.has_column()
            || wire.has_type_literal() || wire.has_parameter(), "Invalid NativeQuery scalar kind");
        return Reference(id, wire.node_case());
    }

    void ReadExpression(const P::Expression& expression) {
        CheckFields(expression);
        Y_ENSURE(expression.id() == Expressions.size() + 1 && !expression.has_location(),
            "Invalid NativeQuery expression order or location");
        TPositionHandle pos;
        TExprNode::TPtr node;
        size_t depth = 1;
        const auto takeScalar = [this, &depth](ui32 id) {
            auto& entry = ScalarReference(id);
            depth = std::max(depth, 1 + entry.Depth);
            return entry.Node;
        };
        switch (expression.node_case()) {
            case P::Expression::kAtom: {
                const auto& atom = expression.atom();
                CheckFields(atom);
                Y_ENSURE((atom.flags() & ~TNodeFlags::FlagsMask) == 0, "Unsupported NativeQuery atom flags");
                node = Ctx.NewAtom(pos, CopyString(atom.content()), atom.flags());
                break;
            }
            case P::Expression::kArgument: {
                CheckFields(expression.argument());
                node = Ctx.NewArgument(pos, CopyString(expression.argument().debug_name()));
                break;
            }
            case P::Expression::kArguments: {
                const auto& args = expression.arguments();
                CheckFields(args);
                Y_ENSURE(args.arguments_size() == 1 && OwnedArguments.insert(args.arguments(0)).second,
                    "Invalid NativeQuery argument ownership");
                node = Ctx.NewArguments(pos, {Reference(args.arguments(0), P::Expression::kArgument).Node});
                break;
            }
            case P::Expression::kLambda: {
                const auto& lambda = expression.lambda();
                CheckFields(lambda);
                Y_ENSURE(lambda.body_size() == 1 && OwnedArgumentLists.insert(lambda.arguments_expr_id()).second,
                    "Invalid NativeQuery lambda ownership");
                auto args = Reference(lambda.arguments_expr_id(), P::Expression::kArguments).Node;
                auto bodyNode = takeScalar(lambda.body(0));
                node = Ctx.NewLambda(pos, std::move(args), std::move(bodyNode));
                break;
            }
            case P::Expression::kCall: {
                const auto& call = expression.call();
                CheckFields(call);
                Y_ENSURE(call.function_id() > 0 && call.function_id() <= Functions.size() && call.arguments_size() <= 256,
                    "Invalid NativeQuery scalar callable");
                const auto& function = *Functions[call.function_id() - 1];
                Y_ENSURE(function.kind() == P::Function::YQL_CALLABLE, "Unsupported NativeQuery scalar function kind");
                const auto& name = function.name();
                if (const auto slot = LiteralSlot(name)) {
                    Y_ENSURE(call.arguments_size() == 1, "Invalid NativeQuery literal arity");
                    auto atom = Reference(call.arguments(0), P::Expression::kAtom).Node;
                    Y_ENSURE(ValidLiteral(*slot, atom->Content()), "Invalid NativeQuery literal value");
                }
                TExprNode::TListType args;
                for (const auto id : call.arguments()) {
                    args.push_back(takeScalar(id));
                }
                node = Ctx.NewCallable(pos, name, std::move(args));
                break;
            }
            case P::Expression::kList: {
                const auto& list = expression.list();
                CheckFields(list);
                Y_ENSURE(list.items_size() <= 256, "NativeQuery expression list limit exceeded");
                TExprNode::TListType items;
                for (const auto id : list.items()) {
                    items.push_back(takeScalar(id));
                }
                node = Ctx.NewList(pos, std::move(items));
                break;
            }
            case P::Expression::kTypeLiteral: {
                const auto id = expression.type_literal();
                Y_ENSURE(id > 0 && id <= Types.size() && expression.has_type_id(), "Invalid NativeQuery type literal");
                node = ExpandType(pos, *Types[id - 1], Ctx);
                break;
            }
            case P::Expression::kParameter: {
                const auto name = CopyString(expression.parameter());
                const auto* type = Parameters.FindPtr(name);
                Y_ENSURE(type, "Unknown NativeQuery parameter");
                UsedParameters.insert(name);
                auto expandedType = ExpandType(pos, **type, Ctx);
                expandedType->SetTypeAnn(Ctx.MakeType<TTypeExprType>(*type));
                node = Ctx.NewCallable(pos, "Parameter", {Ctx.NewAtom(pos, name), std::move(expandedType)});
                break;
            }
            case P::Expression::kColumn: {
                const auto& column = expression.column();
                CheckFields(column);
                Y_ENSURE(column.iu_id() > 0 && column.iu_id() <= Props.InfoUnitRegistry.Size(),
                    "Invalid NativeQuery column IU");
                auto argument = Reference(column.row_argument_expr_id(), P::Expression::kArgument).Node;
                node = Ctx.NewCallable(pos, "Member", {std::move(argument), Ctx.NewAtom(pos, column.iu_id() - 1)});
                break;
            }
            default:
                ythrow yexception() << "Unsupported NativeQuery expression kind";
        }
        if (expression.has_type_id()) {
            Y_ENSURE(expression.type_id() > 0 && expression.type_id() <= Types.size(), "Invalid NativeQuery expression annotation");
            const auto* type = Types[expression.type_id() - 1];
            Y_ENSURE((expression.has_atom() && type->GetKind() == ETypeAnnotationKind::Unit)
                || expression.has_column() || expression.has_call() || expression.has_list()
                || expression.has_parameter() || expression.has_type_literal(), "Invalid NativeQuery expression type");
            if (expression.has_type_literal()) {
                Y_ENSURE(type->GetKind() == ETypeAnnotationKind::Type
                    && IsSameAnnotation(*type->Cast<TTypeExprType>()->GetType(), *Types[expression.type_literal() - 1]),
                    "Invalid NativeQuery type literal annotation");
            }
            if (expression.has_parameter()) {
                Y_ENSURE(IsSameAnnotation(*type, *Parameters.at(expression.parameter())), "Invalid NativeQuery parameter annotation");
            }
            node->SetTypeAnn(type);
        }
        Y_ENSURE(depth <= MaxExpressionDepth, "NativeQuery expression nesting limit exceeded");
        Expressions.push_back({&expression, std::move(node), depth, false});
    }

    TInfoUnitId AvailableUnit(ui32 wireId, IOperator& input) const {
        Y_ENSURE(wireId > 0 && UnitTypes.contains(wireId - 1)
            && input.GetOutputIUs().Contains(wireId - 1), "Unavailable NativeQuery input IU");
        return wireId - 1;
    }

    const TTypeAnnotationNode* LambdaType(const TExpressionEntry& lambda, IOperator& input) {
        const auto& body = Expressions[lambda.Wire->lambda().body(0) - 1];
        const auto& row = lambda.Node->Head().Head();
        const auto columnType = [&](const TExprNode& member) {
            Y_ENSURE(&member.Head() == &row, "Out-of-scope NativeQuery lambda argument");
            const auto* type = UnitTypes.at(AvailableUnit(GetMemberId(member) + 1, input));
            CheckExpressionType(member, *type);
            return type;
        };
        const auto& node = *body.Node;
        if (node.IsCallable("Member")) {
            return columnType(node);
        }
        if (node.IsCallable("Parameter")) {
            return Parameters.at(TString(node.Head().Content()));
        }
        Y_ENSURE(node.IsCallable(), "Unsupported NativeQuery lambda result");
        if (const auto slot = LiteralSlot(node.Content())) {
            const auto* type = Ctx.MakeType<TDataExprType>(*slot);
            CheckExpressionType(node, *type);
            return type;
        }

        return AnnotateScalarLambda(*lambda.Node, Ctx, Ctx, AnnotationContext, UnitTypes, input);
    }

    TIntrusivePtr<IOperator> ReadTable(const P::Read& read, TPositionHandle pos) {
        CheckFields(read);
        Y_ENSURE(Catalog && read.table_id() > 0 && read.table_id() <= Bindings.Tables.size(), "Unknown NativeQuery read table");
        const auto& table = Bindings.Tables[read.table_id() - 1];
        const auto alias = CopyString(read.alias());
        Y_ENSURE(static_cast<size_t>(read.columns_size()) == table.Columns.size(), "Expected the complete NativeQuery scan schema");
        TUnorderedIUs outputs;
        THashSet<ui32> seen;
        for (const auto& binding : read.columns()) {
            CheckFields(binding);
            const auto* column = table.Columns.FindPtr(binding.column_id());
            Y_ENSURE(column && seen.insert(binding.column_id()).second && binding.output_iu_id() > 0
                && binding.output_iu_id() <= Props.InfoUnitRegistry.Size(), "Invalid NativeQuery scan column ID");
            const auto id = binding.output_iu_id() - 1;
            const auto& unit = Props.InfoUnitRegistry.Get(id);
            Y_ENSURE(unit.GetAlias() == alias && unit.GetColumnName() == column->Metadata->Name
                && Define(id, column->Type), "Invalid NativeQuery scan binding");
            outputs.Add(id);
        }
        Bindings.ReadTables.insert(read.table_id());
        const auto bound = Catalog->Lookup(Ctx, *table.Metadata);
        return MakeIntrusive<TOpRead>(alias, std::move(outputs), bound.StorageType, bound.Table,
            nullptr, nullptr, std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
    }

    TIntrusivePtr<IOperator> ReadTable(const L::Read& read, TPositionHandle pos) {
        CheckFields(read);
        Y_ENSURE(Catalog, "LogicalPlan reads require a catalog");
        const auto metadata = Catalog->Resolve(Catalog->NameSettings().DefaultCluster, CopyString(read.table_path()));
        THashMap<TString, const TColumnMetadata*> columns;
        for (const auto& column : metadata->Columns) {
            columns.emplace(column.Name, &column);
        }
        TUnorderedIUs outputs;
        for (const auto& binding : read.columns()) {
            CheckFields(binding);
            const auto it = columns.find(binding.column());
            Y_ENSURE(it != columns.end() && binding.iu_id() && binding.iu_id() <= Props.InfoUnitRegistry.Size(),
                "Invalid LogicalPlan scan binding");
            const auto id = binding.iu_id() - 1;
            Y_ENSURE(Define(id, ColumnType(Ctx, *it->second)), "Duplicate LogicalPlan scan IU");
            outputs.Add(id);
            columns.erase(it);
        }
        Bindings.ReadPaths.insert(metadata->Path);
        const auto bound = Catalog->Lookup(Ctx, *metadata);
        return MakeIntrusive<TOpRead>("", std::move(outputs), bound.StorageType, bound.Table,
            nullptr, nullptr, std::nullopt, std::nullopt, ESortDir::None, TPhysicalOpProps{}, pos);
    }

    TOperatorEntry TakeOperator(ui32 id) {
        Y_ENSURE(id > 0 && id <= Operators.size() && Operators[id - 1].Node,
            "Invalid, forward or reused NativeQuery operator reference");
        return std::move(Operators[id - 1]);
    }

    static const P::EmptySource* Rows(const P::Operator& op) {
        return op.has_empty_source() ? &op.empty_source() : nullptr;
    }

    static const P::EmptySource* Rows(const L::Operator& op) {
        return op.has_rows() ? &op.rows() : nullptr;
    }

    template <class TOperator>
    TIntrusivePtr<IOperator> ReadOperator(const TOperator& op, size_t& depth) {
        CheckFields(op);
        if constexpr (std::is_same_v<TOperator, P::Operator>) {
            Y_ENSURE(!op.has_location(), "Unsupported NativeQuery operator location");
        }
        TPositionHandle pos;
        if (const auto* rows = Rows(op)) {
            CheckFields(*rows);
            Y_ENSURE(!rows->has_rows_expr_id() && rows->columns().empty(), "Unsupported non-unit rows source");
            return MakeIntrusive<TOpEmptySource>(pos);
        }
        switch (op.node_case()) {
            case TOperator::kRead:
                return ReadTable(op.read(), pos);
            case TOperator::kMap: {
                const auto& map = op.map();
                CheckFields(map);
                Y_ENSURE(!map.definitions().empty() && static_cast<size_t>(map.definitions_size()) <= Props.InfoUnitRegistry.Size(),
                    "Unsupported NativeQuery Map");
                auto [input, childDepth] = TakeOperator(map.input());
                depth = childDepth + 1;
                TMapIUs definitions;
                for (const auto& definition : map.definitions()) {
                    CheckFields(definition);
                    const auto id = definition.output_iu_id();
                    Y_ENSURE(id > 0 && id <= Props.InfoUnitRegistry.Size(), "Invalid NativeQuery map output IU");
                    const auto& lambda = Reference(definition.lambda_expr_id(), P::Expression::kLambda);
                    const auto* type = LambdaType(lambda, *input);
                    Y_ENSURE(Define(id - 1, type), "Duplicate NativeQuery IU definition");
                    definitions.Add(id - 1, TExpression(lambda.Node, &Ctx));
                }
                return MakeIntrusive<TOpMap>(std::move(input), pos, std::move(definitions));
            }
            case TOperator::kFilter: {
                const auto& filter = op.filter();
                CheckFields(filter);
                auto [input, childDepth] = TakeOperator(filter.input());
                depth = childDepth + 1;
                const auto& lambda = Reference(filter.predicate_lambda_expr_id(), P::Expression::kLambda);
                const TTypeAnnotationNode& item = OptionalItemType(*LambdaType(lambda, *input));
                Y_ENSURE(item.GetKind() == ETypeAnnotationKind::Data && item.Cast<TDataExprType>()->GetSlot() == EDataSlot::Bool,
                    "Non-boolean NativeQuery predicate");
                return MakeIntrusive<TOpFilter>(std::move(input), pos, TExpression(lambda.Node, &Ctx));
            }
            case TOperator::kAggregate: {
                const auto& aggregate = op.aggregate();
                CheckFields(aggregate);
                Y_ENSURE(aggregate.key_iu_ids().empty() && !aggregate.distinct_all() && !aggregate.aggregations().empty()
                    && static_cast<size_t>(aggregate.aggregations_size()) <= Props.InfoUnitRegistry.Size(),
                    "NativeQuery supports logical COUNT without grouping keys");
                auto [input, childDepth] = TakeOperator(aggregate.input());
                depth = childDepth + 1;
                TAggregationIUs traits;
                const auto* type = Ctx.MakeType<TDataExprType>(EDataSlot::Uint64);
                for (const auto& item : aggregate.aggregations()) {
                    CheckFields(item);
                    const auto id = item.output_iu_id();
                    const auto function = item.function_id();
                    Y_ENSURE(id > 0 && id <= Props.InfoUnitRegistry.Size() && function > 0 && function <= Functions.size()
                        && Functions[function - 1]->kind() == P::Function::AGGREGATE && !item.unwrap(),
                        "Unsupported NativeQuery aggregate function or output IU");
                    const auto inputId = AvailableUnit(item.input_iu_id(), *input);
                    Y_ENSURE(IsCountInput(*UnitTypes.at(inputId), item.distinct()), "Unsupported NativeQuery COUNT input");
                    Y_ENSURE(Define(id - 1, type), "Duplicate NativeQuery COUNT output IU");
                    traits.Add(id - 1, TOpAggregationTraits{inputId, "count", item.distinct()});
                }
                return MakeIntrusive<TOpAggregate>(std::move(input), std::move(traits), TOrderedIUs<>{},
                    EOpPhase::Undefined, false, pos);
            }
            case TOperator::kJoin: {
                const auto& join = op.join();
                CheckFields(join);
                auto kind = ReadJoinKind(join.kind());
                Y_ENSURE(join.filter_lambda_expr_ids().empty() && static_cast<size_t>(join.keys_size()) <= MaxNodes
                    && ((join.kind() == P::Join::CROSS) == join.keys().empty()),
                    "NativeQuery JOIN requires ordinary equi-keys or a CROSS join");
                auto [left, leftDepth] = TakeOperator(join.left());
                auto [right, rightDepth] = TakeOperator(join.right());
                depth = std::max(leftDepth, rightDepth) + 1;
                Y_ENSURE(!left->GetOutputIUs().HasAny(right->GetOutputIUs()), "Overlapping NativeQuery JOIN input bindings");
                TJoinIUs keys;
                for (const auto& key : join.keys()) {
                    CheckFields(key);
                    const auto leftId = AvailableUnit(key.left_iu_id(), *left);
                    const auto rightId = AvailableUnit(key.right_iu_id(), *right);
                    Y_ENSURE(!key.equal_nulls() && IsSupportedJoinKeyType(*UnitTypes.at(leftId), *UnitTypes.at(rightId)),
                        "NativeQuery JOIN keys require ordinary equality and matching comparable data item types");
                    Y_ENSURE(keys.Add(leftId, rightId), "Duplicate NativeQuery JOIN key");
                }
                if (join.kind() == P::Join::LEFT) {
                    NullExtend(Ctx, UnitTypes, right->GetOutputIUs());
                }
                return MakeIntrusive<TOpJoin>(std::move(left), std::move(right), pos, std::move(kind), std::move(keys));
            }
            case TOperator::kReplicatePort: {
                const auto& port = op.replicate_port();
                CheckFields(port);
                const auto id = port.hub_id();
                Y_ENSURE(port.port_index() == 0 && port.columns().empty() && id > 0 && id <= ReplicateHubs.size()
                    && ReplicateHubs[id - 1], "NativeQuery supports a single primary port per Replicate hub");
                const auto* hub = ReplicateHubs[id - 1];
                ReplicateHubs[id - 1] = nullptr;
                auto [input, childDepth] = TakeOperator(hub->input_operator_id());
                depth = childDepth + 1;
                return TReplicate::Create(std::move(input), pos, Props.InfoUnitRegistry)->AddOutput();
            }
            default:
                ythrow yexception() << "Unsupported NativeQuery operator kind";
        }
    }

    TExprContext& Ctx;
    const TVector<const TTypeAnnotationNode*>& Types;
    const TVector<const P::Function*>& Functions;
    const TParameterTypes& Parameters;
    THashSet<TString>& UsedParameters;
    const TOfflineCatalog* Catalog;
    TBindings Bindings;
    TPlanProps Props;
    TVector<TExpressionEntry> Expressions;
    TVector<TOperatorEntry> Operators;
    TVector<const P::ReplicateHub*> ReplicateHubs;
    THashSet<ui32> OwnedArguments;
    THashSet<ui32> OwnedArgumentLists;
    TUnitTypes UnitTypes;
    TTypeAnnotationContextPtr AnnotationContext;
};

P WriteNative(const TNativeQuery& query, const TNativeSourceOrdinals& source, const TOfflineCatalog* catalog,
    const TNativeParameters* parameters, L* logical)
{
    Y_ENSURE(!query.Roots.empty() && query.Roots.size() <= MaxNodes && query.Roots.size() == query.Results.size()
        && query.Roots.size() == query.Execution.DiscardResults.size()
        && !query.Execution.HasUncommittedChangesRead, "Unsupported NativeQuery execution contract");
    Y_ENSURE(source.Statements.size() == query.Roots.size(), "Missing NativeQuery source statement ordinals");
    P output;
    output.set_format_version(1);
    output.set_semantics_version(SemanticsVersion.data(), SemanticsVersion.size());
    auto* execution = output.mutable_execution();
    TTableWriter tables(output, catalog, parameters);
    for (ui32 i = 0; i < query.Roots.size(); ++i) {
        Y_ENSURE(query.Roots[i], "Missing NativeQuery root");
        const auto& root = *query.Roots[i];
        const auto& props = root.PlanProps;
        CheckLogicalOperator(root);
        Y_ENSURE(root.GetInput() && props.ResultIndex == i && !props.WithEffects && !props.WithReturning
            && !props.MaterializedResult && !props.ReturningRowsLimit && !props.PgSyntax
            && !props.EffectPriority && !props.PhysicalTxGroup, "Unsupported NativeQuery root contract");
        auto* wire = output.add_roots();
        wire->set_id(i + 1);
        Y_ENSURE(i == 0 || source.Statements[i - 1] < source.Statements[i],
            "NativeQuery source statement ordinals must be strictly increasing");
        wire->set_statement_index(source.Statements[i]);
        wire->set_result_id(i + 1);
        wire->set_unordered_result(props.UnorderedResult);
        TPlanWriter(output, *wire->mutable_plan(), tables, logical ? logical->add_roots()->mutable_graph() : nullptr).Write(root);
        for (const auto& [id, name] : root.GetColumns().Items()) {
            auto* column = wire->add_columns();
            column->set_iu_id(id + 1);
            column->set_name(name.data(), name.size());
        }
        for (const auto& name : root.GetQueryColumns()) {
            wire->add_query_columns(name.data(), name.size());
        }
        const auto& result = query.Results[i];
        auto* resultWire = output.add_results();
        resultWire->set_id(i + 1);
        resultWire->set_rows_limit(result.RowsLimit);
        resultWire->set_discard(query.Execution.DiscardResults[i]);
        for (const auto& name : result.ColumnHints) {
            resultWire->add_column_hints(name.data(), name.size());
        }
    }
    THashSet<ui32> selectedTables;
    for (const auto& operation : query.Execution.TableOperations) {
        const auto* id = tables.FindId(operation.GetTable());
        Y_ENSURE(id && operation.GetOperation() == static_cast<ui32>(TYdbOperation::Select)
            && selectedTables.insert(*id).second, "Unsupported NativeQuery table operation");
        auto* wire = execution->add_table_operations();
        wire->set_table_id(*id);
        wire->set_operation(P::SELECT);
    }
    Y_ENSURE(selectedTables.size() == static_cast<size_t>(output.tables_size()), "Missing NativeQuery SELECT table operations");
    return output;
}

struct TRegistries {
    TVector<const TTypeAnnotationNode*> Types;
    TVector<const P::Function*> Functions;
    TParameterTypes Parameters;
};

template <class TQuery>
TRegistries ReadRegistries(const TQuery& query, TExprContext& ctx, const TNativeParameters* parameters) {
    TRegistries result;
    auto& [types, functions, boundParameters] = result;
    TVector<size_t> typeDepths;
    Y_ENSURE(static_cast<size_t>(query.types_size()) <= MaxNodes && query.functions_size() <= 64,
        "Unsupported NativeQuery function or type registry size");
    for (const auto& type : query.types()) {
        CheckFields(type);
        Y_ENSURE(type.id() == types.size() + 1, "Invalid NativeQuery type ID");
        size_t depth = 1;
        const auto item = [&](ui32 id) {
            Y_ENSURE(id > 0 && id <= types.size(), "Invalid or forward NativeQuery type reference");
            depth = std::max(depth, 1 + typeDepths[id - 1]);
            Y_ENSURE(depth <= MaxExpressionDepth, "NativeQuery type nesting limit exceeded");
            return types[id - 1];
        };
        if (type.has_unit()) {
            CheckFields(type.unit());
            types.push_back(ctx.MakeType<TUnitExprType>());
        } else if (type.has_null_type()) {
            CheckFields(type.null_type());
            types.push_back(ctx.MakeType<TNullExprType>());
        } else if (type.has_optional_item()) {
            types.push_back(ctx.MakeType<TOptionalExprType>(item(type.optional_item())));
        } else if (type.has_list_item()) {
            types.push_back(ctx.MakeType<TListExprType>(item(type.list_item())));
        } else if (type.has_type_of()) {
            types.push_back(ctx.MakeType<TTypeExprType>(item(type.type_of())));
        } else if (type.has_tuple()) {
            CheckFields(type.tuple());
            Y_ENSURE(type.tuple().type_ids_size() <= 256, "NativeQuery tuple size limit exceeded");
            TTypeAnnotationNode::TListType items;
            for (const auto id : type.tuple().type_ids()) {
                items.push_back(item(id));
            }
            types.push_back(ctx.MakeType<TTupleExprType>(items));
        } else {
            Y_ENSURE(type.has_data(), "Unsupported NativeQuery type");
            const auto& data = type.data();
            CheckFields(data);
            Y_ENSURE(data.parameters_size() == (data.slot() == "Decimal" ? 2 : 0), "Unsupported NativeQuery data type parameters");
            types.push_back(data.parameters_size() ? DataType(ctx, data.slot(), data.parameters(0), data.parameters(1))
                : DataType(ctx, data.slot()));
        }
        typeDepths.push_back(depth);
    }
    for (const auto& function : query.functions()) {
        CheckFields(function);
        const bool supported = (function.kind() == P::Function::YQL_CALLABLE && SupportedScalar(function.name()))
            || (function.kind() == P::Function::AGGREGATE && function.name() == "count");
        Y_ENSURE(function.id() == functions.size() + 1 && supported
            && function.module().empty() && function.implementation_version().empty() && !function.has_signature_type_id(),
            "Unsupported NativeQuery function");
        Y_ENSURE(std::ranges::none_of(functions, [&](const auto* known) {
            return known->kind() == function.kind() && known->name() == function.name();
        }), "Duplicate NativeQuery function");
        functions.push_back(&function);
    }

    const auto requestTypes = parameters ? MakeOfflineTypeSettings(*parameters, ctx).Parameters : TParameterTypes{};
    for (const auto& parameter : query.parameters()) {
        CheckFields(parameter);
        const auto name = CopyString(parameter.name());
        const auto id = parameter.type_id();
        const auto* bound = requestTypes.FindPtr(name);
        Y_ENSURE(bound && id > 0 && id <= types.size() && IsSameAnnotation(**bound, *types[id - 1])
            && boundParameters.emplace(name, *bound).second, "NativeQuery parameter type differs from the request");
    }
    return result;
}

} // namespace

std::shared_ptr<TNativeQuery> ImportNativeQuery(const P& query, TExprContext& ctx, const TOfflineCatalog* catalog,
    TNativeResultTypes* resultTypes, const TNativeParameters* parameters)
{
    CheckFields(query);
    Y_ENSURE(query.format_version() == 1 && TStringBuf(query.semantics_version()) == SemanticsVersion,
        "Unsupported NativeQuery format or semantics version");
    Y_ENSURE(query.required_features().empty() && !query.has_context() && !query.has_diagnostics()
        && !query.roots().empty() && static_cast<size_t>(query.parameters_size()) <= MaxInformationUnits
        && static_cast<size_t>(query.roots_size()) <= MaxNodes && query.roots_size() == query.results_size(),
        "Unsupported NativeQuery context or execution shape");
    CheckFields(query.execution());
    Y_ENSURE(!query.execution().has_uncommitted_changes_read(), "Unsupported NativeQuery uncommitted reads");
    const auto [types, functions, boundParameters] = ReadRegistries(query, ctx, parameters);
    THashSet<TString> usedParameters;

    auto output = std::make_shared<TNativeQuery>();
    const auto tables = BindTables(query, catalog, ctx, types);
    THashSet<ui32> readTables;
    TNativeResultTypes outputTypes;
    for (int i = 0; i < query.roots_size(); ++i) {
        const auto& root = query.roots(i);
        const auto& result = query.results(i);
        CheckFields(root);
        CheckFields(result);
        const auto index = static_cast<ui32>(i);
        Y_ENSURE(i == 0 || query.roots(i - 1).statement_index() < root.statement_index(),
            "NativeQuery source statement ordinals must be strictly increasing");
        Y_ENSURE(root.id() == index + 1 && result.id() == index + 1
            && root.has_result_id() && root.result_id() == result.id() && root.has_plan() && !root.has_location()
            && !root.transaction_group() && !root.effect_priority() && !root.pg_expression_semantics()
            && !root.has_returning_rows_limit_expr_id() && root.columns_size() > 0 && static_cast<size_t>(root.columns_size()) <= MaxInformationUnits
            && static_cast<size_t>(root.query_columns_size()) <= MaxInformationUnits && static_cast<size_t>(result.column_hints_size()) <= MaxInformationUnits,
            "Unsupported NativeQuery result contract");
        auto importedRoot = TPlanReader(ctx, types, functions, boundParameters, usedParameters, catalog,
            TNativeReadBindings{tables, readTables}).Read(root, root.plan(), index, resultTypes ? &outputTypes.emplace_back() : nullptr);
        TVector<TString> hints;
        for (const auto& hint : result.column_hints()) {
            hints.push_back(CopyString(hint));
        }
        output->Roots.push_back(std::move(importedRoot));
        output->Results.push_back({std::move(hints), result.rows_limit()});
        output->Execution.DiscardResults.push_back(result.discard());
    }
    Y_ENSURE(usedParameters.size() == boundParameters.size(), "Unused NativeQuery parameter declaration");
    Y_ENSURE(readTables.size() == tables.size() && static_cast<size_t>(query.execution().table_operations_size()) == tables.size(),
        "NativeQuery table declarations and operations must match the reads");
    THashSet<ui32> selectedTables;
    for (const auto& operation : query.execution().table_operations()) {
        CheckFields(operation);
        const auto id = operation.table_id();
        Y_ENSURE(operation.operation() == P::SELECT && readTables.contains(id) && selectedTables.insert(id).second,
            "Unsupported NativeQuery table operation");
        auto& read = output->Execution.TableOperations.emplace_back();
        read.SetTable(tables[id - 1].Metadata->Path);
        read.SetOperation(static_cast<ui32>(TYdbOperation::Select));
    }
    if (resultTypes) {
        *resultTypes = std::move(outputTypes);
    }
    return output;
}

P ExportNativeQuery(const TNativeQuery& query, const TNativeSourceOrdinals& source, const TOfflineCatalog* catalog,
    const TNativeParameters* parameters)
{
    return WriteNative(query, source, catalog, parameters, nullptr);
}

L ExportLogicalPlan(const TNativeQuery& query, const TNativeSourceOrdinals& source, const TOfflineCatalog* catalog,
    const TNativeParameters* parameters)
{
    L logical;
    logical.set_version(1);
    auto native = WriteNative(query, source, catalog, parameters, &logical);
    logical.mutable_types()->Swap(native.mutable_types());
    logical.mutable_functions()->Swap(native.mutable_functions());
    logical.mutable_parameters()->Swap(native.mutable_parameters());
    for (int i = 0; i < native.roots_size(); ++i) {
        auto& root = *native.mutable_roots(i);
        auto& dest = *logical.mutable_roots(i);
        dest.set_id(root.id());
        dest.set_statement_index(root.statement_index());
        dest.set_result_index(i);
        dest.mutable_columns()->Swap(root.mutable_columns());
        auto& src = *root.mutable_plan();
        auto& graph = *dest.mutable_graph();
        graph.set_input_operator_id(src.input_operator_id());
        graph.mutable_expressions()->Swap(src.mutable_expressions());
        graph.mutable_replicate_hubs()->Swap(src.mutable_replicate_hubs());
        for (auto& op : *src.mutable_operators()) {
            auto& node = *graph.add_operators();
            node.set_id(op.id());
            switch (op.node_case()) {
                case P::Operator::kEmptySource: node.mutable_rows()->Swap(op.mutable_empty_source()); break;
                case P::Operator::kMap: node.mutable_map()->Swap(op.mutable_map()); break;
                case P::Operator::kFilter: node.mutable_filter()->Swap(op.mutable_filter()); break;
                case P::Operator::kJoin: node.mutable_join()->Swap(op.mutable_join()); break;
                case P::Operator::kAggregate: node.mutable_aggregate()->Swap(op.mutable_aggregate()); break;
                case P::Operator::kReplicatePort: node.mutable_replicate_port()->Swap(op.mutable_replicate_port()); break;
                case P::Operator::kRead: {
                    const auto& read = op.read();
                    const auto& table = native.tables(read.table_id() - 1);
                    auto* out = node.mutable_read();
                    out->set_table_path(table.path());
                    for (const auto& binding : read.columns()) {
                        auto* column = out->add_columns();
                        const auto& iu = src.information_units(binding.output_iu_id() - 1);
                        column->set_column(iu.column_name());
                        column->set_iu_id(binding.output_iu_id());
                    }
                    break;
                }
                default: ythrow yexception() << "Unsupported LogicalPlan export operator";
            }
        }
    }
    return logical;
}

std::shared_ptr<TNativeQuery> ImportLogicalPlan(const L& logical, TExprContext& ctx, const TOfflineCatalog* catalog,
    TNativeResultTypes* resultTypes, const TNativeParameters* parameters)
{
    CheckFields(logical);
    Y_ENSURE(logical.version() == 1 && logical.required_features().empty(), "Unsupported LogicalPlan version or features");
    Y_ENSURE(logical.roots_size() > 0 && static_cast<size_t>(logical.roots_size()) <= MaxNodes
        && static_cast<size_t>(logical.types_size()) <= MaxNodes, "Unsupported LogicalPlan size");
    Y_ENSURE(static_cast<size_t>(logical.parameters_size()) <= MaxInformationUnits, "Too many LogicalPlan parameters");
    const auto [types, functions, boundParameters] = ReadRegistries(logical, ctx, parameters);
    THashSet<TString> usedParameters;
    THashSet<TString> readPaths;
    auto result = std::make_shared<TNativeQuery>();
    TNativeResultTypes outputTypes;
    for (int i = 0; i < logical.roots_size(); ++i) {
        const auto& root = logical.roots(i);
        CheckFields(root);
        Y_ENSURE(root.id() == static_cast<ui32>(i + 1) && root.has_result_index() && root.result_index() == static_cast<ui32>(i)
            && root.has_graph() && !root.has_returning_limit_expr_id() && !root.ordered_rows() && root.has_statement_index()
            && (i == 0 || logical.roots(i - 1).statement_index() < root.statement_index())
            && root.columns_size() > 0 && static_cast<size_t>(root.columns_size()) <= MaxInformationUnits,
            "Unsupported LogicalPlan root destination or ordering");
        auto imported = TPlanReader(ctx, types, functions, boundParameters, usedParameters, catalog,
            TLogicalReadBindings{root.graph(), readPaths}).Read(root, root.graph(), i, resultTypes ? &outputTypes.emplace_back() : nullptr);
        result->Results.push_back({imported->GetQueryColumns(), 0});
        result->Roots.push_back(std::move(imported));
        result->Execution.DiscardResults.push_back(false);
    }
    Y_ENSURE(usedParameters.size() == boundParameters.size(), "Unused LogicalPlan parameter declaration");
    for (const auto& path : readPaths) {
        auto& read = result->Execution.TableOperations.emplace_back();
        read.SetTable(path);
        read.SetOperation(static_cast<ui32>(TYdbOperation::Select));
    }
    if (resultTypes) {
        *resultTypes = std::move(outputTypes);
    }
    return result;
}

} // namespace NKikimr::NKqp::NParsedQueryBench
