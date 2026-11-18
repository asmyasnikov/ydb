#pragma once

#include <ydb/tests/tools/parsed_query_bench/proto/syntax_script.pb.h>

#include <ydb/core/kqp/host/new_parser/kqp_ast.h>

namespace NKikimr::NKqp::NParsedQueryBench {

Ydb::Query::SyntaxScript ExportScript(const NNewYql::TScript& script);
void ValidateSyntaxScriptLimits(const Ydb::Query::SyntaxScript& script);
std::unique_ptr<NNewYql::TScript> ImportScript(const Ydb::Query::SyntaxScript& script);

} // namespace NKikimr::NKqp::NParsedQueryBench
