PROGRAM(parsed_query_bench)

SRCS(main.cpp)

PEERDIR(
    ydb/tests/tools/parsed_query_bench/syntax
)

END()

RECURSE_FOR_TESTS(ut)
