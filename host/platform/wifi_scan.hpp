#pragma once

// The Wi-Fi access points around this machine, for a game's access point
// scan (sceNetApctlScanUser). Linux asks NetworkManager (nmcli), which
// rescans when its list is older than 30 s, and otherwise reads the kernel's
// last scan results (nl80211); Windows reads the WLAN service's list. It can
// take a few seconds. Nothing is sent anywhere: the game only reads the list.
// Elsewhere, or without a wireless adapter, the list is empty.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace portablekit::platform {

struct AccessPoint {
    std::array<std::uint8_t, 6> bssid{};
    std::string ssid;             // up to 32 bytes, not always text
    std::uint32_t channel{};      // 0 when unknown
    std::uint8_t signal{};        // 0 to 100
    bool privacy{};               // encrypted
    bool wpa{};                   // WPA or WPA2 (RSN) rather than WEP, when encrypted
};

// Strongest first.
[[nodiscard]] std::vector<AccessPoint> scan_wifi();

} // namespace portablekit::platform
