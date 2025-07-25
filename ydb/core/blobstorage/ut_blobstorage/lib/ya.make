LIBRARY()

SRCS(
    activity.h
    common.h
    defs.h
    debug_log.cpp
    env.h
    node_warden_mock_bsc.cpp
    node_warden_mock.h
    node_warden_mock_pipe.cpp
    node_warden_mock_state.cpp
    node_warden_mock_state.h
    node_warden_mock_vdisk.h
    ut_helpers.cpp
)

PEERDIR(
    library/cpp/digest/md5
    library/cpp/testing/unittest
    ydb/apps/version
    ydb/core/base
    ydb/core/blob_depot
    ydb/core/blobstorage/backpressure
    ydb/core/blobstorage/dsproxy/mock
    ydb/core/blobstorage/nodewarden
    ydb/core/blobstorage/pdisk
    ydb/core/blobstorage/pdisk/mock
    ydb/core/blobstorage/vdisk/common
    ydb/core/mind
    ydb/core/mind/bscontroller
    ydb/core/mind/hive
    ydb/core/sys_view/service
    ydb/core/tx/scheme_board
    ydb/core/tx/tx_allocator
    ydb/core/tx/mediator
    ydb/core/tx/coordinator
    ydb/core/tx/scheme_board
    ydb/core/util
    yql/essentials/minikql/comp_nodes/llvm16
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql/pg_dummy
    ydb/core/util/actorsys_test
)

END()
