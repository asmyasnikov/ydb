LIBRARY()

SRCS(
    antlr_ast_codec.cpp
    cst_codec.cpp
    script_codec.cpp
)

SET(GRAMMAR_DIR ${ARCADIA_ROOT}/ydb/core/kqp/host/new_parser/grammar)
SET(SCHEMA_DIR ${ARCADIA_BUILD_ROOT}/yql/essentials/parser/proto_ast/gen/v1_proto_split_antlr4)
SET(GRAMMAR_MANIFEST
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    ${GRAMMAR_DIR}/default/ya.make
    ${GRAMMAR_DIR}/ansi/ya.make
    ${ARCADIA_ROOT}/yql/essentials/parser/antlr_ast/org/antlr/v4/tool/templates/codegen/Cpp/Cpp.stg
    ${ARCADIA_ROOT}/yql/essentials/parser/antlr_ast/org/antlr/v4/tool/templates/codegen/Cpp/Files.stg
    ${ARCADIA_ROOT}/ydb/core/kqp/host/new_parser/kqp_ast_builder.cpp
    ${SCHEMA_DIR}/SQLv1Antlr4Parser.proto
)
RUN_PYTHON3(
    grammar_fingerprint.py
    ${BINDIR}/grammar_fingerprint.h GrammarFingerprint ${GRAMMAR_MANIFEST}
    IN ${GRAMMAR_MANIFEST}
    OUT_NOAUTO grammar_fingerprint.h
)

SET(ANTLR_AST_MANIFEST
    ${ARCADIA_ROOT}/yql/essentials/sql/v1/SQLv1Antlr4.g.in
    ${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/gen/v1_antlr4/ya.make
    ${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/gen/v1_ansi_antlr4/ya.make
    ${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/org/antlr/v4/tool/templates/codegen/Cpp/Cpp.stg.in
    ${ARCADIA_ROOT}/yql/essentials/parser/proto_ast/org/antlr/v4/tool/templates/codegen/Cpp/Files.stg.in
    ${SCHEMA_DIR}/SQLv1Antlr4Parser.proto
)
RUN_PYTHON3(
    grammar_fingerprint.py
    ${BINDIR}/antlr_ast_fingerprint.h AntlrAstFingerprint ${ANTLR_AST_MANIFEST}
    IN ${ANTLR_AST_MANIFEST}
    OUT_NOAUTO antlr_ast_fingerprint.h
)

PEERDIR(
    contrib/libs/antlr4_cpp_runtime
    contrib/libs/protobuf
    ydb/core/kqp/host/new_parser/grammar/ansi
    ydb/core/kqp/host/new_parser/grammar/default
    ydb/core/kqp/host/new_parser/syntax
    ydb/tests/tools/parsed_query_bench/proto
    yql/essentials/parser/common/antlr4
    yql/essentials/parser/proto_ast/gen/v1_antlr4
    yql/essentials/parser/proto_ast/gen/v1_ansi_antlr4
    yql/essentials/parser/proto_ast/gen/v1_proto_split_antlr4
    yql/essentials/public/issue
    yql/essentials/sql/settings
)

END()
