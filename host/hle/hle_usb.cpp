// sceUsb and sceUsbGps: the PSP's GPS receiver (PSP-290), plugged into the
// USB port, placed where the GPS location setting says (network.gps,
// "latitude, longitude" in degrees). With no location set, no receiver is
// plugged in: the drivers start, the bus reports no cable and the receiver
// never gets a fix, as on a PSP without one.
//
// The data layout is the one Metal Gear Solid: Portable Ops Plus reads for
// its GPS SCAN: a fix counts once the short at +0x14 is not zero, and the
// latitude must be under 80 degrees either way.
#include "hle_common.hpp"

#include "settings/settings.hpp"

#include "psprecomp/common.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

namespace portablekit {
namespace {

// sceUsbGetState bits.
constexpr std::uint32_t kUsbActivated = 0x200u;
constexpr std::uint32_t kUsbDeactivated = 0x100u;
constexpr std::uint32_t kUsbCableConnected = 0x020u;
constexpr std::uint32_t kUsbCableDisconnected = 0x010u;
constexpr std::uint32_t kUsbConnectionEstablished = 0x002u;
constexpr std::uint32_t kUsbConnectionNotEstablished = 0x001u;
// sceUsbGpsGetState: 0 off, 1 and 2 starting, 3 on.
constexpr std::uint32_t kGpsOn = 3u;
constexpr std::uint32_t kSatellites = 8u;

struct Usb {
    bool gps_driver{};
    bool activated{};
    bool gps_open{};
};

Usb &usb() {
    static Usb value;
    return value;
}

struct Location {
    float latitude{};
    float longitude{};
};

// The setting, when it holds a place on Earth.
std::optional<Location> location() {
    Location place;
    if (std::sscanf(settings::current().gps_location.c_str(), "%f , %f", &place.latitude, &place.longitude) != 2 ||
        std::fabs(place.latitude) > 90.0f || std::fabs(place.longitude) > 180.0f)
        return std::nullopt;
    return place;
}

bool receiver_plugged() { return usb().gps_driver && location().has_value(); }

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    memory.store32(address, std::bit_cast<std::uint32_t>(value));
}

// GpsData: s16 year, month, day, hour, minute, second (UTC); float at +0xC;
// float hdop; u16 at +0x14 (a fix when not zero); float latitude, longitude,
// altitude; float at +0x24; float speed, bearing.
void store_fix(psprecomp::GuestMemory &memory, std::uint32_t address, const Location &place) {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    for (std::uint32_t i = 0; i < 0x30u; i += 4u) memory.store32(address + i, 0u);
    memory.store16(address, static_cast<std::uint16_t>(utc.tm_year + 1900));
    memory.store16(address + 2u, static_cast<std::uint16_t>(utc.tm_mon + 1));
    memory.store16(address + 4u, static_cast<std::uint16_t>(utc.tm_mday));
    memory.store16(address + 6u, static_cast<std::uint16_t>(utc.tm_hour));
    memory.store16(address + 8u, static_cast<std::uint16_t>(utc.tm_min));
    memory.store16(address + 10u, static_cast<std::uint16_t>(utc.tm_sec));
    store_float(memory, address + 0x10u, 1.0f);
    memory.store16(address + 0x14u, static_cast<std::uint16_t>(kSatellites));
    store_float(memory, address + 0x18u, place.latitude);
    store_float(memory, address + 0x1Cu, place.longitude);
}

// SatData: s16 satellites in view, s16, then 24 of {u8 id, u8 elevation,
// s16 azimuth, u8 signal to noise, u8 used, s16}.
void store_satellites(psprecomp::GuestMemory &memory, std::uint32_t address) {
    memory.store16(address, static_cast<std::uint16_t>(kSatellites));
    memory.store16(address + 2u, 0u);
    for (std::uint32_t i = 0; i < 24u; ++i) {
        const std::uint32_t entry = address + 4u + i * 8u;
        const bool seen = i < kSatellites;
        memory.store8(entry, seen ? static_cast<std::uint8_t>(i + 1u) : 0u);
        memory.store8(entry + 1u, seen ? 45u : 0u);
        memory.store16(entry + 2u, seen ? static_cast<std::uint16_t>(i * 45u) : 0u);
        memory.store8(entry + 4u, seen ? 40u : 0u);
        memory.store8(entry + 5u, seen ? 1u : 0u);
        memory.store16(entry + 6u, 0u);
    }
}

} // namespace

void register_usb(HleRegistrar &hle) {
    // (driver name, size, arguments): the bus, accessory and GPS drivers.
    hle.add("sceUsb", "sceUsbStart", [](Runtime &rt, AllegrexContext &ctx) {
        if (read_cstring(rt.memory(), arg(ctx, 0), 64u) == "USBGps_Driver") usb().gps_driver = true;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUsb", "sceUsbStop", [](Runtime &rt, AllegrexContext &ctx) {
        if (read_cstring(rt.memory(), arg(ctx, 0), 64u) == "USBGps_Driver") usb().gps_driver = false;
        kernel().finish(ctx, 0u);
    });
    // (product id): 0x283 for the GPS receiver.
    hle.add("sceUsb", "sceUsbActivate", [](Runtime &, AllegrexContext &ctx) {
        usb().activated = true;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUsb", "sceUsbDeactivate", [](Runtime &, AllegrexContext &ctx) {
        usb().activated = false;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUsb", "sceUsbGetState", [](Runtime &, AllegrexContext &ctx) {
        const bool plugged = receiver_plugged();
        const bool established = plugged && usb().activated;
        kernel().finish(ctx, (usb().activated ? kUsbActivated : kUsbDeactivated) |
                                 (plugged ? kUsbCableConnected : kUsbCableDisconnected) |
                                 (established ? kUsbConnectionEstablished : kUsbConnectionNotEstablished));
    });
    hle.add("sceUsbGps", "sceUsbGpsOpen", [](Runtime &, AllegrexContext &ctx) {
        usb().gps_open = true;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUsbGps", "sceUsbGpsClose", [](Runtime &, AllegrexContext &ctx) {
        usb().gps_open = false;
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUsbGps", "sceUsbGpsGetState", [](Runtime &rt, AllegrexContext &ctx) {
        const bool on = usb().gps_open && usb().activated && receiver_plugged();
        if (arg(ctx, 0) != 0u) rt.memory().store32(arg(ctx, 0), on ? kGpsOn : 0u);
        kernel().finish(ctx, 0u);
    });
    // (GpsData *, SatData *)
    hle.add("sceUsbGps", "sceUsbGpsGetData", [](Runtime &rt, AllegrexContext &ctx) {
        const std::optional<Location> place = location();
        if (place && arg(ctx, 0) != 0u) store_fix(rt.memory(), arg(ctx, 0), *place);
        if (place && arg(ctx, 1) != 0u) store_satellites(rt.memory(), arg(ctx, 1));
        kernel().finish(ctx, 0u);
    });
    hle.add("sceUtility", "sceUtilityLoadUsbModule", [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
    hle.add("sceUtility", "sceUtilityUnloadUsbModule",
            [](Runtime &, AllegrexContext &ctx) { kernel().finish(ctx, 0u); });
}

} // namespace portablekit
