#include "cst_codec.h"
#include "script_codec.h"
#include <ydb/tests/tools/parsed_query_bench/syntax/grammar_fingerprint.h>

#include <ydb/core/kqp/host/new_parser/grammar/ansi/SQLv1Antlr4Lexer.h>
#include <ydb/core/kqp/host/new_parser/grammar/ansi/SQLv1Antlr4Parser.h>
#include <ydb/core/kqp/host/new_parser/grammar/default/SQLv1Antlr4Lexer.h>
#include <ydb/core/kqp/host/new_parser/grammar/default/SQLv1Antlr4Parser.h>
#include <ydb/core/kqp/host/new_parser/kqp_ast_builder.h>

#include <yql/essentials/parser/common/antlr4/depth_limiting_listener.h>
#include <yql/essentials/parser/common/antlr4/error_listener.h>
#include <yql/essentials/parser/common/issue.h>

#include <util/charset/utf8.h>
#include <util/generic/yexception.h>

#include <string_view>
#include <unordered_map>
#include <vector>

namespace NKikimr::NKqp::NParsedQueryBench {
namespace {

using Ydb::Query::AntlrCst;
using Ydb::Query::YqlParserMetadata;

using TTree = antlr4::tree::ParseTree;
using TTerminal = antlr4::tree::TerminalNode;
using TContext = antlr4::ParserRuleContext;
using TToken = antlr4::Token;
using TState = antlr4::atn::ATNState;

constexpr unsigned FormatVersion = 1;
constexpr size_t MaxTokens = 100'000;
constexpr size_t MaxNodes = 400'000;
constexpr size_t MaxSourceBytes = 1 << 20;
constexpr size_t MaxTreeDepth = 512;

void Check(bool condition, const char* message) {
    if (!condition) {
        ythrow yexception() << "CST: " << message;
    }
}

void CheckFields(const google::protobuf::Message& message) {
    Check(message.GetReflection()->GetUnknownFields(message).empty(), "unknown protobuf fields");
}

bool IsHint(std::string_view comment) {
    return comment.starts_with("/*+") || comment.starts_with("--+");
}

void ExportToken(const TToken& token, Ydb::Query::YqlParserToken& result) {
    const bool eof = token.getType() == TToken::EOF;
    result.set_type(eof ? -1 : static_cast<int>(token.getType()));
    result.set_text(token.getText());
    result.set_channel(token.getChannel());
    result.set_line(token.getLine());
    result.set_column(token.getCharPositionInLine());
    result.set_begin(token.getStartIndex());
    result.set_end(eof ? token.getStartIndex() : token.getStopIndex() + 1);
}

unsigned ExportNode(TTree& tree, AntlrCst& result, size_t depth) {
    Check(depth <= MaxTreeDepth && static_cast<size_t>(result.nodes_size()) < MaxNodes, "tree limit exceeded");
    auto* node = result.add_nodes();
    const unsigned id = result.nodes_size();
    node->set_id(id);
    if (auto* context = dynamic_cast<TContext*>(&tree)) {
        auto* rule = node->mutable_rule();
        rule->set_rule_index(context->getRuleIndex());
        rule->set_start_token_index(context->getStart()->getTokenIndex());
        if (auto* stop = context->getStop()) {
            rule->set_stop_token_index(stop->getTokenIndex());
        }
        for (auto* child : context->children) {
            rule->add_children(ExportNode(*child, result, depth + 1));
        }
    } else {
        auto* terminal = dynamic_cast<TTerminal*>(&tree);
        Check(terminal && !dynamic_cast<antlr4::tree::ErrorNode*>(&tree), "expected a valid terminal");
        node->set_token_index(terminal->getSymbol()->getTokenIndex());
    }
    return id;
}

template <class TLexer, class TParser, class TConsume>
auto WithParse(const TString& sql, const NSQLTranslation::TTranslationSettings& settings, TConsume&& consume) {
    NYql::TIssues issues;
    NSQLTranslation::TErrorCollectorOverIssues collector(issues, settings.MaxErrors, settings.File);
    bool hasErrors = false;
    antlr4::YqlErrorListener errorListener(&collector, &hasErrors);
    NAntlrAST::TDepthLimitingListener depthLimiter(4096);
    antlr4::ANTLRInputStream input(sql);
    TLexer lexer(&input);
    antlr4::CommonTokenStream tokens(&lexer);
    TParser parser(&tokens);
    lexer.removeErrorListeners();
    lexer.addErrorListener(&errorListener);
    parser.removeErrorListeners();
    parser.addErrorListener(&errorListener);
    parser.addParseListener(&depthLimiter);
    parser.setBuildParseTree(true);
    auto* root = parser.sql_query();
    Check(!hasErrors && root, "failed to parse query");
    return consume(*root, tokens);
}

template <class TConsume>
auto WithParse(const TString& sql, const NSQLTranslation::TTranslationSettings& settings, TConsume&& consume) {
    if (settings.AnsiLexer) {
        return WithParse<NKqpAnsiAntlr4::SQLv1Antlr4Lexer, NKqpAnsiAntlr4::SQLv1Antlr4Parser>(sql, settings, consume);
    }
    return WithParse<NKqpDefaultAntlr4::SQLv1Antlr4Lexer, NKqpDefaultAntlr4::SQLv1Antlr4Parser>(sql, settings, consume);
}

class TRestoredContext final : public TContext {
public:
    explicit TRestoredContext(size_t rule)
        : Rule(rule)
    {
    }

    size_t getRuleIndex() const override {
        return Rule;
    }

private:
    size_t Rule;
};

struct TSourcePosition {
    size_t Offset = 0;
    size_t Line = 1;
    size_t Column = 0;

    void Advance(std::string_view text) {
        const auto* cursor = reinterpret_cast<const unsigned char*>(text.data());
        const auto* end = cursor + text.size();
        while (cursor != end) {
            wchar32 rune;
            size_t length;
            Check(SafeReadUTF8Char<StrictUTF8::Yes>(rune, length, cursor, end) == RECODE_OK, "invalid token UTF-8");
            cursor += length;
            ++Offset;
            if (rune == '\n') {
                ++Line;
                Column = 0;
            } else {
                ++Column;
            }
        }
    }
};

template <class TParser>
const TParser& Grammar() {
    static const TParser parser(nullptr);
    return parser;
}

template <class TParser>
std::vector<antlr4::CommonToken> RestoreTokens(const TParserTokenFields& input, const TParser& parser) {
    std::vector<antlr4::CommonToken> tokens;
    tokens.reserve(input.size());
    size_t bytes = 0;
    TSourcePosition position;
    for (int i = 0; i != input.size(); ++i) {
        const auto& token = input.Get(i);
        CheckFields(token);
        const bool eof = i + 1 == input.size();
        Check(eof ? token.type() == -1 && token.text() == "<EOF>"
                  : token.type() > 0 && static_cast<size_t>(token.type()) <= parser.getATN().maxTokenType
                      && !token.text().empty(), "invalid token or EOF");
        const size_t type = eof ? TToken::EOF : static_cast<size_t>(token.type());
        const bool hidden = type == TParser::TOKEN_WS || type == TParser::TOKEN_COMMENT;
        Check(token.channel() == (hidden ? TToken::HIDDEN_CHANNEL : TToken::DEFAULT_CHANNEL),
            "unexpected token channel");
        if (type == TParser::TOKEN_WS) {
            Check(token.text().size() == 1 && std::string_view(" \r\t\f\n").find(token.text()[0]) != std::string_view::npos,
                "invalid whitespace token");
        } else if (type == TParser::TOKEN_COMMENT) {
            std::string_view comment(token.text());
            Check((comment.starts_with("--") || (comment.starts_with("/*") && comment.ends_with("*/")))
                && !IsHint(comment), "invalid or unsupported comment");
        } else if (!eof) {
            const auto literal = parser.getVocabulary().getLiteralName(type);
            if (literal.size() > 2 && literal.front() == '\'' && literal.back() == '\''
                && literal.find('\\') == std::string_view::npos) {
                Check(std::string_view(literal).substr(1, literal.size() - 2) == token.text(), "invalid literal token");
            }
        }
        Check(token.begin() == position.Offset && token.line() == position.Line && token.column() == position.Column,
            "discontinuous token positions");
        if (!eof) {
            bytes += token.text().size();
            Check(bytes <= MaxSourceBytes, "source byte limit exceeded");
            position.Advance(token.text());
        }
        Check(token.end() == position.Offset, "invalid token end");
        auto& restored = tokens.emplace_back(type, std::string(token.text().data(), token.text().size()));
        restored.setChannel(token.channel());
        restored.setLine(token.line());
        restored.setCharPositionInLine(token.column());
        restored.setStartIndex(token.begin());
        restored.setStopIndex(token.end() - 1);
        restored.setTokenIndex(i);
    }
    return tokens;
}

class TRestore {
public:
    TRestore(const AntlrCst& input, std::vector<antlr4::CommonToken>& tokens)
        : Input(input)
        , Tokens(tokens)
        , Seen(input.nodes_size())
    {
        Visible.reserve(Tokens.size());
        for (size_t i = 0; i != Tokens.size(); ++i) {
            if (Tokens[i].getChannel() == TToken::DEFAULT_CHANNEL) {
                Visible.push_back(i);
            }
        }
        Index.reserve(input.nodes_size());
        for (int i = 0; i != input.nodes_size(); ++i) {
            const auto& node = input.nodes(i);
            Check(node.id() != 0 && Index.emplace(node.id(), i).second, "missing or duplicate node ID");
        }
    }

    TContext& Restore(size_t rootRule) {
        const auto& root = Lookup(Input.root_node_id());
        Check(root.has_rule() && root.rule().rule_index() == rootRule, "expected the root sql_query rule");
        auto* result = static_cast<TContext*>(RestoreNode(Input.root_node_id(), 1));
        Check(NextVisible == Visible.size() && Restored.size() == Index.size(), "unreachable nodes or unconsumed tokens");
        return *result;
    }

private:
    const AntlrCst::Node& Lookup(unsigned id) const {
        const auto it = Index.find(id);
        Check(it != Index.end(), "unknown node reference");
        return Input.nodes(it->second);
    }

    TTree* RestoreNode(unsigned id, size_t depth) {
        Check(depth <= MaxTreeDepth, "tree depth limit exceeded");
        const auto it = Index.find(id);
        Check(it != Index.end() && !Seen[it->second], "invalid or repeated node reference");
        Seen[it->second] = true;
        const auto& node = Input.nodes(it->second);
        CheckFields(node);
        if (node.has_token_index()) {
            Check(NextVisible < Visible.size() && node.token_index() == Visible[NextVisible],
                "terminal omitted, hidden, duplicated or out of order");
            ++NextVisible;
            auto terminal = std::make_unique<antlr4::tree::TerminalNodeImpl>(&Tokens[node.token_index()]);
            return Restored.emplace_back(std::move(terminal)).get();
        }
        Check(node.has_rule(), "missing node kind");
        const auto& rule = node.rule();
        CheckFields(rule);
        Check(NextVisible < Visible.size() && rule.start_token_index() == Visible[NextVisible], "invalid rule start");
        auto context = std::make_unique<TRestoredContext>(rule.rule_index());
        context->start = &Tokens[rule.start_token_index()];
        context->children.reserve(rule.children_size());
        for (auto childId : rule.children()) {
            auto* child = RestoreNode(childId, depth + 1);
            child->parent = context.get();
            context->children.push_back(child);
        }
        Check(rule.has_stop_token_index() == (NextVisible != 0), "missing or unexpected rule stop");
        if (rule.has_stop_token_index()) {
            Check(rule.stop_token_index() == Visible[NextVisible - 1], "invalid rule stop");
            context->stop = &Tokens[rule.stop_token_index()];
        }
        return Restored.emplace_back(std::move(context)).get();
    }

    const AntlrCst& Input;
    std::unordered_map<unsigned, int> Index;
    std::vector<antlr4::CommonToken>& Tokens;
    std::vector<unsigned> Visible;
    std::vector<bool> Seen;
    std::vector<std::unique_ptr<TTree>> Restored;
    size_t NextVisible = 0;
};

class TRuleShapes {
public:
    explicit TRuleShapes(const antlr4::atn::ATN& atn)
        : Atn(atn)
        , Visited(Atn.states.size())
    {
    }

    void Validate(TContext& root) {
        std::vector<TContext*> stack{&root};
        while (!stack.empty()) {
            auto* context = stack.back();
            stack.pop_back();
            CheckShape(*context);
            for (auto* child : context->children) {
                if (auto* subrule = dynamic_cast<TContext*>(child)) {
                    stack.push_back(subrule);
                }
            }
        }
    }

private:
    // Match direct children to a valid path through this production. A rule
    // child consumes its rule-call edge and resumes at followState; each
    // child's production is checked independently.
    void CheckShape(TContext& context) {
        const auto rule = context.getRuleIndex();
        Check(rule < Atn.ruleToStartState.size(), "unknown rule index");
        const auto* stop = Atn.ruleToStopState[rule];
        Current.assign(1, Atn.ruleToStartState[rule]);
        for (size_t i = 0; i <= context.children.size(); ++i) {
            auto* child = i < context.children.size() ? context.children[i] : nullptr;
            const auto* subrule = dynamic_cast<const TContext*>(child);
            auto* terminal = dynamic_cast<TTerminal*>(child);
            ++Generation;
            Pending.clear();
            Next.clear();
            for (const auto* state : Current) {
                Enqueue(state);
            }
            for (size_t j = 0; j != Pending.size(); ++j) {
                const auto* state = Pending[j];
                if (state == stop) {
                    if (!child) {
                        return;
                    }
                    continue;
                }
                for (const auto& edge : state->transitions) {
                    using namespace antlr4::atn;
                    if (RuleTransition::is(*edge)) {
                        const auto& call = static_cast<const RuleTransition&>(*edge);
                        if (subrule && call.ruleIndex == subrule->getRuleIndex()) {
                            Next.push_back(call.followState);
                        }
                    } else if (edge->getTransitionType() == TransitionType::EPSILON) {
                        Enqueue(edge->target);
                    } else if (!edge->isEpsilon() && terminal
                        && edge->matches(terminal->getSymbol()->getType(), TToken::MIN_USER_TOKEN_TYPE, Atn.maxTokenType)) {
                        Next.push_back(edge->target);
                    }
                }
            }
            Current.swap(Next);
            Check(!Current.empty(), "children do not match the grammar rule");
        }
        Check(false, "rule did not reach its end state");
    }

    void Enqueue(const TState* state) {
        auto& stamp = Visited[state->stateNumber];
        if (stamp != Generation) {
            stamp = Generation;
            Pending.push_back(state);
        }
    }

    const antlr4::atn::ATN& Atn;
    size_t Generation = 0;
    std::vector<size_t> Visited;
    std::vector<const TState*> Pending;
    std::vector<const TState*> Current;
    std::vector<const TState*> Next;
};

} // namespace

YqlParserMetadata PrepareParserMetadata(const TString& sql, NSQLTranslation::TTranslationSettings& settings) {
    settings.InferSyntaxVersion = true;
    Check(sql.size() <= MaxSourceBytes && !sql.StartsWith("\xef\xbb\xbf"), "unsupported source size or UTF-8 BOM");
    NSQLTranslation::TParsedSettings parsed;
    NYql::TIssues issues;
    Check(NSQLTranslation::ParseTranslationSettingsFromComments(sql, parsed, issues) && parsed.ApplyTo(settings, issues),
        "invalid query directives");
    Check(!settings.PgParser && settings.SyntaxVersion == 1 && !settings.Syntax, "expected YQL syntax v1");
    YqlParserMetadata metadata;
    metadata.set_source_name(settings.File);
    metadata.set_lexer_mode(settings.AnsiLexer ? YqlParserMetadata::ANSI : YqlParserMetadata::DEFAULT);
    metadata.mutable_directives()->set_explicit_syntax_v1(parsed.HasSyntaxV1);
    metadata.mutable_directives()->set_explicit_ansi_lexer(parsed.HasAnsiLexer);
    return metadata;
}

void ExportParserTokens(antlr4::CommonTokenStream& tokens, TParserTokenFields& output) {
    auto allTokens = tokens.getTokens();
    Check(!allTokens.empty() && allTokens.size() <= MaxTokens && allTokens.back()->getType() == TToken::EOF,
        "incomplete token stream or token limit exceeded");
    output.Clear();
    output.Reserve(allTokens.size());
    for (const auto* token : allTokens) {
        auto* encoded = output.Add();
        ExportToken(*token, *encoded);
        Check(token->getChannel() != TToken::HIDDEN_CHANNEL || !IsHint(encoded->text()),
            "query hints are unsupported by this benchmark");
    }
}

Ydb::Query::SyntaxScript ExportSyntaxScript(const TString& sql, NSQLTranslation::TTranslationSettings settings) {
    const auto metadata = PrepareParserMetadata(sql, settings);
    return WithParse(sql, settings, [&](TContext& root, antlr4::CommonTokenStream& tokens) {
        for (const auto* token : tokens.getTokens()) {
            Check(token->getChannel() != TToken::HIDDEN_CHANNEL || !IsHint(token->getText()),
                "query hints are unsupported by this benchmark");
        }
        auto script = ExportScript(*BuildNewYqlAst(root, settings.AnsiLexer, settings.File));
        *script.mutable_directives() = metadata.directives();
        ValidateSyntaxScriptLimits(script);
        return script;
    });
}

AntlrCst ExportAntlrCst(const TString& sql, NSQLTranslation::TTranslationSettings settings) {
    AntlrCst result;
    result.set_format_version(FormatVersion);
    *result.mutable_metadata() = PrepareParserMetadata(sql, settings);
    result.mutable_metadata()->set_grammar_fingerprint(GrammarFingerprint, 32);
    WithParse(sql, settings, [&](TContext& root, antlr4::CommonTokenStream& tokens) {
        ExportParserTokens(tokens, *result.mutable_tokens());
        result.set_root_node_id(ExportNode(root, result, 1));
    });
    return result;
}

TImportedParserTokens ImportParserTokens(const YqlParserMetadata& metadata, const TParserTokenFields& tokens,
    std::string_view grammarFingerprint)
{
    CheckFields(metadata);
    CheckFields(metadata.directives());
    Check(metadata.grammar_fingerprint() == grammarFingerprint, "unknown grammar fingerprint");
    Check(metadata.lexer_mode() == YqlParserMetadata::DEFAULT || metadata.lexer_mode() == YqlParserMetadata::ANSI,
        "unsupported lexer mode");
    const bool ansi = metadata.lexer_mode() == YqlParserMetadata::ANSI;
    Check((ansi || !metadata.directives().explicit_ansi_lexer()) && metadata.hints().empty(),
        "inconsistent lexer directive or unsupported query hints");
    Check(metadata.source_name().size() <= 4096, "source name limit exceeded");
    TSourcePosition().Advance(metadata.source_name());
    Check(tokens.size() > 0 && static_cast<size_t>(tokens.size()) <= MaxTokens, "token limit exceeded");
    using TAnsiParser = NKqpAnsiAntlr4::SQLv1Antlr4Parser;
    using TDefaultParser = NKqpDefaultAntlr4::SQLv1Antlr4Parser;
    return {ansi, TString(metadata.source_name()), ansi
        ? RestoreTokens(tokens, Grammar<TAnsiParser>())
        : RestoreTokens(tokens, Grammar<TDefaultParser>())};
}

void ValidateAntlrRuleShapes(TContext& root, bool ansiLexer) {
    const auto& atn = ansiLexer
        ? Grammar<NKqpAnsiAntlr4::SQLv1Antlr4Parser>().getATN()
        : Grammar<NKqpDefaultAntlr4::SQLv1Antlr4Parser>().getATN();
    TRuleShapes(atn).Validate(root);
}

std::unique_ptr<NNewYql::TScript> ParseText(const TString& sql, NSQLTranslation::TTranslationSettings settings) {
    settings.InferSyntaxVersion = true;
    NYql::TIssues issues;
    Check(NSQLTranslation::ParseTranslationSettings(sql, settings, issues), "invalid query directives");
    Check(!settings.PgParser && settings.SyntaxVersion == 1 && !settings.Syntax, "expected YQL syntax v1");
    return WithParse(sql, settings, [&](TContext& root, antlr4::CommonTokenStream&) {
        return BuildNewYqlAst(root, settings.AnsiLexer, settings.File);
    });
}

std::unique_ptr<NNewYql::TScript> ImportAntlrCst(const AntlrCst& cst) {
    CheckFields(cst);
    Check(cst.format_version() == FormatVersion && cst.has_metadata(), "unsupported envelope version");
    Check(cst.nodes_size() > 0 && static_cast<size_t>(cst.nodes_size()) <= MaxNodes, "node limit exceeded");

    auto input = ImportParserTokens(cst.metadata(), cst.tokens(), std::string_view(GrammarFingerprint, 32));
    TRestore tree(cst, input.Tokens);
    auto& root = tree.Restore(input.AnsiLexer
        ? static_cast<size_t>(NKqpAnsiAntlr4::SQLv1Antlr4Parser::RuleSql_query)
        : static_cast<size_t>(NKqpDefaultAntlr4::SQLv1Antlr4Parser::RuleSql_query));
    ValidateAntlrRuleShapes(root, input.AnsiLexer);
    return BuildNewYqlAst(root, input.AnsiLexer, input.File);
}

} // namespace NKikimr::NKqp::NParsedQueryBench
