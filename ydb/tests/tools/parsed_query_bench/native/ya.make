PROGRAM(parsed_query_native_bench)

SRCS(
    catalog.cpp
    host_text.cpp
    logical_signature.cpp
    main.cpp
    native_codec.cpp
)

PEERDIR(
    contrib/libs/protobuf
    library/cpp/json
    ydb/core/kqp/executer_actor
    ydb/core/kqp/expr_nodes
    ydb/core/kqp/gateway
    ydb/core/kqp/host
    ydb/core/kqp/host/new_parser
    ydb/core/kqp/host/new_parser/rbo
    ydb/core/kqp/node_service
    ydb/core/kqp/opt
    ydb/core/kqp/opt/rbo
    ydb/core/kqp/provider
    ydb/core/kqp/session_actor
    ydb/core/scheme
    ydb/core/testlib/basics/core
    ydb/core/tx/columnshard/data_accessor/local_db
    ydb/core/tx/columnshard/data_sharing/manager
    ydb/core/tx/columnshard/engines/reader/common_reader/iterator
    ydb/core/tx/columnshard/engines/storage/indexes/count_min_sketch
    ydb/core/tx/columnshard/engines/storage/indexes/max
    ydb/core/tx/columnshard/engines/storage/indexes/min_max
    ydb/core/tx/time_cast
    ydb/library/yql/providers/s3/provider
    ydb/public/api/protos
    ydb/tests/tools/parsed_query_bench/proto
    ydb/tests/tools/parsed_query_bench/syntax
    yql/essentials/ast
    yql/essentials/core
    yql/essentials/core/services/mounts
    yql/essentials/core/type_ann
    yql/essentials/minikql
    yql/essentials/minikql/invoke_builtins/llvm16
    yql/essentials/parser/pg_wrapper
    yql/essentials/providers/common/arrow_resolve
    yql/essentials/providers/common/udf_resolve
    yql/essentials/public/udf
    yql/essentials/sql/pg
    yt/yql/providers/yt/comp_nodes/dq/llvm16
    yt/yql/providers/yt/comp_nodes/llvm16
)

RESOURCE(
    ydb/core/kqp/kqp_default_settings.txt kqp_default_settings.txt
)

YQL_LAST_ABI_VERSION()

END()

RECURSE_FOR_TESTS(ut)
