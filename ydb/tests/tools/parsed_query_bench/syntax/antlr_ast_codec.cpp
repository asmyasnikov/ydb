#include "antlr_ast_codec.h"
#include "cst_codec.h"
#include <ydb/tests/tools/parsed_query_bench/syntax/antlr_ast_fingerprint.h>

#include <ydb/core/kqp/host/new_parser/grammar/default/SQLv1Antlr4Parser.h>
#include <ydb/core/kqp/host/new_parser/kqp_ast_builder.h>
#include <yql/essentials/parser/common/antlr4/depth_limiting_listener.h>
#include <yql/essentials/parser/common/antlr4/error_listener.h>
#include <yql/essentials/parser/common/issue.h>
#include <yql/essentials/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Lexer.h>
#include <yql/essentials/parser/proto_ast/gen/v1_antlr4/SQLv1Antlr4Parser.h>
#include <yql/essentials/parser/proto_ast/gen/v1_ansi_antlr4/SQLv1Antlr4Lexer.h>
#include <yql/essentials/parser/proto_ast/gen/v1_ansi_antlr4/SQLv1Antlr4Parser.h>

#include <util/generic/yexception.h>

#include <limits>
#include <string_view>
#include <unordered_map>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using Ydb::Query::SqlV1ParserAst;
using NSQLv1Generated::TSQLv1ParserAST;
using TContext = antlr4::ParserRuleContext;
using TToken = antlr4::Token;
using google::protobuf::Descriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;

constexpr ui32 FormatVersion = 1;
constexpr size_t MaxRuleDepth = 512;
// Generated protobuf inserts oneof and repetition blocks between CST rules.
constexpr size_t MaxProtoDepth = 1024;
constexpr size_t MaxMessages = 400'000;

void Check(bool condition, const char* message) {
    if (!condition) {
        ythrow yexception() << "ANTLR AST: " << message;
    }
}

void CheckFields(const Message& node) {
    Check(node.GetReflection()->GetUnknownFields(node).empty(), "unknown protobuf fields");
}

template <class TLexer, class TParser>
void ParseAndExport(const TString& sql, const NSQLTranslation::TTranslationSettings& settings, SqlV1ParserAst& result) {
    NYql::TIssues issues;
    NSQLTranslation::TErrorCollectorOverIssues collector(issues, settings.MaxErrors, settings.File);
    bool lexerError = false;
    antlr4::ANTLRInputStream source(sql.data(), sql.size());
    TLexer lexer(&source);
    antlr4::CommonTokenStream tokens(&lexer);
    google::protobuf::Arena arena;
    TParser parser(&tokens, &arena);
    antlr4::YqlErrorListener lexerListener(&collector, &lexerError);
    antlr4::YqlErrorListener parserListener(&collector, &parser.error);
    NAntlrAST::TDepthLimitingListener depthLimit(MaxRuleDepth);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&lexerListener);
    parser.removeErrorListeners();
    parser.addErrorListener(&parserListener);
    parser.addParseListener(&depthLimit);
    parser.setBuildParseTree(false);
    const auto* ast = dynamic_cast<const TSQLv1ParserAST*>(parser.Parse(&collector));
    if (lexerError || !ast) {
        ythrow yexception() << "ANTLR AST: failed to parse query: " << issues.ToOneLineString();
    }
    ExportParserTokens(tokens, *result.mutable_tokens());
    result.mutable_ast()->CopyFrom(*ast);
}

class TRuleContext final : public TContext {
public:
    explicit TRuleContext(size_t ruleIndex)
        : RuleIndex(ruleIndex)
    {}

    size_t getRuleIndex() const override {
        return RuleIndex;
    }

private:
    const size_t RuleIndex;
};

const std::unordered_map<const Descriptor*, size_t>& RuleIndices() {
    static const auto ruleIndices = [] {
        // Both native lexer modes use the same parser rule names and ordering.
        const NKqpDefaultAntlr4::SQLv1Antlr4Parser parser(nullptr);
        const auto& ruleNames = parser.getRuleNames();
        auto* pool = TSQLv1ParserAST::descriptor()->file()->pool();
        std::unordered_map<const Descriptor*, size_t> result;
        result.reserve(ruleNames.size());
        for (size_t index = 0; index < ruleNames.size(); ++index) {
            const auto* descriptor = pool->FindMessageTypeByName("NSQLv1Generated.TRule_" + ruleNames[index]);
            Check(descriptor != nullptr, "generated rule descriptor not found");
            result.emplace(descriptor, index);
        }
        return result;
    }();
    return ruleIndices;
}

class TProtoCst {
public:
    explicit TProtoCst(std::vector<antlr4::CommonToken>& tokens)
        : Tokens(tokens)
        , Rules(RuleIndices())
    {
        SkipHidden();
    }

    TContext& Restore(const TSQLv1ParserAST& ast) {
        CheckFields(ast);
        Check(ast.HasRule_sql_query() && ast.GetStartTokenIndex() == ast.GetRule_sql_query().GetStartTokenIndex()
            && ast.GetEndTokenIndex() == ast.GetRule_sql_query().GetEndTokenIndex(), "invalid sql_query envelope");
        auto& result = RestoreRule(ast.GetRule_sql_query(), nullptr, 1);
        Check(Next == Tokens.size(), "unconsumed tokens");
        return result;
    }

private:
    void SkipHidden() {
        while (Next < Tokens.size() && Tokens[Next].getChannel() != TToken::DEFAULT_CHANNEL) {
            ++Next;
        }
    }

    TContext& RestoreRule(const Message& rule, TContext* parent, size_t depth) {
        const auto it = Rules.find(rule.GetDescriptor());
        Check(it != Rules.end(), "unknown grammar rule");
        auto context = std::make_unique<TRuleContext>(it->second);
        Check(Next < Tokens.size(), "rule starts after EOF");
        context->start = &Tokens[Next];
        context->parent = parent;
        RestoreChildren(rule, *context, depth);
        context->stop = Last;
        auto* result = context.get();
        Tree.push_back(std::move(context));
        return *result;
    }

    void RestoreChildren(const Message& node, TContext& context, size_t depth) {
        Check(depth <= MaxProtoDepth && ++Messages <= MaxMessages, "tree limit exceeded");
        CheckFields(node);
        const auto* descriptor = node.GetDescriptor();
        const auto* reflection = node.GetReflection();
        const auto* start = descriptor->FindFieldByName("StartTokenIndex");
        const auto* end = descriptor->FindFieldByName("EndTokenIndex");
        Check(start && end && Next < Tokens.size() && reflection->GetUInt32(node, start) == Next,
            "invalid rule or block start");
        const size_t begin = Next;
        if (const auto* alt = descriptor->FindOneofByName("Alt")) {
            Check(reflection->GetOneofFieldDescriptor(node, alt) != nullptr, "missing grammar alternative");
        }
        // Generated field numbers follow the production order. TBlock and TAlt
        // preserve choices and repetitions in protobuf but are invisible in ANTLR CST.
        for (int i = 0; i < descriptor->field_count(); ++i) {
            const auto* field = descriptor->field(i);
            if (field->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
                continue;
            }
            if (field->is_repeated()) {
                const auto count = reflection->FieldSize(node, field);
                for (int j = 0; j < count; ++j) {
                    RestoreChild(reflection->GetRepeatedMessage(node, field, j), *field, context, depth + 1);
                }
            } else if (reflection->HasField(node, field)) {
                RestoreChild(reflection->GetMessage(node, field), *field, context, depth + 1);
            }
        }
        const ui32 expectedEnd = Next == begin ? static_cast<ui32>(begin) - 1 : Last->getTokenIndex();
        Check(reflection->GetUInt32(node, end) == expectedEnd, "invalid rule or block end");
    }

    void RestoreChild(const Message& child, const FieldDescriptor& field, TContext& context, size_t depth) {
        if (const auto* terminal = dynamic_cast<const NSQLv1Generated::TToken*>(&child)) {
            CheckFields(*terminal);
            Check(Next < Tokens.size(), "terminal appears after EOF");
            auto* token = &Tokens[Next];
            const ui32 type = token->getType() == TToken::EOF ? std::numeric_limits<ui32>::max() : token->getType();
            Check(terminal->GetId() == type && terminal->GetElement() == static_cast<ui32>(field.number())
                && terminal->GetLine() == token->getLine() && terminal->GetColumn() == token->getCharPositionInLine()
                && std::string_view(terminal->GetValue().data(), terminal->GetValue().size()) == token->getText(),
                "terminal does not match the token stream");
            auto node = std::make_unique<antlr4::tree::TerminalNodeImpl>(token);
            context.addChild(node.get());
            Tree.push_back(std::move(node));
            Last = token;
            ++Next;
            SkipHidden();
        } else if (Rules.contains(child.GetDescriptor())) {
            context.addChild(&RestoreRule(child, &context, depth));
        } else {
            RestoreChildren(child, context, depth);
        }
    }

    std::vector<antlr4::CommonToken>& Tokens;
    const std::unordered_map<const Descriptor*, size_t>& Rules;
    std::vector<std::unique_ptr<antlr4::tree::ParseTree>> Tree;
    size_t Next = 0;
    size_t Messages = 0;
    TToken* Last = nullptr;
};

} // namespace

SqlV1ParserAst ExportAntlrAst(const TString& sql, NSQLTranslation::TTranslationSettings settings) {
    Check(sql.find('\0') == TString::npos, "embedded NUL is unsupported by generated proto terminals");
    SqlV1ParserAst result;
    result.set_format_version(FormatVersion);
    *result.mutable_metadata() = PrepareParserMetadata(sql, settings);
    result.mutable_metadata()->set_grammar_fingerprint(AntlrAstFingerprint, 32);
    if (settings.AnsiLexer) {
        ParseAndExport<NALPAnsiAntlr4::SQLv1Antlr4Lexer, NALPAnsiAntlr4::SQLv1Antlr4Parser>(sql, settings, result);
    } else {
        ParseAndExport<NALPDefaultAntlr4::SQLv1Antlr4Lexer, NALPDefaultAntlr4::SQLv1Antlr4Parser>(sql, settings, result);
    }
    return result;
}

std::unique_ptr<NNewYql::TScript> ImportAntlrAst(const SqlV1ParserAst& input) {
    CheckFields(input);
    Check(input.format_version() == FormatVersion && input.has_metadata() && input.has_ast(), "invalid envelope");
    auto imported = ImportParserTokens(input.metadata(), input.tokens(), std::string_view(AntlrAstFingerprint, 32));
    TProtoCst tree(imported.Tokens);
    auto& root = tree.Restore(input.ast());
    ValidateAntlrRuleShapes(root, imported.AnsiLexer);
    return BuildNewYqlAst(root, imported.AnsiLexer, imported.File);
}

} // namespace NKikimr::NKqp::NParsedQueryBench
