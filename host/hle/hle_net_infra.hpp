#pragma once

namespace portablekit {

class HleRegistrar;

// Infrastructure networking through the host (hle_net_infra.cpp).
// Off with <prefix>_NET_OFFLINE=1, which leaves hle_net_offline.cpp's answers.
[[nodiscard]] bool net_infra_enabled();
// Joins the access point, as the network configuration dialog does.
void net_infra_connect();
void register_net_infra(HleRegistrar &hle);

} // namespace portablekit
