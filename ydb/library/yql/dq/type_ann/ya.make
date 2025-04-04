LIBRARY()

PEERDIR(
    yql/essentials/ast
    yql/essentials/core
    yql/essentials/core/type_ann
    ydb/library/yql/dq/expr_nodes
    ydb/library/yql/dq/proto
    yql/essentials/providers/common/provider
)

SRCS(
    dq_type_ann.cpp
)

YQL_LAST_ABI_VERSION()

GENERATE_ENUM_SERIALIZATION(dq_type_ann.h)

END()
