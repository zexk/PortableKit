// Which defaults each platform starts from; no settings.ini is read. Also
// where the data directory is.
#include "app_paths.hpp"
#include "install/user_data.hpp"
#include "settings/settings.hpp"

#include <filesystem>
#include <iostream>

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

int main() {
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
