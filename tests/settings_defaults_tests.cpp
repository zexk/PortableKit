// Which defaults each platform starts from; no settings.ini is read. Also
// where the data directory is.
#include "app_paths.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"
#include "perf/frame_stats.hpp"
#include "perf/perf_overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {
using namespace portablekit::settings;

int failures{};

void check(bool condition, const char *message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void test_desktop_is_the_declared_defaults() {
    const Settings desktop = defaults_for(Platform::Desktop);
    const Settings declared{};
    check(desktop.aspect == declared.aspect && desktop.fullscreen == declared.fullscreen &&
              desktop.mouse == declared.mouse && desktop.internal_scale == declared.internal_scale &&
              desktop.right_stick == declared.right_stick && desktop.analog_camera == declared.analog_camera,
          "the desktop defaults are Settings{} as declared");
    check(desktop.aspect == Aspect::Original && !desktop.fullscreen && desktop.mouse,
          "the desktop keeps the original aspect, a window and the mouse");
    check(desktop.fast_loading && !desktop.unthrottled, "loads run fast and the game keeps real time otherwise");
}

void test_android() {
    const Settings android = defaults_for(Platform::Android);
    check(android.aspect == Aspect::Fill, "a phone fills its wide screen");
    check(android.fullscreen, "a phone is full screen");
    check(!android.mouse, "a phone does not capture a mouse");
    check(android.analog_camera && android.right_stick == RightStick::Camera,
          "the analog camera is on, as the touch camera needs");
    check(android.touch_controls, "the touch controls are on");
    check(android.frame_rate == FrameRate::Fps30 && android.frame_rate_auto, "30 fps, lowered when behind");
    check(android.present_mode == PresentMode::Fifo && android.perf == PerfDisplay::Off,
          "vsync on and no performance overlay");
    check(android.fast_loading, "a phone loads fast too");
}

void test_this_build() {
    const Settings expected = defaults_for(kPlatform);
    check(defaults().aspect == expected.aspect && defaults().mouse == expected.mouse &&
              defaults().fullscreen == expected.fullscreen,
          "defaults() is this platform's set");
}

} // namespace

// A folder named "portable" beside the executable holds the data instead of
// the user's data folder. One that was there before the test is left alone.
void test_portable_folder() {
    namespace fs = std::filesystem;
    namespace install = portablekit::install;
    const fs::path portable = portablekit::executable_directory() / install::kPortableFolder;
    std::error_code ec;
    const bool made = fs::create_directory(portable, ec);
    check(install::user_data_directory() == portable, "a portable folder beside the executable holds the data");
    if (!made) return;
    fs::remove(portable, ec);
    check(install::user_data_directory() != portable, "without it the data goes to the user's data folder");
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "fps") {
        using namespace portablekit;
        check(current().perf == PerfDisplay::Fps, "FPS override selects the counter");
        check(perf::options().overlay && !perf::options().log, "FPS counter enables only the overlay");
        check(perf::overlay_width() == 64 && perf::overlay_height() == 11, "FPS counter is one small line");
        std::array<std::uint32_t, perf::kOverlayWidth * perf::kOverlayHeight + 2> pixels;
        pixels.fill(0x12345678);
        perf::draw_overlay(pixels.data() + 1);
        check(pixels.front() == 0x12345678 && pixels.back() == 0x12345678, "Overlay stays inside its buffer");
        const auto lower = pixels.begin() + 1 + perf::overlay_height() * perf::kOverlayWidth;
        check(*lower == 0, "FPS background is transparent");
        check(std::all_of(lower, pixels.end() - 1, [&](auto pixel) { return pixel == *lower; }),
              "FPS-only mode draws no graph or detail rows");
        check(std::any_of(pixels.begin() + 1, lower, [&](auto pixel) { return pixel != *lower; }),
              "FPS text is visible before the first measured second");
        current().perf = PerfDisplay::Overlay;
        perf::draw_overlay(pixels.data() + 1);
        check((pixels[1] >> 24u) == 255u, "Detailed overlay keeps its background");
        check(perf::overlay_width() == perf::kOverlayWidth && perf::overlay_height() == perf::kOverlayHeight,
              "Detailed overlay retains its original size");
        return failures ? 1 : 0;
    }
    // Check an environment override through the real settings parser.
    if (argc == 2) {
        check(current().fov_percent == static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)),
              "FOV override parses and stays within its supported range");
        return failures ? 1 : 0;
    }
    test_desktop_is_the_declared_defaults();
    test_portable_folder();
    test_android();
    test_this_build();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "settings defaults tests passed\n";
    return 0;
}
