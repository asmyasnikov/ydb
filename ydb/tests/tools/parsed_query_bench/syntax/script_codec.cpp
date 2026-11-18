#include "script_codec.h"

#include <ydb/tests/tools/parsed_query_bench/measurement.h>

#include <util/generic/yexception.h>

#include <limits>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using namespace NNewYql;
using PS = Ydb::Query::SyntaxScript;
using Metadata = Ydb::Query::YqlParserMetadata;

// Leave room for the ExecuteQueryRequest and ParsedQuery envelopes.
constexpr size_t MaxDepth = MaxRequestRecursionDepth - 2;
constexpr size_t MaxNodes = 100000;
constexpr size_t MaxTextBytes = MaxRequestBytes;

class TReadBudget {
public:
    class TScope {
    public:
        explicit TScope(size_t& depth)
            : Depth(depth)
        {
            ++Depth;
        }

        ~TScope() {
            --Depth;
        }

        TScope(const TScope&) = delete;
        TScope& operator=(const TScope&) = delete;

    private:
        size_t& Depth;
    };

    TScope Enter() {
        Y_ENSURE(Depth < MaxDepth && Nodes < MaxNodes, "SyntaxScript AST depth or size limit exceeded");
        ++Nodes;
        return TScope(Depth);
    }

    template <class TProto>
    TScope Enter(const TProto& message) {
        Y_ENSURE(message.GetReflection()->GetUnknownFields(message).empty(),
            "Unknown field in SyntaxScript v1");
        return Enter();
    }

    void CheckRepeated(size_t count) const {
        Y_ENSURE(count <= MaxNodes - Nodes, "SyntaxScript AST size limit exceeded");
    }

    template <class TProto>
    TString ReadString(const TProto& in) {
        Y_ENSURE(in.size() <= MaxTextBytes - TextBytes, "SyntaxScript text size limit exceeded");
        TextBytes += in.size();
        return TString(in.data(), in.size());
    }

    void CountRepeatedScalars(size_t count) {
        CheckRepeated(count);
        Nodes += count;
    }

private:
    size_t Depth = 0;
    size_t Nodes = 0;
    size_t TextBytes = 0;
};

void CheckExportedTree(const google::protobuf::Message& message, TReadBudget& budget) {
    auto scope = budget.Enter();
    const auto* descriptor = message.GetDescriptor();
    const auto* reflection = message.GetReflection();
    for (int i = 0; i < descriptor->field_count(); ++i) {
        const auto* field = descriptor->field(i);
        const bool child = field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE;
        if (field->is_repeated()) {
            const auto count = reflection->FieldSize(message, field);
            if (child) {
                budget.CheckRepeated(count);
                for (int index = 0; index < count; ++index) {
                    CheckExportedTree(reflection->GetRepeatedMessage(message, field, index), budget);
                }
            } else {
                budget.CountRepeatedScalars(count);
            }
        } else if (child && reflection->HasField(message, field)) {
            CheckExportedTree(reflection->GetMessage(message, field), budget);
        }
    }
}

template <class TProto, class TAst, int First, int Last>
struct TEnumCodec {
    static TProto Encode(TAst input) {
        const int value = static_cast<int>(input) + First;
        Y_ENSURE(value >= First && value <= Last, "Unsupported SyntaxScript AST enum value");
        return static_cast<TProto>(value);
    }

    static TAst Decode(TProto input) {
        const int value = static_cast<int>(input);
        Y_ENSURE(value >= First && value <= Last, "Unsupported SyntaxScript enum value");
        return static_cast<TAst>(value - First);
    }
};

using TLiteralKindCodec = TEnumCodec<PS::Literal::Kind, ELiteralKind, PS::Literal::INTEGER, PS::Literal::BLOB>;
static_assert(static_cast<int>(ELiteralKind::Integer) + 1 == static_cast<int>(PS::Literal::INTEGER));
static_assert(static_cast<int>(ELiteralKind::Real) + 1 == static_cast<int>(PS::Literal::REAL));
static_assert(static_cast<int>(ELiteralKind::String) + 1 == static_cast<int>(PS::Literal::STRING));
static_assert(static_cast<int>(ELiteralKind::Boolean) + 1 == static_cast<int>(PS::Literal::BOOLEAN));
static_assert(static_cast<int>(ELiteralKind::Null) + 1 == static_cast<int>(PS::Literal::NULL_LITERAL));
static_assert(static_cast<int>(ELiteralKind::Blob) + 1 == static_cast<int>(PS::Literal::BLOB));

using TUnaryOpCodec = TEnumCodec<PS::Unary::Op, EUnaryOp, PS::Unary::PLUS, PS::Unary::IS_NOT_NULL>;
static_assert(static_cast<int>(EUnaryOp::Plus) + 1 == static_cast<int>(PS::Unary::PLUS));
static_assert(static_cast<int>(EUnaryOp::Minus) + 1 == static_cast<int>(PS::Unary::MINUS));
static_assert(static_cast<int>(EUnaryOp::BitNot) + 1 == static_cast<int>(PS::Unary::BIT_NOT));
static_assert(static_cast<int>(EUnaryOp::Not) + 1 == static_cast<int>(PS::Unary::NOT));
static_assert(static_cast<int>(EUnaryOp::IsNull) + 1 == static_cast<int>(PS::Unary::IS_NULL));
static_assert(static_cast<int>(EUnaryOp::IsNotNull) + 1 == static_cast<int>(PS::Unary::IS_NOT_NULL));

using TBinaryOpCodec = TEnumCodec<PS::Binary::Op, EBinaryOp, PS::Binary::OR, PS::Binary::COALESCE>;
static_assert(static_cast<int>(EBinaryOp::Or) + 1 == static_cast<int>(PS::Binary::OR));
static_assert(static_cast<int>(EBinaryOp::And) + 1 == static_cast<int>(PS::Binary::AND));
static_assert(static_cast<int>(EBinaryOp::Xor) + 1 == static_cast<int>(PS::Binary::XOR));
static_assert(static_cast<int>(EBinaryOp::Equal) + 1 == static_cast<int>(PS::Binary::EQUAL));
static_assert(static_cast<int>(EBinaryOp::NotEqual) + 1 == static_cast<int>(PS::Binary::NOT_EQUAL));
static_assert(static_cast<int>(EBinaryOp::Less) + 1 == static_cast<int>(PS::Binary::LESS));
static_assert(static_cast<int>(EBinaryOp::LessOrEqual) + 1 == static_cast<int>(PS::Binary::LESS_OR_EQUAL));
static_assert(static_cast<int>(EBinaryOp::Greater) + 1 == static_cast<int>(PS::Binary::GREATER));
static_assert(static_cast<int>(EBinaryOp::GreaterOrEqual) + 1 == static_cast<int>(PS::Binary::GREATER_OR_EQUAL));
static_assert(static_cast<int>(EBinaryOp::IsDistinctFrom) + 1 == static_cast<int>(PS::Binary::IS_DISTINCT_FROM));
static_assert(static_cast<int>(EBinaryOp::IsNotDistinctFrom) + 1 == static_cast<int>(PS::Binary::IS_NOT_DISTINCT_FROM));
static_assert(static_cast<int>(EBinaryOp::Add) + 1 == static_cast<int>(PS::Binary::ADD));
static_assert(static_cast<int>(EBinaryOp::Subtract) + 1 == static_cast<int>(PS::Binary::SUBTRACT));
static_assert(static_cast<int>(EBinaryOp::Multiply) + 1 == static_cast<int>(PS::Binary::MULTIPLY));
static_assert(static_cast<int>(EBinaryOp::Divide) + 1 == static_cast<int>(PS::Binary::DIVIDE));
static_assert(static_cast<int>(EBinaryOp::Modulo) + 1 == static_cast<int>(PS::Binary::MODULO));
static_assert(static_cast<int>(EBinaryOp::BitAnd) + 1 == static_cast<int>(PS::Binary::BIT_AND));
static_assert(static_cast<int>(EBinaryOp::BitOr) + 1 == static_cast<int>(PS::Binary::BIT_OR));
static_assert(static_cast<int>(EBinaryOp::BitXor) + 1 == static_cast<int>(PS::Binary::BIT_XOR));
static_assert(static_cast<int>(EBinaryOp::ShiftLeft) + 1 == static_cast<int>(PS::Binary::SHIFT_LEFT));
static_assert(static_cast<int>(EBinaryOp::ShiftRight) + 1 == static_cast<int>(PS::Binary::SHIFT_RIGHT));
static_assert(static_cast<int>(EBinaryOp::RotateLeft) + 1 == static_cast<int>(PS::Binary::ROTATE_LEFT));
static_assert(static_cast<int>(EBinaryOp::RotateRight) + 1 == static_cast<int>(PS::Binary::ROTATE_RIGHT));
static_assert(static_cast<int>(EBinaryOp::Concat) + 1 == static_cast<int>(PS::Binary::CONCAT));
static_assert(static_cast<int>(EBinaryOp::Coalesce) + 1 == static_cast<int>(PS::Binary::COALESCE));

using TQuantifierCodec = TEnumCodec<PS::SetQuantifier, ESetQuantifier, PS::QUANTIFIER_DEFAULT, PS::QUANTIFIER_DISTINCT>;
static_assert(static_cast<int>(ESetQuantifier::Default) + 0 == static_cast<int>(PS::QUANTIFIER_DEFAULT));
static_assert(static_cast<int>(ESetQuantifier::All) + 0 == static_cast<int>(PS::QUANTIFIER_ALL));
static_assert(static_cast<int>(ESetQuantifier::Distinct) + 0 == static_cast<int>(PS::QUANTIFIER_DISTINCT));

using TSortDirectionCodec = TEnumCodec<PS::SortSpecification::Direction, ESortDirection, PS::SortSpecification::DEFAULT, PS::SortSpecification::DESCENDING>;
static_assert(static_cast<int>(ESortDirection::Default) + 0 == static_cast<int>(PS::SortSpecification::DEFAULT));
static_assert(static_cast<int>(ESortDirection::Ascending) + 0 == static_cast<int>(PS::SortSpecification::ASCENDING));
static_assert(static_cast<int>(ESortDirection::Descending) + 0 == static_cast<int>(PS::SortSpecification::DESCENDING));

using TNullTreatmentCodec = TEnumCodec<PS::Call::NullTreatment, ENullTreatment, PS::Call::DEFAULT, PS::Call::IGNORE>;
static_assert(static_cast<int>(ENullTreatment::Default) + 0 == static_cast<int>(PS::Call::DEFAULT));
static_assert(static_cast<int>(ENullTreatment::Respect) + 0 == static_cast<int>(PS::Call::RESPECT));
static_assert(static_cast<int>(ENullTreatment::Ignore) + 0 == static_cast<int>(PS::Call::IGNORE));

using TWindowUnitsCodec = TEnumCodec<PS::WindowFrame::Units, EWindowFrameUnits, PS::WindowFrame::ROWS, PS::WindowFrame::GROUPS>;
static_assert(static_cast<int>(EWindowFrameUnits::Rows) == static_cast<int>(PS::WindowFrame::ROWS));
static_assert(static_cast<int>(EWindowFrameUnits::Range) == static_cast<int>(PS::WindowFrame::RANGE));
static_assert(static_cast<int>(EWindowFrameUnits::Groups) == static_cast<int>(PS::WindowFrame::GROUPS));

using TWindowBoundKindCodec = TEnumCodec<PS::WindowBound::Kind, EWindowBoundKind, PS::WindowBound::CURRENT_ROW, PS::WindowBound::UNBOUNDED_FOLLOWING>;
static_assert(static_cast<int>(EWindowBoundKind::CurrentRow) == static_cast<int>(PS::WindowBound::CURRENT_ROW));
static_assert(static_cast<int>(EWindowBoundKind::Preceding) == static_cast<int>(PS::WindowBound::PRECEDING));
static_assert(static_cast<int>(EWindowBoundKind::Following) == static_cast<int>(PS::WindowBound::FOLLOWING));
static_assert(static_cast<int>(EWindowBoundKind::UnboundedPreceding) == static_cast<int>(PS::WindowBound::UNBOUNDED_PRECEDING));
static_assert(static_cast<int>(EWindowBoundKind::UnboundedFollowing) == static_cast<int>(PS::WindowBound::UNBOUNDED_FOLLOWING));

using TWindowExclusionCodec = TEnumCodec<PS::WindowFrame::Exclusion, EWindowExclusion, PS::WindowFrame::EXCLUSION_DEFAULT, PS::WindowFrame::EXCLUDE_NO_OTHERS>;
static_assert(static_cast<int>(EWindowExclusion::Default) == static_cast<int>(PS::WindowFrame::EXCLUSION_DEFAULT));
static_assert(static_cast<int>(EWindowExclusion::CurrentRow) == static_cast<int>(PS::WindowFrame::EXCLUDE_CURRENT_ROW));
static_assert(static_cast<int>(EWindowExclusion::Group) == static_cast<int>(PS::WindowFrame::EXCLUDE_GROUP));
static_assert(static_cast<int>(EWindowExclusion::Ties) == static_cast<int>(PS::WindowFrame::EXCLUDE_TIES));
static_assert(static_cast<int>(EWindowExclusion::NoOthers) == static_cast<int>(PS::WindowFrame::EXCLUDE_NO_OTHERS));

using TPatternOpCodec = TEnumCodec<PS::PatternMatch::Op, EPatternOp, PS::PatternMatch::LIKE, PS::PatternMatch::MATCH>;
static_assert(static_cast<int>(EPatternOp::Like) + 1 == static_cast<int>(PS::PatternMatch::LIKE));
static_assert(static_cast<int>(EPatternOp::ILike) + 1 == static_cast<int>(PS::PatternMatch::ILIKE));
static_assert(static_cast<int>(EPatternOp::Glob) + 1 == static_cast<int>(PS::PatternMatch::GLOB));
static_assert(static_cast<int>(EPatternOp::Regexp) + 1 == static_cast<int>(PS::PatternMatch::REGEXP));
static_assert(static_cast<int>(EPatternOp::RLike) + 1 == static_cast<int>(PS::PatternMatch::RLIKE));
static_assert(static_cast<int>(EPatternOp::Match) + 1 == static_cast<int>(PS::PatternMatch::MATCH));

using TJoinKindCodec = TEnumCodec<PS::Join::Kind, EJoinKind, PS::Join::INNER, PS::Join::EXCLUSION>;
static_assert(static_cast<int>(EJoinKind::Inner) + 1 == static_cast<int>(PS::Join::INNER));
static_assert(static_cast<int>(EJoinKind::Cross) + 1 == static_cast<int>(PS::Join::CROSS));
static_assert(static_cast<int>(EJoinKind::Left) + 1 == static_cast<int>(PS::Join::LEFT));
static_assert(static_cast<int>(EJoinKind::Right) + 1 == static_cast<int>(PS::Join::RIGHT));
static_assert(static_cast<int>(EJoinKind::Full) + 1 == static_cast<int>(PS::Join::FULL));
static_assert(static_cast<int>(EJoinKind::LeftOnly) + 1 == static_cast<int>(PS::Join::LEFT_ONLY));
static_assert(static_cast<int>(EJoinKind::RightOnly) + 1 == static_cast<int>(PS::Join::RIGHT_ONLY));
static_assert(static_cast<int>(EJoinKind::LeftSemi) + 1 == static_cast<int>(PS::Join::LEFT_SEMI));
static_assert(static_cast<int>(EJoinKind::RightSemi) + 1 == static_cast<int>(PS::Join::RIGHT_SEMI));
static_assert(static_cast<int>(EJoinKind::Exclusion) + 1 == static_cast<int>(PS::Join::EXCLUSION));

using TGroupingKindCodec = TEnumCodec<PS::GroupingElement::Kind, EGroupingKind, PS::GroupingElement::ORDINARY, PS::GroupingElement::SETS>;
static_assert(static_cast<int>(EGroupingKind::Ordinary) + 0 == static_cast<int>(PS::GroupingElement::ORDINARY));
static_assert(static_cast<int>(EGroupingKind::Rollup) + 0 == static_cast<int>(PS::GroupingElement::ROLLUP));
static_assert(static_cast<int>(EGroupingKind::Cube) + 0 == static_cast<int>(PS::GroupingElement::CUBE));
static_assert(static_cast<int>(EGroupingKind::Sets) + 0 == static_cast<int>(PS::GroupingElement::SETS));

using TSetOpCodec = TEnumCodec<PS::SetOperation::Op, ESetOperation, PS::SetOperation::UNION, PS::SetOperation::EXCEPT>;
static_assert(static_cast<int>(ESetOperation::Union) + 1 == static_cast<int>(PS::SetOperation::UNION));
static_assert(static_cast<int>(ESetOperation::Intersect) + 1 == static_cast<int>(PS::SetOperation::INTERSECT));
static_assert(static_cast<int>(ESetOperation::Except) + 1 == static_cast<int>(PS::SetOperation::EXCEPT));

using TWriteKindCodec = TEnumCodec<PS::Write::Kind, EWriteKind, PS::Write::INSERT, PS::Write::DELETE>;
static_assert(static_cast<int>(EWriteKind::Insert) + 1 == static_cast<int>(PS::Write::INSERT));
static_assert(static_cast<int>(EWriteKind::Upsert) + 1 == static_cast<int>(PS::Write::UPSERT));
static_assert(static_cast<int>(EWriteKind::Replace) + 1 == static_cast<int>(PS::Write::REPLACE));
static_assert(static_cast<int>(EWriteKind::Update) + 1 == static_cast<int>(PS::Write::UPDATE));
static_assert(static_cast<int>(EWriteKind::Delete) + 1 == static_cast<int>(PS::Write::DELETE));

using TPragmaFormCodec = TEnumCodec<PS::Pragma::ValueForm, EPragmaValueForm, PS::Pragma::NO_VALUE, PS::Pragma::LIST>;
static_assert(static_cast<int>(EPragmaValueForm::None) + 0 == static_cast<int>(PS::Pragma::NO_VALUE));
static_assert(static_cast<int>(EPragmaValueForm::Assignment) + 0 == static_cast<int>(PS::Pragma::ASSIGNMENT));
static_assert(static_cast<int>(EPragmaValueForm::List) + 0 == static_cast<int>(PS::Pragma::LIST));

using TExplainModeCodec = TEnumCodec<PS::Statement::ExplainMode, EExplainMode, PS::Statement::EXPLAIN_NONE, PS::Statement::EXPLAIN_QUERY_PLAN>;
static_assert(static_cast<int>(EExplainMode::None) + 0 == static_cast<int>(PS::Statement::EXPLAIN_NONE));
static_assert(static_cast<int>(EExplainMode::Explain) + 0 == static_cast<int>(PS::Statement::EXPLAIN));
static_assert(static_cast<int>(EExplainMode::QueryPlan) + 0 == static_cast<int>(PS::Statement::EXPLAIN_QUERY_PLAN));

void Convert(const TSourceRange& in, PS::SourceRange& out);
void Convert(const PS::SourceRange& in, TSourceRange& out, TReadBudget& budget);
void Convert(const TIdentifier& in, PS::Identifier& out);
void Convert(const PS::Identifier& in, TIdentifier& out, TReadBudget& budget);
void Convert(const TParameter& in, PS::Parameter& out);
void Convert(const PS::Parameter& in, TParameter& out, TReadBudget& budget);
void Convert(const TLiteral& in, PS::Literal& out);
void Convert(const PS::Literal& in, TLiteral& out, TReadBudget& budget);
void Convert(const TName& in, PS::Name& out);
void Convert(const PS::Name& in, TName& out, TReadBudget& budget);
void Convert(const TStar& in, PS::Star& out);
void Convert(const PS::Star& in, TStar& out, TReadBudget& budget);
void Convert(const TUnary& in, PS::Unary& out);
void Convert(const PS::Unary& in, TUnary& out, TReadBudget& budget);
void Convert(const TBinary& in, PS::Binary& out);
void Convert(const PS::Binary& in, TBinary& out, TReadBudget& budget);
void Convert(const TNamedExpression& in, PS::NamedExpression& out);
void Convert(const PS::NamedExpression& in, TNamedExpression& out, TReadBudget& budget);
void Convert(const TSortSpecification& in, PS::SortSpecification& out);
void Convert(const PS::SortSpecification& in, TSortSpecification& out, TReadBudget& budget);
void Convert(const TWindowBound& in, PS::WindowBound& out);
void Convert(const PS::WindowBound& in, TWindowBound& out, TReadBudget& budget);
void Convert(const TWindowFrame& in, PS::WindowFrame& out);
void Convert(const PS::WindowFrame& in, TWindowFrame& out, TReadBudget& budget);
void Convert(const TWindowSpecification& in, PS::WindowSpecification& out);
void Convert(const PS::WindowSpecification& in, TWindowSpecification& out, TReadBudget& budget);
void Convert(const TWindowReference& in, PS::WindowReference& out);
void Convert(const PS::WindowReference& in, TWindowReference& out, TReadBudget& budget);
void Convert(const TWindowDefinition& in, PS::WindowDefinition& out);
void Convert(const PS::WindowDefinition& in, TWindowDefinition& out, TReadBudget& budget);
void Convert(const TCall& in, PS::Call& out);
void Convert(const PS::Call& in, TCall& out, TReadBudget& budget);
void Convert(const TMember& in, PS::Member& out);
void Convert(const PS::Member& in, TMember& out, TReadBudget& budget);
void Convert(const TIndex& in, PS::Index& out);
void Convert(const PS::Index& in, TIndex& out, TReadBudget& budget);
void Convert(const TTuple& in, PS::Tuple& out);
void Convert(const PS::Tuple& in, TTuple& out, TReadBudget& budget);
void Convert(const TList& in, PS::ExpressionList& out);
void Convert(const PS::ExpressionList& in, TList& out, TReadBudget& budget);
void Convert(const TWhen& in, PS::When& out);
void Convert(const PS::When& in, TWhen& out, TReadBudget& budget);
void Convert(const TCase& in, PS::Case& out);
void Convert(const PS::Case& in, TCase& out, TReadBudget& budget);
void Convert(const TTypeTag& in, PS::TypeTag& out);
void Convert(const PS::TypeTag& in, TTypeTag& out, TReadBudget& budget);
void Convert(const TTypeName& in, PS::TypeName& out);
void Convert(const PS::TypeName& in, TTypeName& out, TReadBudget& budget);
void Convert(const TTypeMember& in, PS::TypeMember& out);
void Convert(const PS::TypeMember& in, TTypeMember& out, TReadBudget& budget);
void Convert(const TCast& in, PS::Cast& out);
void Convert(const PS::Cast& in, TCast& out, TReadBudget& budget);
void Convert(const TBetween& in, PS::Between& out);
void Convert(const PS::Between& in, TBetween& out, TReadBudget& budget);
void Convert(const TIn& in, PS::In& out);
void Convert(const PS::In& in, TIn& out, TReadBudget& budget);
void Convert(const TPatternMatch& in, PS::PatternMatch& out);
void Convert(const PS::PatternMatch& in, TPatternMatch& out, TReadBudget& budget);
void Convert(const TLambda& in, PS::Lambda& out);
void Convert(const PS::Lambda& in, TLambda& out, TReadBudget& budget);
void Convert(const TExpression& in, PS::Expression& out);
void Convert(const PS::Expression& in, TExpression& out, TReadBudget& budget);
void Convert(const TTable& in, PS::Table& out);
void Convert(const PS::Table& in, TTable& out, TReadBudget& budget);
void Convert(const TJoin& in, PS::Join& out);
void Convert(const PS::Join& in, TJoin& out, TReadBudget& budget);
void Convert(const TSource& in, PS::Source& out);
void Convert(const PS::Source& in, TSource& out, TReadBudget& budget);
void Convert(const TGroupingElement& in, PS::GroupingElement& out);
void Convert(const PS::GroupingElement& in, TGroupingElement& out, TReadBudget& budget);
void Convert(const TSelect& in, PS::Select& out);
void Convert(const PS::Select& in, TSelect& out, TReadBudget& budget);
void Convert(const TSetOperation& in, PS::SetOperation& out);
void Convert(const PS::SetOperation& in, TSetOperation& out, TReadBudget& budget);
void Convert(const TCte& in, PS::Cte& out);
void Convert(const PS::Cte& in, TCte& out, TReadBudget& budget);
void Convert(const TQuery& in, PS::Query& out);
void Convert(const PS::Query& in, TQuery& out, TReadBudget& budget);
void Convert(const TDeclareStatement& in, PS::Declare& out);
void Convert(const PS::Declare& in, TDeclareStatement& out, TReadBudget& budget);
void Convert(const TWriteStatement& in, PS::Write& out);
void Convert(const PS::Write& in, TWriteStatement& out, TReadBudget& budget);
void Convert(const TPragmaValue& in, PS::PragmaValue& out);
void Convert(const PS::PragmaValue& in, TPragmaValue& out, TReadBudget& budget);
void Convert(const TPragmaStatement& in, PS::Pragma& out);
void Convert(const PS::Pragma& in, TPragmaStatement& out, TReadBudget& budget);
void Convert(const TNamedBinding& in, PS::NamedBinding& out);
void Convert(const PS::NamedBinding& in, TNamedBinding& out, TReadBudget& budget);
void Convert(const TStatement& in, PS::Statement& out);
void Convert(const PS::Statement& in, TStatement& out, TReadBudget& budget);

template <class TProto, class TAst>
void Decode(const TProto& in, TAst& out, TReadBudget& budget) {
    auto scope = budget.Enter(in);
    Convert(in, out, budget);
}

template <class TProto, class TAst>
void DecodeOptional(const TProto& in, std::optional<TAst>& out, TReadBudget& budget) {
    Decode(in, out.emplace(), budget);
}

template <class TProto, class TAst>
void DecodePtr(const TProto& in, std::unique_ptr<TAst>& out, TReadBudget& budget) {
    out = std::make_unique<TAst>();
    Decode(in, *out, budget);
}

template <class TProto, class TAst>
void DecodeRepeated(const TProto& in, TVector<TAst>& out, TReadBudget& budget) {
    budget.CheckRepeated(in.size());
    out.reserve(in.size());
    for (const auto& entry : in) {
        Decode(entry, out.emplace_back(), budget);
    }
}

template <class TProto, class TAst>
void DecodeRepeatedPtrs(const TProto& in, TVector<std::unique_ptr<TAst>>& out, TReadBudget& budget) {
    budget.CheckRepeated(in.size());
    out.reserve(in.size());
    for (const auto& entry : in) {
        DecodePtr(entry, out.emplace_back(), budget);
    }
}

template <class... TVisitors>
struct TOverloaded : TVisitors... {
    using TVisitors::operator()...;
};


void Convert(const TSourceRange& in, PS::SourceRange& out) {
    out.set_begin(in.Begin);
    out.set_end(in.End);
    out.set_line(in.Line);
    out.set_column(in.Column);
}
void Convert(const PS::SourceRange& in, TSourceRange& out, TReadBudget&) {
    Y_ENSURE(in.begin() <= in.end() && in.end() <= std::numeric_limits<size_t>::max()
        && in.line(), "Invalid SyntaxScript source range");
    out.Begin = in.begin();
    out.End = in.end();
    out.Line = in.line();
    out.Column = in.column();
}

void Convert(const TIdentifier& in, PS::Identifier& out) {
    out.set_spelling(in.Spelling.data(), in.Spelling.size());
    Convert(in.Range, *out.mutable_range());
}
void Convert(const PS::Identifier& in, TIdentifier& out, TReadBudget& budget) {
    Y_ENSURE(!in.spelling().empty(), "Empty SyntaxScript identifier");
    out.Spelling = budget.ReadString(in.spelling());
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
}

void Convert(const TParameter& in, PS::Parameter& out) {
    out.set_spelling(in.Spelling.data(), in.Spelling.size());
}
void Convert(const PS::Parameter& in, TParameter& out, TReadBudget& budget) {
    Y_ENSURE(!in.spelling().empty() && in.spelling().front() == '$', "Invalid SyntaxScript parameter");
    out.Spelling = budget.ReadString(in.spelling());
}

void Convert(const TLiteral& in, PS::Literal& out) {
    out.set_kind(TLiteralKindCodec::Encode(in.Kind));
    out.set_spelling(in.Spelling.data(), in.Spelling.size());
}
void Convert(const PS::Literal& in, TLiteral& out, TReadBudget& budget) {
    Y_ENSURE(!in.spelling().empty(), "Empty SyntaxScript literal");
    out.Kind = TLiteralKindCodec::Decode(in.kind());
    out.Spelling = budget.ReadString(in.spelling());
}

void Convert(const TName& in, PS::Name& out) {
    Convert(in.Name, *out.mutable_name());
    if (in.Module) {
        Convert(*in.Module, *out.mutable_module());
    }
}
void Convert(const PS::Name& in, TName& out, TReadBudget& budget) {
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    if (in.has_module()) {
        DecodeOptional(in.module(), out.Module, budget);
    }
}

void Convert(const TStar& in, PS::Star& out) {
    if (in.Qualifier) {
        Convert(*in.Qualifier, *out.mutable_qualifier());
    }
}
void Convert(const PS::Star& in, TStar& out, TReadBudget& budget) {
    if (in.has_qualifier()) {
        DecodeOptional(in.qualifier(), out.Qualifier, budget);
    }
}

void Convert(const TUnary& in, PS::Unary& out) {
    out.set_op(TUnaryOpCodec::Encode(in.Op));
    Y_ENSURE(in.Operand, "Missing SyntaxScript operand");
    Convert(*in.Operand, *out.mutable_operand());
}
void Convert(const PS::Unary& in, TUnary& out, TReadBudget& budget) {
    out.Op = TUnaryOpCodec::Decode(in.op());
    Y_ENSURE(in.has_operand(), "Missing SyntaxScript operand");
    DecodePtr(in.operand(), out.Operand, budget);
}

void Convert(const TBinary& in, PS::Binary& out) {
    out.set_op(TBinaryOpCodec::Encode(in.Op));
    Y_ENSURE(in.Left, "Missing SyntaxScript left");
    Convert(*in.Left, *out.mutable_left());
    Y_ENSURE(in.Right, "Missing SyntaxScript right");
    Convert(*in.Right, *out.mutable_right());
}
void Convert(const PS::Binary& in, TBinary& out, TReadBudget& budget) {
    out.Op = TBinaryOpCodec::Decode(in.op());
    Y_ENSURE(in.has_left(), "Missing SyntaxScript left");
    DecodePtr(in.left(), out.Left, budget);
    Y_ENSURE(in.has_right(), "Missing SyntaxScript right");
    DecodePtr(in.right(), out.Right, budget);
}

void Convert(const TNamedExpression& in, PS::NamedExpression& out) {
    Y_ENSURE(in.Expression, "Missing SyntaxScript expression");
    Convert(*in.Expression, *out.mutable_expression());
    if (in.Alias) {
        Convert(*in.Alias, *out.mutable_alias());
    }
}
void Convert(const PS::NamedExpression& in, TNamedExpression& out, TReadBudget& budget) {
    Y_ENSURE(in.has_expression(), "Missing SyntaxScript expression");
    DecodePtr(in.expression(), out.Expression, budget);
    if (in.has_alias()) {
        DecodeOptional(in.alias(), out.Alias, budget);
    }
}

void Convert(const TSortSpecification& in, PS::SortSpecification& out) {
    Y_ENSURE(in.Expression, "Missing SyntaxScript expression");
    Convert(*in.Expression, *out.mutable_expression());
    out.set_direction(TSortDirectionCodec::Encode(in.Direction));
}
void Convert(const PS::SortSpecification& in, TSortSpecification& out, TReadBudget& budget) {
    Y_ENSURE(in.has_expression(), "Missing SyntaxScript expression");
    DecodePtr(in.expression(), out.Expression, budget);
    out.Direction = TSortDirectionCodec::Decode(in.direction());
}

void Convert(const TWindowBound& in, PS::WindowBound& out) {
    Convert(in.Range, *out.mutable_range());
    out.set_kind(TWindowBoundKindCodec::Encode(in.Kind));
    if (in.Offset) {
        Convert(*in.Offset, *out.mutable_offset());
    }
}
void Convert(const PS::WindowBound& in, TWindowBound& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    out.Kind = TWindowBoundKindCodec::Decode(in.kind());
    const bool hasOffset = out.Kind == EWindowBoundKind::Preceding || out.Kind == EWindowBoundKind::Following;
    Y_ENSURE(in.has_offset() == hasOffset, "Invalid SyntaxScript window bound offset");
    if (hasOffset) {
        DecodePtr(in.offset(), out.Offset, budget);
    }
}

void Convert(const TWindowFrame& in, PS::WindowFrame& out) {
    Convert(in.Range, *out.mutable_range());
    out.set_units(TWindowUnitsCodec::Encode(in.Units));
    Convert(in.Begin, *out.mutable_begin());
    if (in.End) {
        Convert(*in.End, *out.mutable_end());
    }
    out.set_exclusion(TWindowExclusionCodec::Encode(in.Exclusion));
}
void Convert(const PS::WindowFrame& in, TWindowFrame& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    out.Units = TWindowUnitsCodec::Decode(in.units());
    Y_ENSURE(in.has_begin(), "Missing SyntaxScript window frame beginning");
    Decode(in.begin(), out.Begin, budget);
    if (in.has_end()) {
        DecodeOptional(in.end(), out.End, budget);
    }
    out.Exclusion = TWindowExclusionCodec::Decode(in.exclusion());
}

void Convert(const TWindowSpecification& in, PS::WindowSpecification& out) {
    Convert(in.Range, *out.mutable_range());
    if (in.BaseWindow) {
        Convert(*in.BaseWindow, *out.mutable_base_window());
    }
    for (const auto& value : in.PartitionBy) {
        Convert(value, *out.add_partition_by());
    }
    out.set_compact_partition(in.CompactPartition);
    for (const auto& value : in.OrderBy) {
        Convert(value, *out.add_order_by());
    }
    if (in.Frame) {
        Convert(*in.Frame, *out.mutable_frame());
    }
}
void Convert(const PS::WindowSpecification& in, TWindowSpecification& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    if (in.has_base_window()) {
        DecodeOptional(in.base_window(), out.BaseWindow, budget);
    }
    DecodeRepeated(in.partition_by(), out.PartitionBy, budget);
    out.CompactPartition = in.compact_partition();
    DecodeRepeated(in.order_by(), out.OrderBy, budget);
    if (in.has_frame()) {
        DecodeOptional(in.frame(), out.Frame, budget);
    }
}

void Convert(const TWindowReference& in, PS::WindowReference& out) {
    Convert(in.Range, *out.mutable_range());
    std::visit(TOverloaded{
        [&](const TIdentifier& value) { Convert(value, *out.mutable_name()); },
        [&](const TWindowSpecification& value) { Convert(value, *out.mutable_specification()); },
    }, in.Value);
}
void Convert(const PS::WindowReference& in, TWindowReference& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    switch (in.kind_case()) {
        case PS::WindowReference::kName:
            Decode(in.name(), out.Value.emplace<TIdentifier>(), budget);
            break;
        case PS::WindowReference::kSpecification:
            Decode(in.specification(), out.Value.emplace<TWindowSpecification>(), budget);
            break;
        default:
            ythrow yexception() << "Missing SyntaxScript window reference";
    }
}

void Convert(const TWindowDefinition& in, PS::WindowDefinition& out) {
    Convert(in.Range, *out.mutable_range());
    Convert(in.Name, *out.mutable_name());
    Convert(in.Specification, *out.mutable_specification());
}
void Convert(const PS::WindowDefinition& in, TWindowDefinition& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    Y_ENSURE(in.has_name() && in.has_specification(), "Missing SyntaxScript window definition name or specification");
    Decode(in.name(), out.Name, budget);
    Decode(in.specification(), out.Specification, budget);
}

void Convert(const TCall& in, PS::Call& out) {
    Y_ENSURE(in.Callable, "Missing SyntaxScript callable");
    Convert(*in.Callable, *out.mutable_callable());
    for (const auto& value : in.Arguments) {
        Convert(value, *out.add_arguments());
    }
    out.set_quantifier(TQuantifierCodec::Encode(in.Quantifier));
    out.set_star_argument(in.StarArgument);
    out.set_null_treatment(TNullTreatmentCodec::Encode(in.NullTreatment));
    if (in.Filter) {
        Convert(*in.Filter, *out.mutable_filter());
    }
    if (in.Over) {
        Convert(*in.Over, *out.mutable_over());
    }
}
void Convert(const PS::Call& in, TCall& out, TReadBudget& budget) {
    Y_ENSURE(!in.star_argument() || in.arguments().empty(), "Invalid SyntaxScript star call");
    Y_ENSURE(in.has_callable(), "Missing SyntaxScript callable");
    DecodePtr(in.callable(), out.Callable, budget);
    DecodeRepeated(in.arguments(), out.Arguments, budget);
    out.Quantifier = TQuantifierCodec::Decode(in.quantifier());
    out.StarArgument = in.star_argument();
    out.NullTreatment = TNullTreatmentCodec::Decode(in.null_treatment());
    if (in.has_filter()) {
        DecodePtr(in.filter(), out.Filter, budget);
    }
    if (in.has_over()) {
        DecodeOptional(in.over(), out.Over, budget);
    }
}

void Convert(const TMember& in, PS::Member& out) {
    Y_ENSURE(in.Object, "Missing SyntaxScript object");
    Convert(*in.Object, *out.mutable_object());
    Convert(in.Member, *out.mutable_member());
}
void Convert(const PS::Member& in, TMember& out, TReadBudget& budget) {
    Y_ENSURE(in.has_object(), "Missing SyntaxScript object");
    DecodePtr(in.object(), out.Object, budget);
    Y_ENSURE(in.has_member(), "Missing SyntaxScript member");
    Decode(in.member(), out.Member, budget);
}

void Convert(const TIndex& in, PS::Index& out) {
    Y_ENSURE(in.Object, "Missing SyntaxScript object");
    Convert(*in.Object, *out.mutable_object());
    Y_ENSURE(in.Index, "Missing SyntaxScript index");
    Convert(*in.Index, *out.mutable_index());
}
void Convert(const PS::Index& in, TIndex& out, TReadBudget& budget) {
    Y_ENSURE(in.has_object(), "Missing SyntaxScript object");
    DecodePtr(in.object(), out.Object, budget);
    Y_ENSURE(in.has_index(), "Missing SyntaxScript index");
    DecodePtr(in.index(), out.Index, budget);
}

void Convert(const TTuple& in, PS::Tuple& out) {
    for (const auto& value : in.Elements) {
        Convert(value, *out.add_elements());
    }
}
void Convert(const PS::Tuple& in, TTuple& out, TReadBudget& budget) {
    DecodeRepeated(in.elements(), out.Elements, budget);
}

void Convert(const TList& in, PS::ExpressionList& out) {
    for (const auto& value : in.Elements) {
        Y_ENSURE(value, "Missing SyntaxScript elements entry");
        Convert(*value, *out.add_elements());
    }
}
void Convert(const PS::ExpressionList& in, TList& out, TReadBudget& budget) {
    DecodeRepeatedPtrs(in.elements(), out.Elements, budget);
}

void Convert(const TWhen& in, PS::When& out) {
    Y_ENSURE(in.When, "Missing SyntaxScript when_expr");
    Convert(*in.When, *out.mutable_when_expr());
    Y_ENSURE(in.Then, "Missing SyntaxScript then_expr");
    Convert(*in.Then, *out.mutable_then_expr());
}
void Convert(const PS::When& in, TWhen& out, TReadBudget& budget) {
    Y_ENSURE(in.has_when_expr(), "Missing SyntaxScript when_expr");
    DecodePtr(in.when_expr(), out.When, budget);
    Y_ENSURE(in.has_then_expr(), "Missing SyntaxScript then_expr");
    DecodePtr(in.then_expr(), out.Then, budget);
}

void Convert(const TCase& in, PS::Case& out) {
    if (in.Operand) {
        Convert(*in.Operand, *out.mutable_operand());
    }
    for (const auto& value : in.Branches) {
        Convert(value, *out.add_branches());
    }
    if (in.Else) {
        Convert(*in.Else, *out.mutable_else_expr());
    }
}
void Convert(const PS::Case& in, TCase& out, TReadBudget& budget) {
    Y_ENSURE(!in.branches().empty(), "Empty SyntaxScript CASE");
    if (in.has_operand()) {
        DecodePtr(in.operand(), out.Operand, budget);
    }
    DecodeRepeated(in.branches(), out.Branches, budget);
    if (in.has_else_expr()) {
        DecodePtr(in.else_expr(), out.Else, budget);
    }
}

void Convert(const TTypeTag& in, PS::TypeTag& out) {
    Convert(in.Range, *out.mutable_range());
    std::visit(TOverloaded{
        [&](const TIdentifier& value) { Convert(value, *out.mutable_identifier()); },
        [&](const TLiteral& value) { Convert(value, *out.mutable_literal()); },
        [&](const TParameter& value) { Convert(value, *out.mutable_parameter()); },
    }, in.Value);
}
void Convert(const PS::TypeTag& in, TTypeTag& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    switch (in.kind_case()) {
        case PS::TypeTag::kIdentifier:
            Decode(in.identifier(), out.Value.emplace<TIdentifier>(), budget);
            break;
        case PS::TypeTag::kLiteral:
            Decode(in.literal(), out.Value.emplace<TLiteral>(), budget);
            break;
        case PS::TypeTag::kParameter:
            Decode(in.parameter(), out.Value.emplace<TParameter>(), budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

void Convert(const TTypeName& in, PS::TypeName& out) {
    Convert(in.Name, *out.mutable_name());
    for (const auto& value : in.Arguments) {
        Convert(value, *out.add_arguments());
    }
    for (const auto& value : in.Parameters) {
        out.add_parameters(value.data(), value.size());
    }
    out.set_optional_depth(in.OptionalDepth);
    out.set_binding(in.Binding);
    for (const auto& value : in.Members) {
        Convert(value, *out.add_members());
    }
    for (const auto& value : in.Tags) {
        Convert(value, *out.add_tags());
    }
}
void Convert(const PS::TypeName& in, TTypeName& out, TReadBudget& budget) {
    Y_ENSURE(in.optional_depth() <= MaxDepth, "SyntaxScript optional type depth limit exceeded");
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    DecodeRepeated(in.arguments(), out.Arguments, budget);
    budget.CountRepeatedScalars(in.parameters().size());
    for (const auto& value : in.parameters()) {
        out.Parameters.emplace_back(budget.ReadString(value));
    }
    out.OptionalDepth = in.optional_depth();
    out.Binding = in.binding();
    DecodeRepeated(in.members(), out.Members, budget);
    DecodeRepeated(in.tags(), out.Tags, budget);
}

void Convert(const TTypeMember& in, PS::TypeMember& out) {
    if (in.Name) {
        Convert(*in.Name, *out.mutable_name());
    }
    Convert(in.Type, *out.mutable_type());
}
void Convert(const PS::TypeMember& in, TTypeMember& out, TReadBudget& budget) {
    if (in.has_name()) {
        DecodeOptional(in.name(), out.Name, budget);
    }
    Y_ENSURE(in.has_type(), "Missing SyntaxScript type");
    Decode(in.type(), out.Type, budget);
}

void Convert(const TCast& in, PS::Cast& out) {
    Y_ENSURE(in.Value, "Missing SyntaxScript value");
    Convert(*in.Value, *out.mutable_value());
    Convert(in.Type, *out.mutable_type());
    out.set_bitcast(in.Bitcast);
}
void Convert(const PS::Cast& in, TCast& out, TReadBudget& budget) {
    Y_ENSURE(in.has_value(), "Missing SyntaxScript value");
    DecodePtr(in.value(), out.Value, budget);
    Y_ENSURE(in.has_type(), "Missing SyntaxScript type");
    Decode(in.type(), out.Type, budget);
    out.Bitcast = in.bitcast();
}

void Convert(const TBetween& in, PS::Between& out) {
    Y_ENSURE(in.Value, "Missing SyntaxScript value");
    Convert(*in.Value, *out.mutable_value());
    Y_ENSURE(in.Lower, "Missing SyntaxScript lower");
    Convert(*in.Lower, *out.mutable_lower());
    Y_ENSURE(in.Upper, "Missing SyntaxScript upper");
    Convert(*in.Upper, *out.mutable_upper());
    out.set_negated(in.Negated);
    out.set_symmetric(in.Symmetric);
}
void Convert(const PS::Between& in, TBetween& out, TReadBudget& budget) {
    Y_ENSURE(in.has_value(), "Missing SyntaxScript value");
    DecodePtr(in.value(), out.Value, budget);
    Y_ENSURE(in.has_lower(), "Missing SyntaxScript lower");
    DecodePtr(in.lower(), out.Lower, budget);
    Y_ENSURE(in.has_upper(), "Missing SyntaxScript upper");
    DecodePtr(in.upper(), out.Upper, budget);
    out.Negated = in.negated();
    out.Symmetric = in.symmetric();
}

void Convert(const TIn& in, PS::In& out) {
    Y_ENSURE(in.Value, "Missing SyntaxScript value");
    Convert(*in.Value, *out.mutable_value());
    Y_ENSURE(in.Collection, "Missing SyntaxScript collection");
    Convert(*in.Collection, *out.mutable_collection());
    out.set_negated(in.Negated);
    out.set_compact(in.Compact);
}
void Convert(const PS::In& in, TIn& out, TReadBudget& budget) {
    Y_ENSURE(in.has_value(), "Missing SyntaxScript value");
    DecodePtr(in.value(), out.Value, budget);
    Y_ENSURE(in.has_collection(), "Missing SyntaxScript collection");
    DecodePtr(in.collection(), out.Collection, budget);
    out.Negated = in.negated();
    out.Compact = in.compact();
}

void Convert(const TPatternMatch& in, PS::PatternMatch& out) {
    out.set_op(TPatternOpCodec::Encode(in.Op));
    Y_ENSURE(in.Value, "Missing SyntaxScript value");
    Convert(*in.Value, *out.mutable_value());
    Y_ENSURE(in.Pattern, "Missing SyntaxScript pattern");
    Convert(*in.Pattern, *out.mutable_pattern());
    if (in.Escape) {
        Convert(*in.Escape, *out.mutable_escape());
    }
    out.set_negated(in.Negated);
}
void Convert(const PS::PatternMatch& in, TPatternMatch& out, TReadBudget& budget) {
    out.Op = TPatternOpCodec::Decode(in.op());
    Y_ENSURE(in.has_value(), "Missing SyntaxScript value");
    DecodePtr(in.value(), out.Value, budget);
    Y_ENSURE(in.has_pattern(), "Missing SyntaxScript pattern");
    DecodePtr(in.pattern(), out.Pattern, budget);
    if (in.has_escape()) {
        DecodePtr(in.escape(), out.Escape, budget);
    }
    out.Negated = in.negated();
}

void Convert(const TLambda& in, PS::Lambda& out) {
    for (const auto& value : in.Parameters) {
        Convert(value, *out.add_parameters());
    }
    for (const auto& value : in.Bindings) {
        Convert(value, *out.add_bindings());
    }
    Y_ENSURE(in.Body, "Missing SyntaxScript body");
    Convert(*in.Body, *out.mutable_body());
}
void Convert(const PS::Lambda& in, TLambda& out, TReadBudget& budget) {
    DecodeRepeated(in.parameters(), out.Parameters, budget);
    DecodeRepeated(in.bindings(), out.Bindings, budget);
    Y_ENSURE(in.has_body(), "Missing SyntaxScript body");
    DecodePtr(in.body(), out.Body, budget);
}

void Convert(const TExpression& in, PS::Expression& out) {
    Convert(in.Range, *out.mutable_range());
    std::visit(TOverloaded{
        [&](const TLiteral& value) { Convert(value, *out.mutable_literal()); },
        [&](const TName& value) { Convert(value, *out.mutable_name()); },
        [&](const TParameter& value) { Convert(value, *out.mutable_parameter()); },
        [&](const TStar& value) { Convert(value, *out.mutable_star()); },
        [&](const TUnary& value) { Convert(value, *out.mutable_unary()); },
        [&](const TBinary& value) { Convert(value, *out.mutable_binary()); },
        [&](const TCall& value) { Convert(value, *out.mutable_call()); },
        [&](const TMember& value) { Convert(value, *out.mutable_member()); },
        [&](const TIndex& value) { Convert(value, *out.mutable_index()); },
        [&](const TTuple& value) { Convert(value, *out.mutable_tuple()); },
        [&](const TList& value) { Convert(value, *out.mutable_list()); },
        [&](const TCase& value) { Convert(value, *out.mutable_case_expr()); },
        [&](const TCast& value) { Convert(value, *out.mutable_cast()); },
        [&](const TSubquery& value) {
            Y_ENSURE(value.Query, "Missing SyntaxScript subquery");
            Convert(*value.Query, *out.mutable_subquery());
        },
        [&](const TExists& value) {
            Y_ENSURE(value.Query, "Missing SyntaxScript exists");
            Convert(*value.Query, *out.mutable_exists());
        },
        [&](const TBetween& value) { Convert(value, *out.mutable_between()); },
        [&](const TIn& value) { Convert(value, *out.mutable_in_expr()); },
        [&](const TPatternMatch& value) { Convert(value, *out.mutable_pattern_match()); },
        [&](const TLambda& value) { Convert(value, *out.mutable_lambda()); },
    }, in.Value);
}
void Convert(const PS::Expression& in, TExpression& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    switch (in.kind_case()) {
        case PS::Expression::kLiteral:
            Decode(in.literal(), out.Value.emplace<TLiteral>(), budget);
            break;
        case PS::Expression::kName:
            Decode(in.name(), out.Value.emplace<TName>(), budget);
            break;
        case PS::Expression::kParameter:
            Decode(in.parameter(), out.Value.emplace<TParameter>(), budget);
            break;
        case PS::Expression::kStar:
            Decode(in.star(), out.Value.emplace<TStar>(), budget);
            break;
        case PS::Expression::kUnary:
            Decode(in.unary(), out.Value.emplace<TUnary>(), budget);
            break;
        case PS::Expression::kBinary:
            Decode(in.binary(), out.Value.emplace<TBinary>(), budget);
            break;
        case PS::Expression::kCall:
            Decode(in.call(), out.Value.emplace<TCall>(), budget);
            break;
        case PS::Expression::kMember:
            Decode(in.member(), out.Value.emplace<TMember>(), budget);
            break;
        case PS::Expression::kIndex:
            Decode(in.index(), out.Value.emplace<TIndex>(), budget);
            break;
        case PS::Expression::kTuple:
            Decode(in.tuple(), out.Value.emplace<TTuple>(), budget);
            break;
        case PS::Expression::kList:
            Decode(in.list(), out.Value.emplace<TList>(), budget);
            break;
        case PS::Expression::kCaseExpr:
            Decode(in.case_expr(), out.Value.emplace<TCase>(), budget);
            break;
        case PS::Expression::kCast:
            Decode(in.cast(), out.Value.emplace<TCast>(), budget);
            break;
        case PS::Expression::kSubquery:
            DecodePtr(in.subquery(), out.Value.emplace<TSubquery>().Query, budget);
            break;
        case PS::Expression::kExists:
            DecodePtr(in.exists(), out.Value.emplace<TExists>().Query, budget);
            break;
        case PS::Expression::kBetween:
            Decode(in.between(), out.Value.emplace<TBetween>(), budget);
            break;
        case PS::Expression::kInExpr:
            Decode(in.in_expr(), out.Value.emplace<TIn>(), budget);
            break;
        case PS::Expression::kPatternMatch:
            Decode(in.pattern_match(), out.Value.emplace<TPatternMatch>(), budget);
            break;
        case PS::Expression::kLambda:
            Decode(in.lambda(), out.Value.emplace<TLambda>(), budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

void Convert(const TTable& in, PS::Table& out) {
    Convert(in.Name, *out.mutable_name());
    if (in.Cluster) {
        Convert(*in.Cluster, *out.mutable_cluster());
    }
    if (in.Provider) {
        Convert(*in.Provider, *out.mutable_provider());
    }
    out.set_binding(in.Binding);
    out.set_anonymous(in.Anonymous);
}
void Convert(const PS::Table& in, TTable& out, TReadBudget& budget) {
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    if (in.has_cluster()) {
        DecodeOptional(in.cluster(), out.Cluster, budget);
    }
    if (in.has_provider()) {
        DecodeOptional(in.provider(), out.Provider, budget);
    }
    out.Binding = in.binding();
    out.Anonymous = in.anonymous();
}

void Convert(const TJoin& in, PS::Join& out) {
    out.set_kind(TJoinKindCodec::Encode(in.Kind));
    Y_ENSURE(in.Left, "Missing SyntaxScript left");
    Convert(*in.Left, *out.mutable_left());
    Y_ENSURE(in.Right, "Missing SyntaxScript right");
    Convert(*in.Right, *out.mutable_right());
    if (in.On) {
        Convert(*in.On, *out.mutable_on());
    }
    for (const auto& value : in.Using) {
        Convert(value, *out.add_using_columns());
    }
    out.set_natural(in.Natural);
    out.set_left_any(in.LeftAny);
    out.set_right_any(in.RightAny);
}
void Convert(const PS::Join& in, TJoin& out, TReadBudget& budget) {
    Y_ENSURE(!in.has_on() || (in.using_columns().empty() && !in.natural()), "Conflicting SyntaxScript JOIN clauses");
    Y_ENSURE(!in.natural() || in.using_columns().empty(), "Conflicting SyntaxScript JOIN clauses");
    out.Kind = TJoinKindCodec::Decode(in.kind());
    Y_ENSURE(in.has_left(), "Missing SyntaxScript left");
    DecodePtr(in.left(), out.Left, budget);
    Y_ENSURE(in.has_right(), "Missing SyntaxScript right");
    DecodePtr(in.right(), out.Right, budget);
    if (in.has_on()) {
        DecodePtr(in.on(), out.On, budget);
    }
    DecodeRepeated(in.using_columns(), out.Using, budget);
    out.Natural = in.natural();
    out.LeftAny = in.left_any();
    out.RightAny = in.right_any();
}

void Convert(const TSource& in, PS::Source& out) {
    Convert(in.Range, *out.mutable_range());
    if (in.Alias) {
        Convert(*in.Alias, *out.mutable_alias());
    }
    for (const auto& value : in.ColumnAliases) {
        Convert(value, *out.add_column_aliases());
    }
    Y_ENSURE(!in.Sampling && in.Hints.empty(), "SyntaxScript source sampling and hints are unsupported in this benchmark");
    std::visit(TOverloaded{
        [&](const TTable& value) { Convert(value, *out.mutable_table()); },
        [&](const TDerivedTable& value) {
            Y_ENSURE(value.Query, "Missing SyntaxScript derived_table");
            Convert(*value.Query, *out.mutable_derived_table());
        },
        [&](const TJoin& value) { Convert(value, *out.mutable_join()); },
        [&](const TExpressionSource& value) {
            Y_ENSURE(value.Expression, "Missing SyntaxScript expression_source");
            Convert(*value.Expression, *out.mutable_expression_source());
        },
    }, in.Value);
}
void Convert(const PS::Source& in, TSource& out, TReadBudget& budget) {
    Y_ENSURE(!in.has_sampling() && in.hints().empty(), "SyntaxScript source sampling and hints are unsupported in this benchmark");
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    if (in.has_alias()) {
        DecodeOptional(in.alias(), out.Alias, budget);
    }
    DecodeRepeated(in.column_aliases(), out.ColumnAliases, budget);
    switch (in.kind_case()) {
        case PS::Source::kTable:
            Decode(in.table(), out.Value.emplace<TTable>(), budget);
            break;
        case PS::Source::kDerivedTable:
            DecodePtr(in.derived_table(), out.Value.emplace<TDerivedTable>().Query, budget);
            break;
        case PS::Source::kJoin:
            Decode(in.join(), out.Value.emplace<TJoin>(), budget);
            break;
        case PS::Source::kExpressionSource:
            DecodePtr(in.expression_source(), out.Value.emplace<TExpressionSource>().Expression, budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

void Convert(const TGroupingElement& in, PS::GroupingElement& out) {
    Convert(in.Range, *out.mutable_range());
    out.set_kind(TGroupingKindCodec::Encode(in.Kind));
    if (in.Expression.Expression) {
        Convert(in.Expression, *out.mutable_expression());
    }
    for (const auto& value : in.Elements) {
        Convert(value, *out.add_elements());
    }
}
void Convert(const PS::GroupingElement& in, TGroupingElement& out, TReadBudget& budget) {
    if (in.kind() == PS::GroupingElement::ORDINARY) {
        Y_ENSURE(in.has_expression() && in.elements().empty(), "Invalid ordinary SyntaxScript grouping element");
    } else {
        Y_ENSURE(!in.has_expression(), "Conflicting SyntaxScript grouping element fields");
    }
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    out.Kind = TGroupingKindCodec::Decode(in.kind());
    if (in.has_expression()) {
        Decode(in.expression(), out.Expression, budget);
    }
    DecodeRepeated(in.elements(), out.Elements, budget);
}

void Convert(const TSelect& in, PS::Select& out) {
    out.set_quantifier(TQuantifierCodec::Encode(in.Quantifier));
    for (const auto& value : in.Columns) {
        Convert(value, *out.add_columns());
    }
    if (in.From) {
        Convert(*in.From, *out.mutable_from());
    }
    if (in.Where) {
        Convert(*in.Where, *out.mutable_where());
    }
    for (const auto& value : in.GroupBy) {
        Convert(value, *out.add_group_by());
    }
    out.set_compact_group_by(in.CompactGroupBy);
    out.set_group_quantifier(TQuantifierCodec::Encode(in.GroupQuantifier));
    if (in.GroupHint) {
        Convert(*in.GroupHint, *out.mutable_group_hint());
    }
    if (in.Having) {
        Convert(*in.Having, *out.mutable_having());
    }
    for (const auto& value : in.OrderBy) {
        Convert(value, *out.add_order_by());
    }
    out.set_assume_order_by(in.AssumeOrderBy);
    for (const auto& value : in.Windows) {
        Convert(value, *out.add_windows());
    }
}
void Convert(const PS::Select& in, TSelect& out, TReadBudget& budget) {
    Y_ENSURE(!in.columns().empty(), "Empty SyntaxScript projection");
    out.Quantifier = TQuantifierCodec::Decode(in.quantifier());
    DecodeRepeated(in.columns(), out.Columns, budget);
    if (in.has_from()) {
        DecodePtr(in.from(), out.From, budget);
    }
    if (in.has_where()) {
        DecodePtr(in.where(), out.Where, budget);
    }
    DecodeRepeated(in.group_by(), out.GroupBy, budget);
    out.CompactGroupBy = in.compact_group_by();
    out.GroupQuantifier = TQuantifierCodec::Decode(in.group_quantifier());
    if (in.has_group_hint()) {
        DecodeOptional(in.group_hint(), out.GroupHint, budget);
    }
    if (in.has_having()) {
        DecodePtr(in.having(), out.Having, budget);
    }
    DecodeRepeated(in.windows(), out.Windows, budget);
    DecodeRepeated(in.order_by(), out.OrderBy, budget);
    out.AssumeOrderBy = in.assume_order_by();
}

void Convert(const TSetOperation& in, PS::SetOperation& out) {
    out.set_op(TSetOpCodec::Encode(in.Op));
    out.set_quantifier(TQuantifierCodec::Encode(in.Quantifier));
    Y_ENSURE(in.Left, "Missing SyntaxScript left");
    Convert(*in.Left, *out.mutable_left());
    Y_ENSURE(in.Right, "Missing SyntaxScript right");
    Convert(*in.Right, *out.mutable_right());
}
void Convert(const PS::SetOperation& in, TSetOperation& out, TReadBudget& budget) {
    out.Op = TSetOpCodec::Decode(in.op());
    out.Quantifier = TQuantifierCodec::Decode(in.quantifier());
    Y_ENSURE(in.has_left(), "Missing SyntaxScript left");
    DecodePtr(in.left(), out.Left, budget);
    Y_ENSURE(in.has_right(), "Missing SyntaxScript right");
    DecodePtr(in.right(), out.Right, budget);
}

void Convert(const TCte& in, PS::Cte& out) {
    Convert(in.Name, *out.mutable_name());
    for (const auto& value : in.Columns) {
        Convert(value, *out.add_columns());
    }
    Y_ENSURE(in.Query, "Missing SyntaxScript query");
    Convert(*in.Query, *out.mutable_query());
    out.set_recursive(in.Recursive);
}
void Convert(const PS::Cte& in, TCte& out, TReadBudget& budget) {
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    DecodeRepeated(in.columns(), out.Columns, budget);
    Y_ENSURE(in.has_query(), "Missing SyntaxScript query");
    DecodePtr(in.query(), out.Query, budget);
    out.Recursive = in.recursive();
}

void Convert(const TQuery& in, PS::Query& out) {
    Convert(in.Range, *out.mutable_range());
    for (const auto& value : in.With) {
        Convert(value, *out.add_with());
    }
    for (const auto& value : in.OrderBy) {
        Convert(value, *out.add_order_by());
    }
    out.set_parenthesized(in.Parenthesized);
    if (in.Limit) {
        Convert(*in.Limit, *out.mutable_limit());
    }
    if (in.Offset) {
        Convert(*in.Offset, *out.mutable_offset());
    }
    std::visit(TOverloaded{
        [&](const TSelect& value) { Convert(value, *out.mutable_select()); },
        [&](const TSetOperation& value) { Convert(value, *out.mutable_set_operation()); },
    }, in.Body);
}
void Convert(const PS::Query& in, TQuery& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    DecodeRepeated(in.with(), out.With, budget);
    DecodeRepeated(in.order_by(), out.OrderBy, budget);
    out.Parenthesized = in.parenthesized();
    if (in.has_limit()) {
        DecodePtr(in.limit(), out.Limit, budget);
    }
    if (in.has_offset()) {
        DecodePtr(in.offset(), out.Offset, budget);
    }
    switch (in.body_case()) {
        case PS::Query::kSelect:
            Decode(in.select(), out.Body.emplace<TSelect>(), budget);
            break;
        case PS::Query::kSetOperation:
            Decode(in.set_operation(), out.Body.emplace<TSetOperation>(), budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

void Convert(const TDeclareStatement& in, PS::Declare& out) {
    Convert(in.Name, *out.mutable_name());
    Convert(in.Type, *out.mutable_type());
}
void Convert(const PS::Declare& in, TDeclareStatement& out, TReadBudget& budget) {
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    Y_ENSURE(in.has_type(), "Missing SyntaxScript type");
    Decode(in.type(), out.Type, budget);
}

void Convert(const TWriteStatement& in, PS::Write& out) {
    out.set_kind(TWriteKindCodec::Encode(in.Kind));
    Y_ENSURE(in.Target, "Missing SyntaxScript target");
    Convert(*in.Target, *out.mutable_target());
    for (const auto& value : in.Columns) {
        Convert(value, *out.add_columns());
    }
    if (in.Where) {
        Convert(*in.Where, *out.mutable_where());
    }
    for (const auto& value : in.Assignments) {
        Convert(value, *out.add_assignments());
    }
    for (const auto& value : in.Returning) {
        Convert(value, *out.add_returning());
    }
    out.set_returning_all(in.ReturningAll);
    Y_ENSURE(!in.Input || in.Rows.empty(), "Conflicting SyntaxScript write inputs");
    Y_ENSURE(in.OnConflict == "abort" || in.OnConflict == "revert", "Unsupported SyntaxScript conflict action");
    out.set_on_conflict(in.OnConflict == "revert" ? PS::Write::CONFLICT_REVERT : PS::Write::CONFLICT_ABORT);
    if (in.Input) {
        Convert(*in.Input, *out.mutable_query());
    } else if (!in.Rows.empty()) {
        auto& values = *out.mutable_values();
        for (const auto& row : in.Rows) {
            auto& elements = *values.add_rows();
            for (const auto& value : row) {
                Y_ENSURE(value, "Missing SyntaxScript VALUES expression");
                Convert(*value, *elements.add_elements());
            }
        }
    }
}
void Convert(const PS::Write& in, TWriteStatement& out, TReadBudget& budget) {
    Y_ENSURE(in.has_target() && in.target().kind_case() == PS::Source::kTable
        && !in.target().has_alias() && in.target().column_aliases().empty()
        && !in.target().table().has_cluster() && !in.target().table().has_provider()
        && !in.target().table().binding() && !in.target().table().anonymous(), "Invalid SyntaxScript write target");
    Y_ENSURE(!in.returning_all() || in.returning().empty(), "Conflicting SyntaxScript RETURNING fields");
    Y_ENSURE(in.on_conflict() == PS::Write::CONFLICT_ABORT || in.on_conflict() == PS::Write::CONFLICT_REVERT,
        "Unsupported SyntaxScript conflict action");
    out.OnConflict = in.on_conflict() == PS::Write::CONFLICT_REVERT ? "revert" : "abort";
    if (in.has_query()) {
        out.Input = std::make_unique<TQuery>();
        Decode(in.query(), *out.Input, budget);
    } else if (in.has_values()) {
        auto valuesScope = budget.Enter(in.values());
        Y_ENSURE(!in.values().rows().empty() && !in.columns().empty(), "Empty SyntaxScript VALUES");
        for (const auto& row : in.values().rows()) {
            auto rowScope = budget.Enter(row);
            Y_ENSURE(row.elements_size() == in.columns_size(), "Invalid SyntaxScript VALUES row arity");
            auto& elements = out.Rows.emplace_back();
            elements.reserve(row.elements_size());
            for (const auto& value : row.elements()) {
                Decode(value, *elements.emplace_back(std::make_unique<TExpression>()), budget);
            }
        }
    }
    const bool hasInput = in.has_query() || in.has_values();
    const bool hasAssignments = !in.assignments().empty();
    if (in.kind() == PS::Write::UPDATE || in.kind() == PS::Write::DELETE) {
        Y_ENSURE(!hasInput || (!in.has_where() && !hasAssignments), "Conflicting SyntaxScript write inputs");
        Y_ENSURE(hasInput || in.columns().empty(), "Unexpected SyntaxScript write columns");
        Y_ENSURE(in.kind() == PS::Write::UPDATE ? (hasInput || hasAssignments) : !hasAssignments,
            "Invalid SyntaxScript write assignments");
        for (const auto& assignment : in.assignments()) {
            Y_ENSURE(assignment.has_alias(), "Missing SyntaxScript assignment target");
        }
    } else {
        Y_ENSURE(hasInput && !in.has_where() && !hasAssignments, "Invalid SyntaxScript INSERT/UPSERT/REPLACE input");
    }
    out.Kind = TWriteKindCodec::Decode(in.kind());
    Y_ENSURE(in.has_target(), "Missing SyntaxScript target");
    DecodePtr(in.target(), out.Target, budget);
    DecodeRepeated(in.columns(), out.Columns, budget);
    if (in.has_where()) {
        DecodePtr(in.where(), out.Where, budget);
    }
    DecodeRepeated(in.assignments(), out.Assignments, budget);
    DecodeRepeated(in.returning(), out.Returning, budget);
    out.ReturningAll = in.returning_all();
}

void Convert(const TPragmaValue& in, PS::PragmaValue& out) {
    Convert(in.Range, *out.mutable_range());
    std::visit(TOverloaded{
        [&](const TIdentifier& value) { Convert(value, *out.mutable_identifier()); },
        [&](const TLiteral& value) { Convert(value, *out.mutable_literal()); },
        [&](const TParameter& value) { Convert(value, *out.mutable_parameter()); },
    }, in.Value);
}
void Convert(const PS::PragmaValue& in, TPragmaValue& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    switch (in.kind_case()) {
        case PS::PragmaValue::kIdentifier:
            Decode(in.identifier(), out.Value.emplace<TIdentifier>(), budget);
            break;
        case PS::PragmaValue::kLiteral:
            Decode(in.literal(), out.Value.emplace<TLiteral>(), budget);
            break;
        case PS::PragmaValue::kParameter:
            Decode(in.parameter(), out.Value.emplace<TParameter>(), budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

void Convert(const TPragmaStatement& in, PS::Pragma& out) {
    if (in.Namespace) {
        Convert(*in.Namespace, *out.mutable_namespace_());
    }
    Convert(in.Name, *out.mutable_name());
    out.set_value_form(TPragmaFormCodec::Encode(in.ValueForm));
    for (const auto& value : in.Values) {
        Convert(value, *out.add_values());
    }
}
void Convert(const PS::Pragma& in, TPragmaStatement& out, TReadBudget& budget) {
    Y_ENSURE(in.value_form() != PS::Pragma::NO_VALUE || in.values().empty(), "Unexpected SyntaxScript PRAGMA values");
    Y_ENSURE(in.value_form() != PS::Pragma::ASSIGNMENT || in.values_size() == 1, "Invalid SyntaxScript PRAGMA assignment");
    if (in.has_namespace_()) {
        DecodeOptional(in.namespace_(), out.Namespace, budget);
    }
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    out.ValueForm = TPragmaFormCodec::Decode(in.value_form());
    DecodeRepeated(in.values(), out.Values, budget);
}

void Convert(const TNamedBinding& in, PS::NamedBinding& out) {
    Convert(in.Range, *out.mutable_range());
    Convert(in.Name, *out.mutable_name());
    Y_ENSURE(in.Value, "Missing SyntaxScript value");
    Convert(*in.Value, *out.mutable_value());
}
void Convert(const PS::NamedBinding& in, TNamedBinding& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    Y_ENSURE(in.has_name(), "Missing SyntaxScript name");
    Decode(in.name(), out.Name, budget);
    Y_ENSURE(in.has_value(), "Missing SyntaxScript value");
    DecodePtr(in.value(), out.Value, budget);
}

void Convert(const TStatement& in, PS::Statement& out) {
    Convert(in.Range, *out.mutable_range());
    out.set_explain(TExplainModeCodec::Encode(in.Explain));
    std::visit(TOverloaded{
        [&](const TQueryStatement& value) {
            Y_ENSURE(value.Query, "Missing SyntaxScript query");
            Convert(*value.Query, *out.mutable_query());
        },
        [&](const TPragmaStatement& value) { Convert(value, *out.mutable_pragma()); },
        [&](const TNamedBinding& value) { Convert(value, *out.mutable_named_binding()); },
        [&](const TDeclareStatement& value) { Convert(value, *out.mutable_declare()); },
        [&](const TWriteStatement& value) { Convert(value, *out.mutable_write()); },
    }, in.Value);
}
void Convert(const PS::Statement& in, TStatement& out, TReadBudget& budget) {
    if (in.has_range()) {
        Decode(in.range(), out.Range, budget);
    }
    out.Explain = TExplainModeCodec::Decode(in.explain());
    switch (in.kind_case()) {
        case PS::Statement::kQuery:
            DecodePtr(in.query(), out.Value.emplace<TQueryStatement>().Query, budget);
            break;
        case PS::Statement::kPragma:
            Decode(in.pragma(), out.Value.emplace<TPragmaStatement>(), budget);
            break;
        case PS::Statement::kNamedBinding:
            Decode(in.named_binding(), out.Value.emplace<TNamedBinding>(), budget);
            break;
        case PS::Statement::kDeclare:
            Decode(in.declare(), out.Value.emplace<TDeclareStatement>(), budget);
            break;
        case PS::Statement::kWrite:
            Decode(in.write(), out.Value.emplace<TWriteStatement>(), budget);
            break;
        default:
            ythrow yexception() << "Missing or unsupported SyntaxScript variant";
    }
}

} // namespace

PS ExportScript(const TScript& script) {
    PS output;
    output.set_format_version(1);
    output.set_lexer_mode(script.AnsiLexer ? Metadata::ANSI : Metadata::DEFAULT);
    output.set_source_name(script.File.data(), script.File.size());
    for (const auto& statement : script.Statements) {
        Convert(statement, *output.add_statements());
    }
    return output;
}

void ValidateSyntaxScriptLimits(const PS& script) {
    TReadBudget budget;
    CheckExportedTree(script, budget);
}

std::unique_ptr<TScript> ImportScript(const PS& script) {
    TReadBudget budget;
    auto scope = budget.Enter(script);
    Y_ENSURE(script.format_version() == 1, "Unsupported SyntaxScript format version");
    Y_ENSURE(script.lexer_mode() == Metadata::ANSI || script.lexer_mode() == Metadata::DEFAULT,
        "Unsupported SyntaxScript lexer mode");
    Y_ENSURE(!script.statements().empty(), "Empty SyntaxScript");
    if (script.has_directives()) {
        auto directivesScope = budget.Enter(script.directives());
        Y_ENSURE(!script.directives().explicit_ansi_lexer() || script.lexer_mode() == Metadata::ANSI,
            "SyntaxScript ANSI directive disagrees with the effective lexer mode");
    }

    auto output = std::make_unique<TScript>();
    output->File = budget.ReadString(script.source_name());
    output->AnsiLexer = script.lexer_mode() == Metadata::ANSI;
    DecodeRepeated(script.statements(), output->Statements, budget);
    return output;
}

} // namespace NKikimr::NKqp::NParsedQueryBench
