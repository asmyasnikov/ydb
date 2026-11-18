PY3TEST()

TEST_SRCS(test_roundtrip.py)
DEPENDS(ydb/tests/tools/parsed_query_bench)
PEERDIR(library/python/testing/yatest_common)

END()
