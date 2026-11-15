#include "platform/wifi_scan.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

#if defined(__linux__) && !defined(__ANDROID__)
#include <filesystem>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#include <wlanapi.h>
#endif

namespace portablekit::platform {
namespace {

[[maybe_unused]] std::uint32_t channel_of(std::uint32_t mhz) {
    if (mhz == 2484u) return 14u;
    if (mhz >= 2412u && mhz < 2484u) return (mhz - 2407u) / 5u;
    if (mhz >= 5000u && mhz < 5925u) return (mhz - 5000u) / 5u;
    if (mhz >= 5950u && mhz <= 7125u) return (mhz - 5950u) / 5u;
    return 0u;
}

// The beacon's information elements: the SSID (element 0), and WPA (vendor
// element 221 for 00:50:F2 type 1) or RSN (48).
[[maybe_unused]] void read_elements(const std::uint8_t *data, std::size_t size, AccessPoint &ap) {
    for (std::size_t i = 0; i + 2u <= size;) {
        const std::uint8_t id = data[i];
        const std::uint8_t length = data[i + 1u];
        if (i + 2u + length > size) break;
        const std::uint8_t *body = data + i + 2u;
        if (id == 0u && ap.ssid.empty())
            ap.ssid.assign(reinterpret_cast<const char *>(body), std::min<std::size_t>(length, 32u));
        if (id == 48u) ap.wpa = true;
        if (id == 221u && length >= 4u && body[0] == 0x00u && body[1] == 0x50u && body[2] == 0xF2u && body[3] == 1u)
            ap.wpa = true;
        i += 2u + length;
    }
}

#if defined(__linux__) && !defined(__ANDROID__)

// nl80211, from the kernel's uapi header.
constexpr std::uint8_t kCmdGetScan = 32u;
constexpr std::uint16_t kAttrIfindex = 3u;
constexpr std::uint16_t kAttrBss = 47u;
constexpr std::uint16_t kBssBssid = 1u;
constexpr std::uint16_t kBssFrequency = 2u;
constexpr std::uint16_t kBssCapability = 5u;
constexpr std::uint16_t kBssInformationElements = 6u;
constexpr std::uint16_t kBssSignalMbm = 7u;
constexpr std::uint16_t kBssSignalUnspec = 8u;

template <typename F> void for_each_attribute(const std::uint8_t *data, std::size_t size, F &&visit) {
    while (size >= NLA_HDRLEN) {
        nlattr header{};
        std::memcpy(&header, data, sizeof(header));
        if (header.nla_len < NLA_HDRLEN || header.nla_len > size) return;
        visit(static_cast<std::uint16_t>(header.nla_type & NLA_TYPE_MASK), data + NLA_HDRLEN,
              static_cast<std::size_t>(header.nla_len - NLA_HDRLEN));
        const std::size_t step = NLA_ALIGN(header.nla_len);
        if (step >= size) return;
        data += step;
        size -= step;
    }
}

template <typename T> T read_value(const std::uint8_t *data, std::size_t size) {
    T value{};
    std::memcpy(&value, data, std::min(size, sizeof(T)));
    return value;
}

void put_attribute(std::vector<std::uint8_t> &out, std::uint16_t type, const void *data, std::size_t size) {
    const nlattr header{static_cast<std::uint16_t>(NLA_HDRLEN + size), type};
    const std::size_t at = out.size();
    out.resize(at + NLA_ALIGN(NLA_HDRLEN + size), 0u);
    std::memcpy(out.data() + at, &header, sizeof(header));
    std::memcpy(out.data() + at + NLA_HDRLEN, data, size);
}

class Netlink {
public:
    Netlink() : fd_(socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC)) {
        if (fd_ < 0) return;
        const timeval timeout{1, 0};
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }
    ~Netlink() {
        if (fd_ >= 0) close(fd_);
    }
    Netlink(const Netlink &) = delete;
    Netlink &operator=(const Netlink &) = delete;

    // Sends one request and calls `reply` with the attributes of each answer,
    // until the dump ends, the request is acknowledged or it fails.
    template <typename F>
    bool request(std::uint16_t family, std::uint8_t command, std::uint16_t flags,
                 const std::vector<std::uint8_t> &attributes, F &&reply) {
        if (fd_ < 0) return false;
        std::vector<std::uint8_t> message(NLMSG_HDRLEN + GENL_HDRLEN + attributes.size(), 0u);
        if (!attributes.empty())
            std::memcpy(message.data() + NLMSG_HDRLEN + GENL_HDRLEN, attributes.data(), attributes.size());
        nlmsghdr header{};
        header.nlmsg_len = static_cast<std::uint32_t>(message.size());
        header.nlmsg_type = family;
        header.nlmsg_flags = static_cast<std::uint16_t>(NLM_F_REQUEST | flags);
        header.nlmsg_seq = ++sequence_;
        std::memcpy(message.data(), &header, sizeof(header));
        const genlmsghdr generic{command, 1u, 0u};
        std::memcpy(message.data() + NLMSG_HDRLEN, &generic, sizeof(generic));
        sockaddr_nl kernel{};
        kernel.nl_family = AF_NETLINK;
        if (sendto(fd_, message.data(), message.size(), 0, reinterpret_cast<const sockaddr *>(&kernel),
                   sizeof(kernel)) < 0)
            return false;
        std::vector<std::uint8_t> buffer(1u << 16u);
        for (;;) {
            const ssize_t got = recv(fd_, buffer.data(), buffer.size(), 0);
            if (got <= 0) return false;
            auto size = static_cast<std::size_t>(got);
            const std::uint8_t *at = buffer.data();
            while (size >= NLMSG_HDRLEN) {
                nlmsghdr answer{};
                std::memcpy(&answer, at, sizeof(answer));
                if (answer.nlmsg_len < NLMSG_HDRLEN || answer.nlmsg_len > size) return false;
                if (answer.nlmsg_seq == sequence_) {
                    if (answer.nlmsg_type == NLMSG_DONE) return true;
                    if (answer.nlmsg_type == NLMSG_ERROR)
                        return answer.nlmsg_len >= NLMSG_HDRLEN + sizeof(int) &&
                               read_value<int>(at + NLMSG_HDRLEN, sizeof(int)) == 0;
                    if (answer.nlmsg_len >= NLMSG_HDRLEN + GENL_HDRLEN)
                        for_each_attribute(at + NLMSG_HDRLEN + GENL_HDRLEN,
                                           answer.nlmsg_len - NLMSG_HDRLEN - GENL_HDRLEN, reply);
                }
                const std::size_t step = NLMSG_ALIGN(answer.nlmsg_len);
                if (step >= size) break;
                at += step;
                size -= step;
            }
        }
    }

private:
    int fd_;
    std::uint32_t sequence_{};
};

// NetworkManager rescans for any user when its list is older than 30 s, which
// takes a few seconds; the kernel's own scan needs privileges. Lines are
// BSSID:SSID-HEX:CHAN:SIGNAL:SECURITY, the BSSID's colons escaped.
std::vector<AccessPoint> scan_network_manager() {
    std::vector<AccessPoint> found;
    FILE *pipe = popen("nmcli -t -f BSSID,SSID-HEX,CHAN,SIGNAL,SECURITY device wifi list --rescan auto 2>/dev/null",
                       "r");
    if (pipe == nullptr) return found;
    char line[512];
    while (std::fgets(line, sizeof(line), pipe) != nullptr) {
        std::vector<std::string> fields(1);
        for (const char *c = line; *c != '\0' && *c != '\n'; ++c) {
            if (*c == '\\' && c[1] != '\0') fields.back() += *++c;
            else if (*c == ':') fields.emplace_back();
            else fields.back() += *c;
        }
        if (fields.size() < 5u) continue;
        AccessPoint ap;
        unsigned bytes[6];
        if (std::sscanf(fields[0].c_str(), "%x:%x:%x:%x:%x:%x", &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4],
                        &bytes[5]) != 6)
            continue;
        for (int i = 0; i < 6; ++i) ap.bssid[i] = static_cast<std::uint8_t>(bytes[i]);
        for (std::size_t i = 0; i + 1u < fields[1].size() && ap.ssid.size() < 32u; i += 2u)
            ap.ssid += static_cast<char>(std::stoi(fields[1].substr(i, 2u), nullptr, 16));
        ap.channel = static_cast<std::uint32_t>(std::atoi(fields[2].c_str()));
        ap.signal = static_cast<std::uint8_t>(std::clamp(std::atoi(fields[3].c_str()), 0, 100));
        ap.privacy = !fields[4].empty() && fields[4] != "--";
        ap.wpa = fields[4].find("WPA") != std::string::npos;
        found.push_back(std::move(ap));
    }
    pclose(pipe);
    return found;
}

// One NL80211_ATTR_BSS: an access point, when it has a BSSID.
std::optional<AccessPoint> read_bss(const std::uint8_t *data, std::size_t size) {
    AccessPoint ap;
    bool has_bssid = false;
    std::optional<std::uint8_t> unspec;
    std::optional<std::int32_t> mbm;
    for_each_attribute(data, size, [&](std::uint16_t field, const std::uint8_t *value, std::size_t length) {
        switch (field) {
        case kBssBssid:
            has_bssid = length >= 6u;
            if (has_bssid) std::memcpy(ap.bssid.data(), value, 6u);
            break;
        case kBssFrequency: ap.channel = channel_of(read_value<std::uint32_t>(value, length)); break;
        case kBssCapability: ap.privacy = (read_value<std::uint16_t>(value, length) & 0x10u) != 0u; break;
        case kBssInformationElements: read_elements(value, length, ap); break;
        case kBssSignalMbm: mbm = read_value<std::int32_t>(value, length); break;
        case kBssSignalUnspec: unspec = read_value<std::uint8_t>(value, length); break;
        default: break;
        }
    });
    if (!has_bssid) return std::nullopt;
    // dBm to a percentage: -100 dBm and below is 0, -50 and above 100.
    if (mbm) ap.signal = static_cast<std::uint8_t>(std::clamp(2 * (*mbm / 100 + 100), 0, 100));
    else if (unspec) ap.signal = std::min<std::uint8_t>(*unspec, 100u);
    return ap;
}

std::vector<AccessPoint> scan_linux() {
    std::vector<AccessPoint> found = scan_network_manager();
    if (!found.empty()) return found;
    Netlink netlink;
    std::uint16_t family = 0u;
    std::vector<std::uint8_t> name;
    put_attribute(name, CTRL_ATTR_FAMILY_NAME, "nl80211", 8u);
    netlink.request(GENL_ID_CTRL, CTRL_CMD_GETFAMILY, NLM_F_ACK, name,
                    [&](std::uint16_t type, const std::uint8_t *data, std::size_t size) {
                        if (type == CTRL_ATTR_FAMILY_ID) family = read_value<std::uint16_t>(data, size);
                    });
    if (family == 0u) return found;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/net", error)) {
        if (!std::filesystem::exists(entry.path() / "wireless", error)) continue;
        const std::uint32_t index = if_nametoindex(entry.path().filename().c_str());
        if (index == 0u) continue;
        std::vector<std::uint8_t> attributes;
        put_attribute(attributes, kAttrIfindex, &index, sizeof(index));
        netlink.request(family, kCmdGetScan, NLM_F_DUMP, attributes,
                        [&](std::uint16_t type, const std::uint8_t *data, std::size_t size) {
                            if (type != kAttrBss) return;
                            if (auto ap = read_bss(data, size)) found.push_back(std::move(*ap));
                        });
    }
    return found;
}

#elif defined(_WIN32)

std::vector<AccessPoint> scan_windows() {
    std::vector<AccessPoint> found;
    // Loaded when asked, so a machine without the WLAN service still starts.
    HMODULE library = LoadLibraryW(L"wlanapi.dll");
    if (library == nullptr) return found;
    using Open = DWORD(WINAPI *)(DWORD, PVOID, PDWORD, PHANDLE);
    using Close = DWORD(WINAPI *)(HANDLE, PVOID);
    using Enumerate = DWORD(WINAPI *)(HANDLE, PVOID, PWLAN_INTERFACE_INFO_LIST *);
    using BssList = DWORD(WINAPI *)(HANDLE, const GUID *, const PDOT11_SSID, DOT11_BSS_TYPE, BOOL, PVOID,
                                    PWLAN_BSS_LIST *);
    using Free = VOID(WINAPI *)(PVOID);
    const auto open = reinterpret_cast<Open>(GetProcAddress(library, "WlanOpenHandle"));
    const auto close = reinterpret_cast<Close>(GetProcAddress(library, "WlanCloseHandle"));
    const auto enumerate = reinterpret_cast<Enumerate>(GetProcAddress(library, "WlanEnumInterfaces"));
    const auto bss_list = reinterpret_cast<BssList>(GetProcAddress(library, "WlanGetNetworkBssList"));
    const auto free_memory = reinterpret_cast<Free>(GetProcAddress(library, "WlanFreeMemory"));
    HANDLE handle = nullptr;
    DWORD version = 0;
    if (open == nullptr || close == nullptr || enumerate == nullptr || bss_list == nullptr || free_memory == nullptr ||
        open(2u, nullptr, &version, &handle) != ERROR_SUCCESS) {
        FreeLibrary(library);
        return found;
    }
    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (enumerate(handle, nullptr, &interfaces) == ERROR_SUCCESS) {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i) {
            PWLAN_BSS_LIST list = nullptr;
            if (bss_list(handle, &interfaces->InterfaceInfo[i].InterfaceGuid, nullptr, dot11_BSS_type_any, FALSE,
                         nullptr, &list) != ERROR_SUCCESS)
                continue;
            for (DWORD j = 0; j < list->dwNumberOfItems; ++j) {
                const WLAN_BSS_ENTRY &entry = list->wlanBssEntries[j];
                AccessPoint ap;
                std::memcpy(ap.bssid.data(), entry.dot11Bssid, 6u);
                ap.ssid.assign(reinterpret_cast<const char *>(entry.dot11Ssid.ucSSID),
                               std::min<ULONG>(entry.dot11Ssid.uSSIDLength, 32u));
                ap.channel = channel_of(entry.ulChCenterFrequency / 1000u);
                ap.signal = static_cast<std::uint8_t>(std::min<ULONG>(entry.uLinkQuality, 100u));
                ap.privacy = (entry.usCapabilityInformation & 0x10u) != 0u;
                read_elements(reinterpret_cast<const std::uint8_t *>(&entry) + entry.ulIeOffset, entry.ulIeSize, ap);
                found.push_back(std::move(ap));
            }
            free_memory(list);
        }
        free_memory(interfaces);
    }
    close(handle, nullptr);
    FreeLibrary(library);
    return found;
}

#endif

} // namespace

std::vector<AccessPoint> scan_wifi() {
#if defined(__linux__) && !defined(__ANDROID__)
    std::vector<AccessPoint> found = scan_linux();
#elif defined(_WIN32)
    std::vector<AccessPoint> found = scan_windows();
#else
    std::vector<AccessPoint> found;
#endif
    // One entry per access point (an adapter may see it twice), strongest first.
    std::stable_sort(found.begin(), found.end(),
                     [](const AccessPoint &a, const AccessPoint &b) { return a.signal > b.signal; });
    std::vector<AccessPoint> unique;
    for (AccessPoint &ap : found)
        if (std::none_of(unique.begin(), unique.end(), [&](const AccessPoint &u) { return u.bssid == ap.bssid; }))
            unique.push_back(std::move(ap));
    return unique;
}

} // namespace portablekit::platform
