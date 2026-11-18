PROGRAM(parsed_query_native_bench)

SRCS(
    catalog.cpp
    main.cpp
    native_codec.cpp
)

PEERDIR(
    contrib/libs/protobuf
    library/cpp/json
    ydb/core/kqp/expr_nodes
    ydb/core/kqp/host/new_parser
    ydb/core/kqp/host/new_parser/rbo
    ydb/core/kqp/opt
    ydb/core/kqp/opt/rbo
    ydb/core/kqp/provider
    ydb/public/api/protos
    ydb/tests/tools/parsed_query_bench/proto
    ydb/tests/tools/parsed_query_bench/syntax
    yql/essentials/ast
    yql/essentials/core
    yql/essentials/core/type_ann
    yql/essentials/minikql
    yql/essentials/minikql/invoke_builtins/llvm16
    yql/essentials/parser/pg_wrapper
    yql/essentials/providers/common/udf_resolve
    yql/essentials/public/udf
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(ut)
