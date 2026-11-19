#pragma once

#include <ydb/core/kqp/host/new_parser/kqp_ast.h>
#include <ydb/tests/tools/parsed_query_bench/proto/antlr_cst.pb.h>
#include <yql/essentials/sql/settings/translation_settings.h>

#include <contrib/libs/antlr4_cpp_runtime/src/antlr4-runtime.h>

#include <memory>
#include <string_view>
#include <vector>

namespace Ydb::Query {
class SyntaxScript;
}

namespace NKikimr::NKqp::NParsedQueryBench {

using TParserTokenFields = google::protobuf::RepeatedPtrField<Ydb::Query::YqlParserToken>;

struct TImportedParserTokens {
    bool AnsiLexer;
    TString File;
    std::vector<antlr4::CommonToken> Tokens;
};

Ydb::Query::YqlParserMetadata PrepareParserMetadata(const TString& sql, NSQLTranslation::TTranslationSettings& settings);
void ExportParserTokens(antlr4::CommonTokenStream& tokens, TParserTokenFields& output);
TImportedParserTokens ImportParserTokens(const Ydb::Query::YqlParserMetadata& metadata, const TParserTokenFields& tokens,
    std::string_view grammarFingerprint);

void ValidateAntlrRuleShapes(antlr4::ParserRuleContext& root, bool ansiLexer);
std::unique_ptr<NNewYql::TScript> ParseText(const TString& sql, NSQLTranslation::TTranslationSettings settings);
Ydb::Query::SyntaxScript ExportSyntaxScript(const TString& sql, NSQLTranslation::TTranslationSettings settings);
Ydb::Query::AntlrCst ExportAntlrCst(const TString& sql, NSQLTranslation::TTranslationSettings settings);
std::unique_ptr<NNewYql::TScript> ImportAntlrCst(const Ydb::Query::AntlrCst& cst);

} // namespace NKikimr::NKqp::NParsedQueryBench
