LIBRARY()

SRCS(
    msgbus_client.cpp
    msgbus_client.h
    msgbus_client_config.h
    grpc_client.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/src/library/grpc/client
    library/cpp/messagebus
    ydb/public/lib/base
)

END()
