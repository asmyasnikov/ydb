#pragma once

#include <yt/yt/core/rpc/public.h>

namespace NYT::NApi {

////////////////////////////////////////////////////////////////////////////////

NYT::NRpc::IChannelPtr CreateTargetClusterInjectingChannel(
    NYT::NRpc::IChannelPtr underlying,
    std::optional<std::string> cluster);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi
