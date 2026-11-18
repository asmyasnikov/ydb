LIBRARY()

SRCS(${ARCADIA_ROOT}/ydb/core/kqp/host/new_parser/kqp_ast_builder.cpp)

PEERDIR(
    contrib/libs/antlr4_cpp_runtime
    ydb/core/kqp/host/new_parser/grammar/ansi
    ydb/core/kqp/host/new_parser/grammar/default
    yql/essentials/public/issue
    yql/essentials/sql/settings
)

END()
