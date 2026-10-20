// Infrastructure networking over the host's own network: the access point is
// always joined, names resolve through the host, sockets are host sockets and
// HTTP requests are made from the host. It is registered after
// hle_net_offline.cpp and replaces it; <prefix>_NET_OFFLINE=1 keeps the
// offline answers instead.
//
// The game's original servers are usually gone. The profile's
// infrastructure_host and infrastructure_url hooks name the servers that
// stand in for them; every name the resolver is asked for and every URL a
// request is sent to passes through them.
//
// Written from the PSP SDK's public headers (pspnet_inet.h, pspnet_apctl.h,
// pspnet_resolver.h, psphttp.h) and the BSD socket conventions the PSP's
// network stack follows. <prefix>_TRACE_NET=1 logs every call.

#include "../profile.hpp"
#include "hle_common.hpp"
#include "hle_net_infra.hpp"

#include "adhoc/discovery.hpp"
#include "adhoc/sockets.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace portablekit {
namespace {

namespace net = adhoc::net;

// errno values, as the PSP's network stack reports them: newlib's numbering,
// not BSD's. Its PSP errors are 0x80010000 with the errno.
constexpr std::uint32_t kEBadF = 9u;
constexpr std::uint32_t kEInval = 22u;
constexpr std::uint32_t kEWouldBlock = 11u;
constexpr std::uint32_t kEInProgress = 119u;
constexpr std::uint32_t kENotConn = 128u;
constexpr std::uint32_t kETimedOut = 116u;

// The error ranges of the libraries; exact values are not documented.
constexpr std::uint32_t kResolverFailed = 0x80410408u;
constexpr std::uint32_t kHttpFailed = 0x80431100u;
constexpr std::uint32_t kHttpUnknownId = 0x80431007u;

// pspnet_apctl.h
constexpr std::uint32_t kApctlDisconnected = 0u;
constexpr std::uint32_t kApctlJoining = 2u;
constexpr std::uint32_t kApctlGettingIp = 3u;
constexpr std::uint32_t kApctlGotIp = 4u;
constexpr std::uint32_t kApctlEventConnectRequest = 0u;
constexpr std::uint32_t kApctlEventEstablished = 3u;
constexpr std::uint32_t kApctlEventGetIp = 4u;
constexpr std::uint32_t kApctlEventDisconnectRequest = 5u;

bool tracing() {
    static const bool on = [] {
        const char *text = portablekit::env("TRACE_NET");
        return text != nullptr && *text != '\0' && *text != '0';
    }();
    return on;
}

void trace(const std::string &line) {
    if (tracing()) std::cerr << "[net] " << line << std::endl;
}

// <prefix>_NET_CAPTURE=<file> writes every payload the game sends or receives,
// one line each: milliseconds since the first, the socket, '>' sent, '<'
// received or '@' connected, the peer ('-' for a connected socket's own),
// then the bytes in hex. Enough to study a protocol without a packet sniffer.
void capture(std::uint32_t id, char direction, const std::string &peer, std::span<const std::uint8_t> bytes) {
    static FILE *const file = [] {
        const char *path = portablekit::env("NET_CAPTURE");
        return path != nullptr && *path != '\0' ? std::fopen(path, "w") : nullptr;
    }();
    if (file == nullptr) return;
    static const auto start = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    std::fprintf(file, "%lld %u %c %s ", static_cast<long long>(ms.count()), id, direction,
                 peer.empty() ? "-" : peer.c_str());
    for (const std::uint8_t byte : bytes) std::fprintf(file, "%02x", byte);
    std::fputc('\n', file);
    std::fflush(file);
}

std::string describe(const AllegrexContext &ctx, unsigned count) {
    std::ostringstream text;
    for (unsigned i = 0; i < count; ++i) text << (i != 0u ? ", " : "") << psprecomp::hex32(arg(ctx, i));
    return text.str();
}

// Ends an import, saying what it returned when tracing.
void done(AllegrexContext &ctx, const char *name, unsigned argc, std::uint32_t result, const std::string &note = {}) {
    if (tracing())
        trace(std::string(name) + "(" + describe(ctx, argc) + ") = " + psprecomp::hex32(result) +
              (note.empty() ? "" : " " + note) + " ra=" + psprecomp::hex32(ctx.gpr[31]));
    kernel().finish(ctx, result);
}

std::string rewrite_host(const std::string &host) {
    const auto hook = game().infrastructure_host;
    return hook != nullptr ? hook(host) : host;
}

std::string rewrite_url(const std::string &url) {
    const auto hook = game().infrastructure_url;
    return hook != nullptr ? hook(url) : url;
}

// Host errno, or Winsock error, as the errno the PSP reports.
std::uint32_t psp_errno(int error) {
#if defined(_WIN32)
#define PSP_NET_E(name) WSA##name
#else
#define PSP_NET_E(name) name
#endif
    switch (error) {
    case PSP_NET_E(EWOULDBLOCK): return kEWouldBlock;
    case PSP_NET_E(EINPROGRESS): return kEInProgress;
    case PSP_NET_E(EALREADY): return 120u;
    case PSP_NET_E(ENOTSOCK): return 108u;
    case PSP_NET_E(EDESTADDRREQ): return 121u;
    case PSP_NET_E(EMSGSIZE): return 122u;
    case PSP_NET_E(EPROTOTYPE): return 107u;
    case PSP_NET_E(ENOPROTOOPT): return 109u;
    case PSP_NET_E(EPROTONOSUPPORT): return 123u;
    case PSP_NET_E(EOPNOTSUPP): return 95u;
    case PSP_NET_E(EAFNOSUPPORT): return 106u;
    case PSP_NET_E(EADDRINUSE): return 112u;
    case PSP_NET_E(EADDRNOTAVAIL): return 125u;
    case PSP_NET_E(ENETDOWN): return 115u;
    case PSP_NET_E(ENETUNREACH): return 114u;
    case PSP_NET_E(ENETRESET): return 126u;
    case PSP_NET_E(ECONNABORTED): return 113u;
    case PSP_NET_E(ECONNRESET): return 104u;
    case PSP_NET_E(ENOBUFS): return 105u;
    case PSP_NET_E(EISCONN): return 127u;
    case PSP_NET_E(ENOTCONN): return kENotConn;
    case PSP_NET_E(ESHUTDOWN): return 110u;
    case PSP_NET_E(ETIMEDOUT): return kETimedOut;
    case PSP_NET_E(ECONNREFUSED): return 111u;
    case PSP_NET_E(EHOSTDOWN): return 117u;
    case PSP_NET_E(EHOSTUNREACH): return 118u;
#if defined(_WIN32)
    case WSAEBADF: return kEBadF;
    case WSAEINVAL: return kEInval;
    case WSAEINTR: return 4u;
    case WSAEACCES: return 13u;
    case WSAEFAULT: return 14u;
#endif
    default: return static_cast<std::uint32_t>(error);  // EINTR, EBADF, EINVAL, EPIPE... agree
    }
#undef PSP_NET_E
}

// --- State ---------------------------------------------------------------

struct Socket {
    net::Socket host{net::kNoSocket};
    int type{};
    bool nonblocking{};
    std::uint32_t receive_timeout_us{};
    std::uint32_t send_timeout_us{};
};

struct Handler {
    std::uint32_t function{};
    std::uint32_t argument{};
};

struct HttpExchange;

struct HttpObject {
    enum class Kind { Template, Connection, Request } kind{};
    std::uint32_t parent{};
    std::string agent;  // templates
    std::string url;    // connections and requests
    std::uint32_t method{};
    std::uint64_t content_length{};
    std::vector<std::pair<std::string, std::string>> headers;
    std::shared_ptr<HttpExchange> exchange;
    std::size_t read{};
    std::uint32_t header_block{};  // guest copy of the headers, for GetAllHeader
};

struct State {
    std::uint32_t errno_value{};
    std::map<std::uint32_t, Socket> sockets;
    std::uint32_t apctl_state{kApctlDisconnected};
    std::map<std::uint32_t, Handler> handlers;
    std::uint32_t next_handler{1u};
    std::uint32_t next_resolver{1u};
    std::map<std::uint32_t, HttpObject> http;
    std::uint32_t next_http{1u};
    // name=value pairs by host, from Set-Cookie.
    std::map<std::string, std::map<std::string, std::string>> cookies;
};

State &state() {
    static State value;
    return value;
}

std::uint32_t new_socket_id() {
    // The lowest free id, so it fits the select() sets (256 bits).
    for (std::uint32_t id = 1u; id < 256u; ++id)
        if (!state().sockets.contains(id)) return id;
    return 0u;
}

// --- Socket addresses ----------------------------------------------------
//
// The PSP's sockaddr_in is BSD's: length byte, family byte (AF_INET = 2),
// port and address in network byte order, eight bytes of padding.

std::optional<net::Address> guest_address(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    if (address == 0u) return std::nullopt;
    std::uint8_t raw[8];
    memory.copy_out(address, raw);
    if (raw[1] != 2u) return std::nullopt;
    std::uint16_t port_network{};
    std::uint32_t ip_network{};
    std::memcpy(&port_network, raw + 2, 2);
    std::memcpy(&ip_network, raw + 4, 4);
    return net::Address::ipv4_address(ip_network, ntohs(port_network));
}

void store_guest_address(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t length_address,
                         const sockaddr_storage &host) {
    if (address == 0u) return;
    std::uint8_t raw[16] = {16u, 2u};
    if (host.ss_family == AF_INET) {
        const auto *in = reinterpret_cast<const sockaddr_in *>(&host);
        std::memcpy(raw + 2, &in->sin_port, 2);
        std::memcpy(raw + 4, &in->sin_addr.s_addr, 4);
    }
    memory.copy_in(address, raw);
    if (length_address != 0u) memory.store32(length_address, 16u);
}

// --- Blocking --------------------------------------------------------------
//
// Host sockets are always non-blocking. A call that would block on a
// blocking PSP socket parks the guest thread in a host wait that retries it.

// One try at a socket call: a result, or nothing when it would block.
using Attempt = std::function<std::optional<std::uint32_t>()>;

void run_socket_call(AllegrexContext &ctx, const char *name, unsigned argc, bool nonblocking, std::uint32_t timeout_us,
                     Attempt attempt) {
    if (const auto result = attempt()) return done(ctx, name, argc, *result);
    if (nonblocking) {
        state().errno_value = kEWouldBlock;
        return done(ctx, name, argc, 0xFFFFFFFFu, "would block");
    }
    trace(std::string(name) + "(" + describe(ctx, argc) + ") waits");
    std::string label = name;
    kernel().wait_host(ctx, timeout_us != 0u ? std::optional<std::uint64_t>(timeout_us) : std::nullopt,
                       [label, attempt = std::move(attempt)](bool timed_out) -> std::optional<std::uint32_t> {
                           auto result = attempt();
                           if (!result && timed_out) {
                               state().errno_value = kEWouldBlock;
                               result = 0xFFFFFFFFu;
                           }
                           if (result) trace(label + " ends = " + psprecomp::hex32(*result));
                           return result;
                       });
}

// -1 with errno from the last host error, or nothing when it would block.
std::optional<std::uint32_t> failed_or_blocked() {
    const int error = net::socket_error();
    if (net::would_block(error)) return std::nullopt;
    state().errno_value = psp_errno(error);
    return 0xFFFFFFFFu;
}

Socket *find_socket(std::uint32_t id) {
    const auto found = state().sockets.find(id);
    if (found == state().sockets.end()) {
        state().errno_value = kEBadF;
        return nullptr;
    }
    return &found->second;
}

// BSD send/recv flags to the host's; MSG_DONTWAIT (0x80) is handled here.
int host_flags(std::uint32_t flags) {
    int out = 0;
    if (flags & 0x1u) out |= MSG_OOB;
    if (flags & 0x2u) out |= MSG_PEEK;
    return out;
}

// --- Socket options --------------------------------------------------------

constexpr std::uint32_t kSolSocket = 0xFFFFu;
constexpr std::uint32_t kSoNonblock = 0x1009u;  // the PSP's own
constexpr std::uint32_t kSoSndTimeo = 0x1005u;
constexpr std::uint32_t kSoRcvTimeo = 0x1006u;

// A BSD (level, option) as the host's; nothing for ones without an equivalent.
std::optional<std::pair<int, int>> host_option(std::uint32_t level, std::uint32_t option) {
    if (level == kSolSocket) {
        switch (option) {
        case 0x0004u: return std::pair{SOL_SOCKET, SO_REUSEADDR};
        case 0x0008u: return std::pair{SOL_SOCKET, SO_KEEPALIVE};
        case 0x0020u: return std::pair{SOL_SOCKET, SO_BROADCAST};
        case 0x0080u: return std::pair{SOL_SOCKET, SO_LINGER};
        case 0x1001u: return std::pair{SOL_SOCKET, SO_SNDBUF};
        case 0x1002u: return std::pair{SOL_SOCKET, SO_RCVBUF};
        case 0x1007u: return std::pair{SOL_SOCKET, SO_ERROR};
        case 0x1008u: return std::pair{SOL_SOCKET, SO_TYPE};
        default: return std::nullopt;
        }
    }
    if (level == 6u && option == 1u) return std::pair{static_cast<int>(IPPROTO_TCP), TCP_NODELAY};
    if (level == 0u && option == 3u) return std::pair{static_cast<int>(IPPROTO_IP), IP_TOS};
    if (level == 0u && option == 4u) return std::pair{static_cast<int>(IPPROTO_IP), IP_TTL};
    return std::nullopt;
}

// A timeout option's value: four bytes of microseconds, or a timeval.
std::uint32_t timeout_value(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t length) {
    if (address == 0u) return 0u;
    if (length >= 8u) return memory.load32(address) * 1000000u + memory.load32(address + 4u);
    return memory.load32(address);
}

// --- Access point ----------------------------------------------------------

void apctl_move(std::uint32_t to, std::uint32_t event) {
    State &s = state();
    const std::uint32_t from = s.apctl_state;
    s.apctl_state = to;
    trace("apctl state " + std::to_string(from) + " -> " + std::to_string(to) + " event " + std::to_string(event));
    for (const auto &[id, handler] : s.handlers) {
        InterruptCall call{};
        call.function = handler.function;
        call.arguments = {from, to, event, 0u, handler.argument};
        call.argument_count = 5u;
        kernel().queue_interrupt(std::move(call));
    }
}

void apctl_connect() {
    if (state().apctl_state == kApctlGotIp) return;
    apctl_move(kApctlJoining, kApctlEventConnectRequest);
    apctl_move(kApctlGettingIp, kApctlEventEstablished);
    apctl_move(kApctlGotIp, kApctlEventGetIp);
}

// The address the PSP reports as its own: this machine's on its network.
std::string own_ip() {
    for (const adhoc::LocalAddress &address : adhoc::local_addresses())
        if (address.address.find(':') == std::string::npos) return address.address;
    return "127.0.0.1";
}

void store_apctl_info(psprecomp::GuestMemory &memory, std::uint32_t code, std::uint32_t info) {
    const std::string ip = own_ip();
    const std::string gateway = ip.substr(0, ip.rfind('.')) + ".1";
    switch (code) {
    case 0u: write_cstring(memory, info, "PortableKit", 64u); break;
    case 1u: for (std::uint32_t i = 0; i < 6u; ++i) memory.store8(info + i, i == 0u ? 0x02u : 0x00u); break;
    case 2u: write_cstring(memory, info, "PortableKit", 32u); break;
    case 3u: memory.store32(info, 11u); break;
    case 4u: memory.store32(info, 0u); break;          // no security
    case 5u: memory.store8(info, 100u); break;         // signal strength, %
    case 6u: memory.store8(info, 6u); break;           // channel
    case 7u: memory.store8(info, 0u); break;           // power saving off
    case 8u: write_cstring(memory, info, ip, 16u); break;
    case 9u: write_cstring(memory, info, "255.255.255.0", 16u); break;
    case 10u: write_cstring(memory, info, gateway, 16u); break;
    case 11u: write_cstring(memory, info, gateway, 16u); break;
    case 12u: write_cstring(memory, info, "0.0.0.0", 16u); break;
    case 13u: memory.store32(info, 0u); break;         // no proxy
    case 14u: write_cstring(memory, info, "", 128u); break;
    case 15u: memory.store16(info, 0u); break;
    default: memory.store32(info, 0u); break;          // EAP type, browser, WISP
    }
}

// --- HTTP ------------------------------------------------------------------
//
// A request is made from a host thread with plain sockets, HTTP/1.1 with
// "Connection: close", and its whole response is kept for ReadData. HTTPS
// is not spoken: a profile that points the game at a server rewrites https
// URLs to http.

struct HttpExchange {
    std::atomic<bool> finished{false};
    bool failed{};
    std::string failure;
    int status{};
    std::string header_text;
    std::string body;
    std::vector<std::pair<std::string, std::string>> set_cookies;  // host, name=value
};

struct ParsedUrl {
    std::string host;
    std::uint16_t port{80u};
    std::string path{"/"};
};

std::optional<ParsedUrl> parse_url(const std::string &url) {
    constexpr std::string_view kHttp = "http://";
    if (url.size() < kHttp.size() || !std::equal(kHttp.begin(), kHttp.end(), url.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        }))
        return std::nullopt;
    ParsedUrl parsed;
    const std::string rest = url.substr(kHttp.size());
    const std::size_t slash = rest.find('/');
    std::string authority = rest.substr(0, slash);
    if (slash != std::string::npos) parsed.path = rest.substr(slash);
    if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
        parsed.port = static_cast<std::uint16_t>(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
        authority.resize(colon);
    }
    parsed.host = authority;
    return parsed.host.empty() ? std::nullopt : std::optional(parsed);
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// A blocking TCP connection from the worker thread.
net::Socket connect_to(const ParsedUrl &url, std::string &failure) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &found) != 0 || found == nullptr) {
        failure = "cannot resolve " + url.host;
        return net::kNoSocket;
    }
    net::Socket s = net::kNoSocket;
    for (addrinfo *a = found; a != nullptr && s == net::kNoSocket; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == net::kNoSocket) continue;
        if (::connect(s, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen)) != 0) {
            net::close_socket(s);
            s = net::kNoSocket;
        }
    }
    freeaddrinfo(found);
    if (s == net::kNoSocket) failure = "cannot connect to " + url.host + ":" + std::to_string(url.port);
    else net::no_sigpipe(s);
    return s;
}

std::string dechunk(const std::string &body) {
    std::string out;
    std::size_t at = 0;
    while (at < body.size()) {
        const std::size_t line_end = body.find("\r\n", at);
        if (line_end == std::string::npos) break;
        const std::size_t size = std::strtoul(body.c_str() + at, nullptr, 16);
        if (size == 0u) break;
        at = line_end + 2u;
        out.append(body, at, std::min(size, body.size() - at));
        at += size + 2u;
    }
    return out;
}

void perform(const std::shared_ptr<HttpExchange> &exchange, std::string url, const std::string method,
             const std::vector<std::pair<std::string, std::string>> headers, const std::string body,
             const std::map<std::string, std::map<std::string, std::string>> cookies) {
    for (int redirects = 0;; ++redirects) {
        const auto parsed = parse_url(url);
        if (!parsed) {
            exchange->failed = true;
            exchange->failure = "not an http URL: " + url;
            return;
        }
        const net::Socket s = connect_to(*parsed, exchange->failure);
        if (s == net::kNoSocket) {
            exchange->failed = true;
            return;
        }
        std::string request = method + " " + parsed->path + " HTTP/1.1\r\nHost: " + parsed->host;
        if (parsed->port != 80u) request += ":" + std::to_string(parsed->port);
        request += "\r\nConnection: close\r\n";
        for (const auto &[name, value] : headers) request += name + ": " + value + "\r\n";
        if (const auto jar = cookies.find(parsed->host); jar != cookies.end() && !jar->second.empty()) {
            request += "Cookie: ";
            bool first = true;
            for (const auto &[name, value] : jar->second) {
                request += (first ? "" : "; ") + name + "=" + value;
                first = false;
            }
            request += "\r\n";
        }
        if (method == "POST") request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        request += "\r\n" + body;
        for (std::size_t sent = 0; sent < request.size();) {
            const auto n = ::send(s, request.data() + sent, static_cast<int>(request.size() - sent), net::kSendFlags);
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        std::string response;
        char buffer[16384];
        for (;;) {
            const auto n = ::recv(s, buffer, sizeof(buffer), 0);
            if (n <= 0) break;
            response.append(buffer, static_cast<std::size_t>(n));
        }
        net::close_socket(s);

        const std::size_t header_end = response.find("\r\n\r\n");
        if (response.compare(0, 5, "HTTP/") != 0 || header_end == std::string::npos) {
            exchange->failed = true;
            exchange->failure = "no HTTP response from " + parsed->host;
            return;
        }
        exchange->header_text = response.substr(0, header_end + 4u);
        exchange->status = std::atoi(response.c_str() + response.find(' ') + 1u);
        exchange->body = response.substr(header_end + 4u);
        std::string location;
        bool chunked = false;
        std::istringstream lines(exchange->header_text);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string name = lower(line.substr(0, colon));
            std::string value = line.substr(colon + 1u);
            value.erase(0, value.find_first_not_of(' '));
            if (name == "location") location = value;
            if (name == "transfer-encoding" && lower(value).find("chunked") != std::string::npos) chunked = true;
            if (name == "set-cookie") exchange->set_cookies.emplace_back(parsed->host, value.substr(0, value.find(';')));
        }
        if (chunked) exchange->body = dechunk(exchange->body);
        const bool redirect = exchange->status == 301 || exchange->status == 302 || exchange->status == 303 ||
                              exchange->status == 307;
        if (!redirect || location.empty() || redirects >= 5) return;
        url = location.front() == '/' ? "http://" + parsed->host +
                                            (parsed->port != 80u ? ":" + std::to_string(parsed->port) : "") + location
                                      : rewrite_url(location);
    }
}

HttpObject *find_http(std::uint32_t id, HttpObject::Kind kind) {
    const auto found = state().http.find(id);
    return found != state().http.end() && found->second.kind == kind ? &found->second : nullptr;
}

std::uint32_t new_http(HttpObject object) {
    const std::uint32_t id = state().next_http++;
    state().http.emplace(id, std::move(object));
    return id;
}

// Extra headers of a request, its connection and its template, outermost first.
std::vector<std::pair<std::string, std::string>> request_headers(const HttpObject &request, std::string &agent) {
    std::vector<std::pair<std::string, std::string>> all;
    std::vector<const HttpObject *> chain{&request};
    while (chain.back()->parent != 0u) {
        const auto found = state().http.find(chain.back()->parent);
        if (found == state().http.end()) break;
        chain.push_back(&found->second);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if ((*it)->kind == HttpObject::Kind::Template) agent = (*it)->agent;
        for (const auto &header : (*it)->headers) {
            std::erase_if(all, [&](const auto &h) { return lower(h.first) == lower(header.first); });
            all.push_back(header);
        }
    }
    return all;
}

} // namespace

bool net_infra_enabled() {
    static const bool enabled = [] {
        const char *text = portablekit::env("NET_OFFLINE");
        return text == nullptr || *text == '\0' || *text == '0';
    }();
    return enabled;
}

void net_infra_connect() { apctl_connect(); }

namespace {

// A URI split into its parts, as sceUriParse lays it out: an opaque flag, then
// pointers to the scheme, user name, password, host name, path, query and
// fragment strings, then the port.
constexpr std::uint32_t kUriParts = 7u;
constexpr std::uint32_t kUriPortOffset = 0x20u;

struct Uri {
    bool opaque = false;
    std::string parts[kUriParts];  // scheme, user name, password, host, path, query, fragment
    std::uint16_t port = 0u;
};

Uri parse_uri(std::string_view text) {
    Uri uri;
    auto take = [&text](std::size_t end) {
        std::string part(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end);
        return part;
    };
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos &&
                                                   text.find_first_of("/?#") > colon) {
        uri.parts[0] = take(colon);
        text.remove_prefix(1);
    }
    if (const std::size_t hash = text.find('#'); hash != std::string_view::npos) {
        uri.parts[6] = std::string(text.substr(hash + 1));
        text = text.substr(0, hash);
    }
    if (const std::size_t question = text.find('?'); question != std::string_view::npos) {
        uri.parts[5] = std::string(text.substr(question + 1));
        text = text.substr(0, question);
    }
    if (text.starts_with("//")) {
        text.remove_prefix(2);
        std::string authority = take(text.find('/'));
        if (const std::size_t at = authority.rfind('@'); at != std::string::npos) {
            const std::string user = authority.substr(0, at);
            authority.erase(0, at + 1);
            const std::size_t colon = user.find(':');
            uri.parts[1] = user.substr(0, colon);
            if (colon != std::string::npos) uri.parts[2] = user.substr(colon + 1);
        }
        if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
            uri.port = static_cast<std::uint16_t>(std::strtoul(authority.c_str() + colon + 1, nullptr, 10));
            authority.erase(colon);
        }
        uri.parts[3] = authority;
    } else if (!uri.parts[0].empty()) {
        uri.opaque = true;
    }
    uri.parts[4] = std::string(text);
    if (uri.port == 0u) uri.port = uri.parts[0] == "https" ? 443u : uri.parts[0] == "http" ? 80u : 0u;
    return uri;
}

// The URI the parts make, with only those the option bits name: scheme 0x01,
// host 0x02, port 0x04, path 0x08, user name 0x10, password 0x20, query 0x40,
// fragment 0x80.
std::string build_uri(const Uri &uri, std::uint32_t option) {
    const auto with = [option](std::uint32_t bit) { return (option & bit) != 0u; };
    std::string text;
    if (with(0x01u) && !uri.parts[0].empty()) text += uri.parts[0] + (uri.opaque ? ":" : "://");
    if (with(0x10u) && !uri.parts[1].empty()) {
        text += uri.parts[1];
        if (with(0x20u) && !uri.parts[2].empty()) text += ":" + uri.parts[2];
        text += "@";
    }
    if (with(0x02u)) text += uri.parts[3];
    if (with(0x04u) && uri.port != 0u) text += ":" + std::to_string(uri.port);
    if (with(0x08u)) text += uri.parts[4];
    if (with(0x40u) && !uri.parts[5].empty()) text += "?" + uri.parts[5];
    if (with(0x80u) && !uri.parts[6].empty()) text += "#" + uri.parts[6];
    return text;
}
} // namespace

void register_net_infra(HleRegistrar &hle) {
    if (!net_infra_enabled()) return;
    static net::WinsockSession winsock;
    std::cerr << "[net] infrastructure networking through the host (" << portablekit::env_name("NET_OFFLINE")
              << "=1 turns it off)\n";

    // --- sceNetInet ------------------------------------------------------
    for (const char *name : {"sceNetInetInit", "sceNetInetTerm"})
        hle.add("sceNetInet", name, [name](Runtime &, AllegrexContext &ctx) { done(ctx, name, 0, 0u); });

    // (domain, type, protocol)
    hle.add("sceNetInet", "sceNetInetSocket", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t id = new_socket_id();
        const int type = static_cast<int>(arg(ctx, 1));
        const int host_type = type == 1 ? SOCK_STREAM : type == 2 ? SOCK_DGRAM : type == 3 ? SOCK_RAW : -1;
        const net::Socket s = id != 0u && arg(ctx, 0) == 2u && host_type != -1
                                  ? ::socket(AF_INET, host_type, static_cast<int>(arg(ctx, 2)))
                                  : net::kNoSocket;
        if (s == net::kNoSocket) {
            state().errno_value = id == 0u ? 24u /* EMFILE */ : psp_errno(net::socket_error());
            return done(ctx, "sceNetInetSocket", 3, 0xFFFFFFFFu);
        }
        net::set_nonblocking(s);
        net::no_sigpipe(s);
        state().sockets[id] = Socket{s, type, false, 0u, 0u};
        done(ctx, "sceNetInetSocket", 3, id);
    });

    hle.add("sceNetInet", "sceNetInetClose", [](Runtime &, AllegrexContext &ctx) {
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr) return done(ctx, "sceNetInetClose", 1, 0xFFFFFFFFu);
        net::close_socket(s->host);
        state().sockets.erase(arg(ctx, 0));
        done(ctx, "sceNetInetClose", 1, 0u);
    });
    hle.add("sceNetInet", "sceNetInetCloseWithRST", [](Runtime &, AllegrexContext &ctx) {
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr) return done(ctx, "sceNetInetCloseWithRST", 1, 0xFFFFFFFFu);
        linger reset{};
        reset.l_onoff = 1;
        setsockopt(s->host, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char *>(&reset), sizeof(reset));
        net::close_socket(s->host);
        state().sockets.erase(arg(ctx, 0));
        done(ctx, "sceNetInetCloseWithRST", 1, 0u);
    });

    // (socket, sockaddr *, length)
    hle.add("sceNetInet", "sceNetInetBind", [](Runtime &rt, AllegrexContext &ctx) {
        Socket *s = find_socket(arg(ctx, 0));
        const auto address = guest_address(rt.memory(), arg(ctx, 1));
        if (s == nullptr || !address) {
            if (s != nullptr) state().errno_value = kEInval;
            return done(ctx, "sceNetInetBind", 3, 0xFFFFFFFFu);
        }
        if (::bind(s->host, reinterpret_cast<const sockaddr *>(&address->storage), address->length) != 0) {
            state().errno_value = psp_errno(net::socket_error());
            return done(ctx, "sceNetInetBind", 3, 0xFFFFFFFFu, address->describe());
        }
        done(ctx, "sceNetInetBind", 3, 0u, address->describe());
    });

    hle.add("sceNetInet", "sceNetInetListen", [](Runtime &, AllegrexContext &ctx) {
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr || ::listen(s->host, static_cast<int>(arg(ctx, 1))) != 0) {
            if (s != nullptr) state().errno_value = psp_errno(net::socket_error());
            return done(ctx, "sceNetInetListen", 2, 0xFFFFFFFFu);
        }
        done(ctx, "sceNetInetListen", 2, 0u);
    });

    // (socket, sockaddr *, length *)
    hle.add("sceNetInet", "sceNetInetAccept", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        Socket *s = find_socket(id);
        if (s == nullptr) return done(ctx, "sceNetInetAccept", 3, 0xFFFFFFFFu);
        const std::uint32_t address = arg(ctx, 1), length = arg(ctx, 2);
        run_socket_call(ctx, "sceNetInetAccept", 3, s->nonblocking, s->receive_timeout_us,
                        [&rt, id, address, length]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            sockaddr_storage peer{};
                            socklen_t size = sizeof(peer);
                            const net::Socket accepted =
                                ::accept(s->host, reinterpret_cast<sockaddr *>(&peer), &size);
                            if (accepted == net::kNoSocket) return failed_or_blocked();
                            const std::uint32_t new_id = new_socket_id();
                            net::set_nonblocking(accepted);
                            net::no_sigpipe(accepted);
                            state().sockets[new_id] = Socket{accepted, s->type, false, 0u, 0u};
                            store_guest_address(rt.memory(), address, length, peer);
                            return new_id;
                        });
    });

    // (socket, sockaddr *, length)
    hle.add("sceNetInet", "sceNetInetConnect", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        Socket *s = find_socket(id);
        const auto address = guest_address(rt.memory(), arg(ctx, 1));
        if (s == nullptr || !address) {
            if (s != nullptr) state().errno_value = kEInval;
            return done(ctx, "sceNetInetConnect", 3, 0xFFFFFFFFu);
        }
        trace("connect " + std::to_string(id) + " to " + address->describe());
        capture(id, '@', address->describe(), {});
        if (::connect(s->host, reinterpret_cast<const sockaddr *>(&address->storage), address->length) == 0)
            return done(ctx, "sceNetInetConnect", 3, 0u);
        const int error = net::socket_error();
        if (!net::connect_pending(error) || s->type != 1) {
            state().errno_value = psp_errno(error);
            return done(ctx, "sceNetInetConnect", 3, 0xFFFFFFFFu);
        }
        if (s->nonblocking) {
            state().errno_value = kEInProgress;
            return done(ctx, "sceNetInetConnect", 3, 0xFFFFFFFFu, "in progress");
        }
        // Blocking: wait until it is writable, then report how it went.
        run_socket_call(ctx, "sceNetInetConnect", 3, false, s->send_timeout_us,
                        [id]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            net::PollEntry entry{};
                            entry.fd = s->host;
                            entry.events = POLLOUT;
                            if (net::poll_sockets(&entry, 1u, 0) <= 0) return std::nullopt;
                            int error = 0;
                            socklen_t size = sizeof(error);
                            getsockopt(s->host, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &size);
                            if (error == 0) return 0u;
                            state().errno_value = psp_errno(error);
                            return 0xFFFFFFFFu;
                        });
    });

    // (socket, data *, length, flags)
    hle.add("sceNetInet", "sceNetInetSend", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0), data = arg(ctx, 1), length = arg(ctx, 2), flags = arg(ctx, 3);
        Socket *s = find_socket(id);
        if (s == nullptr) return done(ctx, "sceNetInetSend", 4, 0xFFFFFFFFu);
        run_socket_call(ctx, "sceNetInetSend", 4, s->nonblocking || (flags & 0x80u) != 0u, s->send_timeout_us,
                        [&rt, id, data, length, flags]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            std::vector<std::uint8_t> bytes(length);
                            rt.memory().copy_out(data, bytes);
                            const auto n = ::send(s->host, reinterpret_cast<const char *>(bytes.data()),
                                                  static_cast<int>(length), host_flags(flags) | net::kSendFlags);
                            if (n < 0) return failed_or_blocked();
                            capture(id, '>', {}, std::span(bytes.data(), static_cast<std::size_t>(n)));
                            return static_cast<std::uint32_t>(n);
                        });
    });

    // (socket, data *, length, flags, sockaddr *, address length)
    hle.add("sceNetInet", "sceNetInetSendto", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0), data = arg(ctx, 1), length = arg(ctx, 2), flags = arg(ctx, 3);
        Socket *s = find_socket(id);
        const auto to = guest_address(rt.memory(), arg(ctx, 4));
        if (s == nullptr) return done(ctx, "sceNetInetSendto", 6, 0xFFFFFFFFu);
        if (to) trace("sendto " + to->describe());
        run_socket_call(ctx, "sceNetInetSendto", 6, s->nonblocking || (flags & 0x80u) != 0u, s->send_timeout_us,
                        [&rt, id, data, length, flags, to]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            std::vector<std::uint8_t> bytes(length);
                            rt.memory().copy_out(data, bytes);
                            const auto n = ::sendto(
                                s->host, reinterpret_cast<const char *>(bytes.data()), static_cast<int>(length),
                                host_flags(flags) | net::kSendFlags,
                                to ? reinterpret_cast<const sockaddr *>(&to->storage) : nullptr, to ? to->length : 0);
                            if (n < 0) return failed_or_blocked();
                            capture(id, '>', to ? to->describe() : std::string{},
                                    std::span(bytes.data(), static_cast<std::size_t>(n)));
                            return static_cast<std::uint32_t>(n);
                        });
    });

    // (socket, buffer *, length, flags)
    hle.add("sceNetInet", "sceNetInetRecv", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0), buffer = arg(ctx, 1), length = arg(ctx, 2), flags = arg(ctx, 3);
        Socket *s = find_socket(id);
        if (s == nullptr) return done(ctx, "sceNetInetRecv", 4, 0xFFFFFFFFu);
        run_socket_call(ctx, "sceNetInetRecv", 4, s->nonblocking || (flags & 0x80u) != 0u, s->receive_timeout_us,
                        [&rt, id, buffer, length, flags]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            std::vector<std::uint8_t> bytes(length);
                            const auto n = ::recv(s->host, reinterpret_cast<char *>(bytes.data()),
                                                  static_cast<int>(length), host_flags(flags));
                            if (n < 0) return failed_or_blocked();
                            capture(id, '<', {}, std::span(bytes.data(), static_cast<std::size_t>(n)));
                            rt.memory().copy_in(buffer, std::span(bytes.data(), static_cast<std::size_t>(n)));
                            return static_cast<std::uint32_t>(n);
                        });
    });

    // (socket, buffer *, length, flags, sockaddr *, length *)
    hle.add("sceNetInet", "sceNetInetRecvfrom", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0), buffer = arg(ctx, 1), length = arg(ctx, 2), flags = arg(ctx, 3);
        const std::uint32_t from = arg(ctx, 4), from_length = arg(ctx, 5);
        Socket *s = find_socket(id);
        if (s == nullptr) return done(ctx, "sceNetInetRecvfrom", 6, 0xFFFFFFFFu);
        run_socket_call(ctx, "sceNetInetRecvfrom", 6, s->nonblocking || (flags & 0x80u) != 0u, s->receive_timeout_us,
                        [&rt, id, buffer, length, flags, from, from_length]() -> std::optional<std::uint32_t> {
                            Socket *s = find_socket(id);
                            if (s == nullptr) return 0xFFFFFFFFu;
                            std::vector<std::uint8_t> bytes(length);
                            sockaddr_storage peer{};
                            socklen_t size = sizeof(peer);
                            const auto n = ::recvfrom(s->host, reinterpret_cast<char *>(bytes.data()),
                                                      static_cast<int>(length), host_flags(flags),
                                                      reinterpret_cast<sockaddr *>(&peer), &size);
                            if (n < 0) return failed_or_blocked();
                            net::Address sender{};
                            sender.storage = peer;
                            sender.length = size;
                            capture(id, '<', sender.describe(), std::span(bytes.data(), static_cast<std::size_t>(n)));
                            rt.memory().copy_in(buffer, std::span(bytes.data(), static_cast<std::size_t>(n)));
                            store_guest_address(rt.memory(), from, from_length, peer);
                            return static_cast<std::uint32_t>(n);
                        });
    });

    hle.add("sceNetInet", "sceNetInetShutdown", [](Runtime &, AllegrexContext &ctx) {
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr || ::shutdown(s->host, static_cast<int>(arg(ctx, 1))) != 0) {
            if (s != nullptr) state().errno_value = psp_errno(net::socket_error());
            return done(ctx, "sceNetInetShutdown", 2, 0xFFFFFFFFu);
        }
        done(ctx, "sceNetInetShutdown", 2, 0u);
    });

    // (socket, level, option, value *, length)
    hle.add("sceNetInet", "sceNetInetSetsockopt", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr) return done(ctx, "sceNetInetSetsockopt", 5, 0xFFFFFFFFu);
        const std::uint32_t level = arg(ctx, 1), option = arg(ctx, 2), value = arg(ctx, 3), length = ctx.gpr[8];
        if (level == kSolSocket && option == kSoNonblock) {
            s->nonblocking = value != 0u && memory.load32(value) != 0u;
            return done(ctx, "sceNetInetSetsockopt", 5, 0u, s->nonblocking ? "non-blocking" : "blocking");
        }
        if (level == kSolSocket && (option == kSoRcvTimeo || option == kSoSndTimeo)) {
            (option == kSoRcvTimeo ? s->receive_timeout_us : s->send_timeout_us) = timeout_value(memory, value, length);
            return done(ctx, "sceNetInetSetsockopt", 5, 0u, "timeout " + std::to_string(timeout_value(memory, value, length)) + "us");
        }
        const auto host = host_option(level, option);
        if (!host) return done(ctx, "sceNetInetSetsockopt", 5, 0u, "ignored");
        std::vector<std::uint8_t> bytes(std::min<std::uint32_t>(length, 64u));
        if (value != 0u) memory.copy_out(value, bytes);
        if (setsockopt(s->host, host->first, host->second, reinterpret_cast<const char *>(bytes.data()),
                       static_cast<socklen_t>(bytes.size())) != 0) {
            state().errno_value = psp_errno(net::socket_error());
            return done(ctx, "sceNetInetSetsockopt", 5, 0xFFFFFFFFu);
        }
        done(ctx, "sceNetInetSetsockopt", 5, 0u);
    });

    // (socket, level, option, value *, length *)
    hle.add("sceNetInet", "sceNetInetGetsockopt", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        Socket *s = find_socket(arg(ctx, 0));
        if (s == nullptr) return done(ctx, "sceNetInetGetsockopt", 5, 0xFFFFFFFFu);
        const std::uint32_t level = arg(ctx, 1), option = arg(ctx, 2), value = arg(ctx, 3), length = ctx.gpr[8];
        std::uint32_t answer = 0u;
        if (level == kSolSocket && option == kSoNonblock) answer = s->nonblocking ? 1u : 0u;
        else if (level == kSolSocket && option == kSoRcvTimeo) answer = s->receive_timeout_us;
        else if (level == kSolSocket && option == kSoSndTimeo) answer = s->send_timeout_us;
        else if (const auto host = host_option(level, option)) {
            int got = 0;
            socklen_t size = sizeof(got);
            getsockopt(s->host, host->first, host->second, reinterpret_cast<char *>(&got), &size);
            answer = host->second == SO_ERROR && host->first == SOL_SOCKET ? psp_errno(got)
                                                                             : static_cast<std::uint32_t>(got);
        }
        if (value != 0u) memory.store32(value, answer);
        if (length != 0u) memory.store32(length, 4u);
        done(ctx, "sceNetInetGetsockopt", 5, 0u);
    });

    for (const bool peer : {false, true}) {
        const char *name = peer ? "sceNetInetGetpeername" : "sceNetInetGetsockname";
        hle.add("sceNetInet", name, [name, peer](Runtime &rt, AllegrexContext &ctx) {
            Socket *s = find_socket(arg(ctx, 0));
            if (s == nullptr) return done(ctx, name, 3, 0xFFFFFFFFu);
            sockaddr_storage address{};
            socklen_t size = sizeof(address);
            const int result = peer ? getpeername(s->host, reinterpret_cast<sockaddr *>(&address), &size)
                                    : getsockname(s->host, reinterpret_cast<sockaddr *>(&address), &size);
            if (result != 0) {
                state().errno_value = psp_errno(net::socket_error());
                return done(ctx, name, 3, 0xFFFFFFFFu);
            }
            store_guest_address(rt.memory(), arg(ctx, 1), arg(ctx, 2), address);
            done(ctx, name, 3, 0u);
        });
    }

    // (n, read set *, write set *, except set *, timeval *): BSD fd_sets of
    // 256 bits in 32-bit words.
    hle.add("sceNetInet", "sceNetInetSelect", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t count = std::min<std::uint32_t>(arg(ctx, 0), 256u);
        const std::array<std::uint32_t, 3> sets{arg(ctx, 1), arg(ctx, 2), arg(ctx, 3)};
        const std::uint32_t timeval = ctx.gpr[8];
        // The ids each set asks about, read now: the call rewrites them.
        auto wanted = std::make_shared<std::array<std::vector<std::uint32_t>, 3>>();
        for (std::size_t k = 0; k < 3u; ++k) {
            if (sets[k] == 0u) continue;
            for (std::uint32_t id = 0; id < count; ++id)
                if (memory.load32(sets[k] + id / 32u * 4u) & (1u << (id % 32u))) (*wanted)[k].push_back(id);
        }
        const auto attempt = [&rt, sets, count, wanted](bool give_up) -> std::optional<std::uint32_t> {
            std::vector<net::PollEntry> entries;
            std::vector<std::pair<std::size_t, std::uint32_t>> owners;
            const short events[3] = {POLLIN, POLLOUT, POLLPRI};
            for (std::size_t k = 0; k < 3u; ++k)
                for (const std::uint32_t id : (*wanted)[k])
                    if (const auto found = state().sockets.find(id); found != state().sockets.end()) {
                        net::PollEntry entry{};
                        entry.fd = found->second.host;
                        entry.events = events[k];
                        entries.push_back(entry);
                        owners.emplace_back(k, id);
                    }
            if (!entries.empty()) net::poll_sockets(entries.data(), entries.size(), 0);
            std::array<std::array<std::uint32_t, 8>, 3> ready{};
            std::uint32_t total = 0;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const short got = entries[i].revents;
                const auto [k, id] = owners[i];
                const bool hit = k == 0u   ? (got & (POLLIN | POLLHUP | POLLERR)) != 0
                                 : k == 1u ? (got & (POLLOUT | POLLERR)) != 0
                                           : (got & POLLPRI) != 0;
                if (hit) {
                    ready[k][id / 32u] |= 1u << (id % 32u);
                    ++total;
                }
            }
            if (total == 0u && !give_up) return std::nullopt;
            for (std::size_t k = 0; k < 3u; ++k)
                if (sets[k] != 0u)
                    for (std::uint32_t word = 0; word < (count + 31u) / 32u; ++word)
                        rt.memory().store32(sets[k] + word * 4u, ready[k][word]);
            return total;
        };
        std::optional<std::uint64_t> timeout_us;
        if (timeval != 0u)
            timeout_us = static_cast<std::uint64_t>(memory.load32(timeval)) * 1000000u + memory.load32(timeval + 4u);
        if (const auto now = attempt(timeout_us && *timeout_us == 0u)) return done(ctx, "sceNetInetSelect", 5, *now);
        kernel().wait_host(ctx, timeout_us, [attempt](bool timed_out) { return attempt(timed_out); });
    });

    hle.add("sceNetInet", "sceNetInetGetErrno", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, state().errno_value);
    });
    hle.add("sceNetInet", "sceNetInetGetPspError", [](Runtime &, AllegrexContext &ctx) {
        kernel().finish(ctx, state().errno_value == 0u ? 0u : 0x80010000u | state().errno_value);
    });
    // (dotted address, in_addr out): 1 if it is one.
    hle.add("sceNetInet", "sceNetInetInetAton", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string text = read_cstring(rt.memory(), arg(ctx, 0), 64u);
        unsigned a = 0, b = 0, c = 0, d = 0;
        char extra = 0;
        if (std::sscanf(text.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255u || b > 255u ||
            c > 255u || d > 255u)
            return done(ctx, "sceNetInetInetAton", 2, 0u, text);
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), a | b << 8 | c << 16 | d << 24);
        done(ctx, "sceNetInetInetAton", 2, 1u, text);
    });

    // --- sceNetApctl -----------------------------------------------------
    for (const char *name : {"sceNetApctlInit", "sceNetApctlTerm"})
        hle.add("sceNetApctl", name, [name](Runtime &, AllegrexContext &ctx) { done(ctx, name, 2, 0u); });
    // (handler, argument)
    hle.add("sceNetApctl", "sceNetApctlAddHandler", [](Runtime &, AllegrexContext &ctx) {
        const std::uint32_t id = state().next_handler++;
        state().handlers[id] = Handler{arg(ctx, 0), arg(ctx, 1)};
        done(ctx, "sceNetApctlAddHandler", 2, id);
    });
    hle.add("sceNetApctl", "sceNetApctlDelHandler", [](Runtime &, AllegrexContext &ctx) {
        state().handlers.erase(arg(ctx, 0));
        done(ctx, "sceNetApctlDelHandler", 1, 0u);
    });
    hle.add("sceNetApctl", "sceNetApctlConnect", [](Runtime &, AllegrexContext &ctx) {
        apctl_connect();
        done(ctx, "sceNetApctlConnect", 1, 0u);
    });
    hle.add("sceNetApctl", "sceNetApctlDisconnect", [](Runtime &, AllegrexContext &ctx) {
        if (state().apctl_state != kApctlDisconnected) apctl_move(kApctlDisconnected, kApctlEventDisconnectRequest);
        done(ctx, "sceNetApctlDisconnect", 0, 0u);
    });
    hle.add("sceNetApctl", "sceNetApctlGetState", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), state().apctl_state);
        done(ctx, "sceNetApctlGetState", 1, 0u, "state " + std::to_string(state().apctl_state));
    });
    // (code, info *)
    hle.add("sceNetApctl", "sceNetApctlGetInfo", [](Runtime &rt, AllegrexContext &ctx) {
        if (state().apctl_state != kApctlGotIp) return done(ctx, "sceNetApctlGetInfo", 2, 0x80410A04u, "not connected");
        if (arg(ctx, 1) != 0u) store_apctl_info(rt.memory(), arg(ctx, 0), arg(ctx, 1));
        done(ctx, "sceNetApctlGetInfo", 2, 0u, "code " + std::to_string(arg(ctx, 0)));
    });

    // --- sceNetResolver --------------------------------------------------
    for (const char *name : {"sceNetResolverInit", "sceNetResolverTerm", "sceNetResolverDelete"})
        hle.add("sceNetResolver", name, [name](Runtime &, AllegrexContext &ctx) { done(ctx, name, 1, 0u); });
    // (int *id, buffer *, buffer length)
    hle.add("sceNetResolver", "sceNetResolverCreate", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), state().next_resolver++);
        done(ctx, "sceNetResolverCreate", 3, 0u);
    });
    // (id, name, in_addr *, timeout, retries)
    hle.add("sceNetResolver", "sceNetResolverStartNtoA", [](Runtime &rt, AllegrexContext &ctx) {
        const std::string asked = read_cstring(rt.memory(), arg(ctx, 1), 256u);
        const std::string name = rewrite_host(asked);
        const std::uint32_t out = arg(ctx, 2);
        const std::string note = asked + (name != asked ? " (as " + name + ")" : "");
        auto lookup = std::make_shared<std::future<std::uint32_t>>(std::async(std::launch::async, [name] {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo *found = nullptr;
            std::uint32_t ip = 0u;
            if (getaddrinfo(name.c_str(), nullptr, &hints, &found) == 0 && found != nullptr)
                ip = reinterpret_cast<const sockaddr_in *>(found->ai_addr)->sin_addr.s_addr;
            if (found != nullptr) freeaddrinfo(found);
            return ip;
        }));
        trace("resolving " + note);
        kernel().wait_host(ctx, std::nullopt,
                           [&rt, lookup, out, note](bool) -> std::optional<std::uint32_t> {
                               if (lookup->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                                   return std::nullopt;
                               const std::uint32_t ip = lookup->get();
                               if (ip == 0u) {
                                   std::cerr << "[net] cannot resolve " << note << "\n";
                                   return kResolverFailed;
                               }
                               if (out != 0u) rt.memory().store32(out, ip);
                               trace("resolved " + note + " to " + net::Address::ipv4_address(ip, 0).host());
                               return 0u;
                           });
    });

    // --- sceHttp ---------------------------------------------------------
    for (const char *name : {"sceHttpInit", "sceHttpEnd", "sceHttpsInit", "sceHttpsEnd", "sceHttpsLoadDefaultCert",
                             "sceHttpSetMallocFunction", "sceHttpSaveSystemCookie", "sceHttpLoadSystemCookie",
                             "sceHttpSetRecvTimeOut", "sceHttpSetSendTimeOut", "sceHttpSetConnectTimeOut",
                             "sceHttpDisableCookie", "sceHttpDisableCache", "sceHttpDisableAuth",
                             "sceHttpSetRedirectCallback", "sceHttpAbortRequest", "sceHttpSetResolveTimeOut",
                             "sceHttpsEnableOption", "sceHttpsDisableOption"})
        hle.add("sceHttp", name, [name](Runtime &, AllegrexContext &ctx) { done(ctx, name, 2, 0u); });

    // --- sceParseUri -----------------------------------------------------
    // (element out, URL, pool, size needed out, pool size): the strings go to
    // the pool. Without an element or pool, only the size needed is given.
    hle.add("sceParseUri", "sceUriParse", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 0), pool = arg(ctx, 2), needed_out = arg(ctx, 3), pool_size = arg(ctx, 4);
        const std::string url = read_cstring(memory, arg(ctx, 1), 1024u);
        const Uri uri = parse_uri(url);
        std::uint32_t needed = 0u;
        for (const std::string &part : uri.parts) needed += static_cast<std::uint32_t>(part.size()) + 1u;
        if (needed_out != 0u) memory.store32(needed_out, needed);
        if (out == 0u || pool == 0u) return done(ctx, "sceUriParse", 5, 0u, url);
        if (pool_size < needed) return done(ctx, "sceUriParse", 5, kHttpFailed, url);
        memory.store32(out, uri.opaque ? 1u : 0u);
        std::uint32_t next = pool;
        for (std::uint32_t i = 0; i < kUriParts; ++i) {
            write_cstring(memory, next, uri.parts[i], uri.parts[i].size() + 1u);
            memory.store32(out + 4u + i * 4u, next);
            next += static_cast<std::uint32_t>(uri.parts[i].size()) + 1u;
        }
        memory.store16(out + kUriPortOffset, uri.port);
        done(ctx, "sceUriParse", 5, 0u, url);
    });
    // (URI out, size needed out, out size, element, option bits)
    hle.add("sceParseUri", "sceUriBuild", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 0), needed_out = arg(ctx, 1), out_size = arg(ctx, 2),
                            element = arg(ctx, 3);
        if (element == 0u) return done(ctx, "sceUriBuild", 5, kHttpFailed);
        Uri uri;
        uri.opaque = memory.load32(element) != 0u;
        for (std::uint32_t i = 0; i < kUriParts; ++i)
            if (const std::uint32_t text = memory.load32(element + 4u + i * 4u); text != 0u)
                uri.parts[i] = read_cstring(memory, text, 1024u);
        uri.port = memory.load16(element + kUriPortOffset);
        const std::string text = build_uri(uri, arg(ctx, 4));
        const auto needed = static_cast<std::uint32_t>(text.size()) + 1u;
        if (needed_out != 0u) memory.store32(needed_out, needed);
        if (out != 0u) {
            if (out_size < needed) return done(ctx, "sceUriBuild", 5, kHttpFailed, text);
            write_cstring(memory, out, text, needed);
        }
        done(ctx, "sceUriBuild", 5, 0u, text);
    });
    // (out, size needed out, out size, text): percent-encodes all but
    // letters, digits and "-_.~". Without an out, only the size is given.
    hle.add("sceParseUri", "sceUriEscape", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t out = arg(ctx, 0), needed_out = arg(ctx, 1), out_size = arg(ctx, 2);
        std::string text;
        for (const char c : read_cstring(memory, arg(ctx, 3), 4096u)) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || std::strchr("-_.~", c) != nullptr) {
                text += c;
            } else {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%%%02X", static_cast<unsigned char>(c));
                text += hex;
            }
        }
        const auto needed = static_cast<std::uint32_t>(text.size()) + 1u;
        if (needed_out != 0u) memory.store32(needed_out, needed);
        if (out != 0u) {
            if (out_size < needed) return done(ctx, "sceUriEscape", 4, kHttpFailed);
            write_cstring(memory, out, text, needed);
        }
        done(ctx, "sceUriEscape", 4, 0u);
    });

    // (user agent, http version, use system proxy)
    hle.add("sceHttp", "sceHttpCreateTemplate", [](Runtime &rt, AllegrexContext &ctx) {
        HttpObject object;
        object.kind = HttpObject::Kind::Template;
        object.agent = arg(ctx, 0) != 0u ? read_cstring(rt.memory(), arg(ctx, 0), 256u) : std::string{};
        done(ctx, "sceHttpCreateTemplate", 3, new_http(std::move(object)));
    });
    // (template, url, enable keep-alive)
    hle.add("sceHttp", "sceHttpCreateConnectionWithURL", [](Runtime &rt, AllegrexContext &ctx) {
        if (find_http(arg(ctx, 0), HttpObject::Kind::Template) == nullptr)
            return done(ctx, "sceHttpCreateConnectionWithURL", 3, kHttpUnknownId);
        HttpObject object;
        object.kind = HttpObject::Kind::Connection;
        object.parent = arg(ctx, 0);
        object.url = read_cstring(rt.memory(), arg(ctx, 1), 1024u);
        const std::string url = object.url;
        done(ctx, "sceHttpCreateConnectionWithURL", 3, new_http(std::move(object)), url);
    });
    // (connection, method, url, u64 content length)
    hle.add("sceHttp", "sceHttpCreateRequestWithURL", [](Runtime &rt, AllegrexContext &ctx) {
        if (find_http(arg(ctx, 0), HttpObject::Kind::Connection) == nullptr)
            return done(ctx, "sceHttpCreateRequestWithURL", 4, kHttpUnknownId);
        HttpObject object;
        object.kind = HttpObject::Kind::Request;
        object.parent = arg(ctx, 0);
        object.method = arg(ctx, 1);
        object.url = read_cstring(rt.memory(), arg(ctx, 2), 1024u);
        object.content_length = arg64(ctx, 4);
        const std::string url = object.url;
        done(ctx, "sceHttpCreateRequestWithURL", 4, new_http(std::move(object)), url);
    });
    for (const char *name : {"sceHttpDeleteTemplate", "sceHttpDeleteConnection", "sceHttpDeleteRequest"})
        hle.add("sceHttp", name, [name](Runtime &, AllegrexContext &ctx) {
            const auto found = state().http.find(arg(ctx, 0));
            if (found == state().http.end()) return done(ctx, name, 1, kHttpUnknownId);
            if (found->second.header_block != 0u) kernel().free_block(static_cast<SceUID>(found->second.header_block));
            state().http.erase(found);
            done(ctx, name, 1, 0u);
        });
    // (id, name, value, mode)
    hle.add("sceHttp", "sceHttpAddExtraHeader", [](Runtime &rt, AllegrexContext &ctx) {
        const auto found = state().http.find(arg(ctx, 0));
        if (found == state().http.end()) return done(ctx, "sceHttpAddExtraHeader", 4, kHttpUnknownId);
        const std::string name = read_cstring(rt.memory(), arg(ctx, 1), 256u);
        const std::string value = read_cstring(rt.memory(), arg(ctx, 2), 1024u);
        std::erase_if(found->second.headers, [&](const auto &h) { return lower(h.first) == lower(name); });
        found->second.headers.emplace_back(name, value);
        done(ctx, "sceHttpAddExtraHeader", 4, 0u, name + ": " + value);
    });

    // (request, data *, size)
    hle.add("sceHttp", "sceHttpSendRequest", [](Runtime &rt, AllegrexContext &ctx) {
        const std::uint32_t id = arg(ctx, 0);
        HttpObject *request = find_http(id, HttpObject::Kind::Request);
        if (request == nullptr) return done(ctx, "sceHttpSendRequest", 3, kHttpUnknownId);
        std::string body(arg(ctx, 2), '\0');
        if (arg(ctx, 1) != 0u && !body.empty())
            rt.memory().copy_out(arg(ctx, 1), std::span(reinterpret_cast<std::uint8_t *>(body.data()), body.size()));
        std::string agent;
        auto headers = request_headers(*request, agent);
        if (!agent.empty()) headers.insert(headers.begin(), {"User-Agent", agent});
        const std::string url = rewrite_url(request->url);
        const std::string method = request->method == 1u ? "POST" : request->method == 2u ? "HEAD" : "GET";
        std::cerr << "[net] " << method << " " << request->url << (url != request->url ? " (as " + url + ")" : "")
                  << "\n";
        std::string sent;
        for (const auto &[name, value] : headers) sent += name + ": " + value + "\r\n";
        sent += "\r\n" + body;
        capture(id, '>', method + ":" + url, std::span(reinterpret_cast<const std::uint8_t *>(sent.data()), sent.size()));
        request->exchange = std::make_shared<HttpExchange>();
        request->read = 0u;
        std::thread([exchange = request->exchange, url, method, headers, body, cookies = state().cookies] {
            perform(exchange, url, method, headers, body, cookies);
            exchange->finished = true;
        }).detach();
        kernel().wait_host(ctx, std::nullopt, [id](bool) -> std::optional<std::uint32_t> {
            HttpObject *request = find_http(id, HttpObject::Kind::Request);
            if (request == nullptr) return kHttpUnknownId;
            const HttpExchange &exchange = *request->exchange;
            if (!exchange.finished) return std::nullopt;
            for (const auto &[host, cookie] : exchange.set_cookies) {
                const std::size_t eq = cookie.find('=');
                if (eq != std::string::npos) state().cookies[host][cookie.substr(0, eq)] = cookie.substr(eq + 1u);
            }
            if (exchange.failed) {
                std::cerr << "[net] request failed: " << exchange.failure << "\n";
                return kHttpFailed;
            }
            trace("response " + std::to_string(exchange.status) + ", " + std::to_string(exchange.body.size()) +
                  " bytes");
            const std::string received = exchange.header_text + "\r\n" + exchange.body;
            capture(id, '<', "http:" + std::to_string(exchange.status),
                    std::span(reinterpret_cast<const std::uint8_t *>(received.data()), received.size()));
            return 0u;
        });
    });

    const auto exchange_of = [](std::uint32_t id) -> HttpExchange * {
        HttpObject *request = find_http(id, HttpObject::Kind::Request);
        return request != nullptr && request->exchange && request->exchange->finished && !request->exchange->failed
                   ? request->exchange.get()
                   : nullptr;
    };
    // (request, int *status)
    hle.add("sceHttp", "sceHttpGetStatusCode", [exchange_of](Runtime &rt, AllegrexContext &ctx) {
        const HttpExchange *exchange = exchange_of(arg(ctx, 0));
        if (exchange == nullptr) return done(ctx, "sceHttpGetStatusCode", 2, kHttpFailed);
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), static_cast<std::uint32_t>(exchange->status));
        done(ctx, "sceHttpGetStatusCode", 2, 0u, std::to_string(exchange->status));
    });
    // (request, u64 *length)
    hle.add("sceHttp", "sceHttpGetContentLength", [exchange_of](Runtime &rt, AllegrexContext &ctx) {
        const HttpExchange *exchange = exchange_of(arg(ctx, 0));
        if (exchange == nullptr) return done(ctx, "sceHttpGetContentLength", 2, kHttpFailed);
        if (arg(ctx, 1) != 0u) store64(rt.memory(), arg(ctx, 1), exchange->body.size());
        done(ctx, "sceHttpGetContentLength", 2, 0u, std::to_string(exchange->body.size()));
    });
    // (request, buffer *, size): bytes copied, 0 at the end.
    hle.add("sceHttp", "sceHttpReadData", [](Runtime &rt, AllegrexContext &ctx) {
        HttpObject *request = find_http(arg(ctx, 0), HttpObject::Kind::Request);
        if (request == nullptr || !request->exchange || !request->exchange->finished || request->exchange->failed)
            return done(ctx, "sceHttpReadData", 3, kHttpFailed);
        const std::string &body = request->exchange->body;
        const std::size_t n = std::min<std::size_t>(arg(ctx, 2), body.size() - request->read);
        rt.memory().copy_in(arg(ctx, 1),
                            std::span(reinterpret_cast<const std::uint8_t *>(body.data()) + request->read, n));
        request->read += n;
        done(ctx, "sceHttpReadData", 3, static_cast<std::uint32_t>(n));
    });
    // (request, u8 **header, u32 *size): the response headers, in guest memory.
    hle.add("sceHttp", "sceHttpGetAllHeader", [](Runtime &rt, AllegrexContext &ctx) {
        HttpObject *request = find_http(arg(ctx, 0), HttpObject::Kind::Request);
        if (request == nullptr || !request->exchange || !request->exchange->finished || request->exchange->failed)
            return done(ctx, "sceHttpGetAllHeader", 3, kHttpFailed);
        const std::string &text = request->exchange->header_text;
        if (request->header_block == 0u) {
            const std::int32_t uid =
                kernel().allocate_block("sceHttpHeader", 0u, static_cast<std::uint32_t>(text.size() + 1u), 0u);
            if (uid < 0) return done(ctx, "sceHttpGetAllHeader", 3, kHttpFailed);
            request->header_block = static_cast<std::uint32_t>(uid);
        }
        const MemoryBlock *block = kernel().find_block(static_cast<SceUID>(request->header_block));
        write_cstring(rt.memory(), block->address, text, text.size() + 1u);
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), block->address);
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), static_cast<std::uint32_t>(text.size()));
        done(ctx, "sceHttpGetAllHeader", 3, 0u);
    });
    // (request, int *errno)
    hle.add("sceHttp", "sceHttpGetNetworkPspError", [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        done(ctx, "sceHttpGetNetworkPspError", 2, 0u);
    });
}

} // namespace portablekit
