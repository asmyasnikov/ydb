LIBRARY()

PEERDIR(
    library/cpp/blockcodecs/core
    library/cpp/blockcodecs/codecs/brotli
    library/cpp/blockcodecs/codecs/bzip
    library/cpp/blockcodecs/codecs/fastlz
    library/cpp/blockcodecs/codecs/lz4
    library/cpp/blockcodecs/codecs/lzma
    library/cpp/blockcodecs/codecs/snappy
    library/cpp/blockcodecs/codecs/zlib
    library/cpp/blockcodecs/codecs/zstd
    library/cpp/streams/brotli
    library/cpp/streams/bzip2
    library/cpp/streams/lzma
)

SRCS(
    chunk.cpp
    compression.cpp
    headers.cpp
    stream.cpp
)

END()

RECURSE(
    list_codings
)

RECURSE_FOR_TESTS(
    benchmark
    fuzz
    ut
)
