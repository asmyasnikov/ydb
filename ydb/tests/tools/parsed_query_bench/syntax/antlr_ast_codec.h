#pragma once

#include <ydb/core/kqp/host/new_parser/kqp_ast.h>
#include <ydb/tests/tools/parsed_query_bench/proto/antlr_ast.pb.h>
#include <yql/essentials/sql/settings/translation_settings.h>

namespace NKikimr::NKqp::NParsedQueryBench {

Ydb::Query::SqlV1ParserAst ExportAntlrAst(const TString& sql, NSQLTranslation::TTranslationSettings settings);
std::unique_ptr<NNewYql::TScript> ImportAntlrAst(const Ydb::Query::SqlV1ParserAst& input);

} // namespace NKikimr::NKqp::NParsedQueryBench
