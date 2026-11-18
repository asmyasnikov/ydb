PY3TEST()

TEST_SRCS(test_native.py)
DEPENDS(ydb/tests/tools/parsed_query_bench/native)
PEERDIR(
    library/python/testing/yatest_common
    ydb/public/api/protos
)

END()
