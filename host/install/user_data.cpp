#include "../profile.hpp"
#include "install/user_data.hpp"

#include "app_paths.hpp"

#include "psprecomp/common.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <system_error>

#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>
#endif

namespace portablekit::install {
namespace {




std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1u);
}

std::filesystem::path environment_path(const char *name) {
    const char *value = std::getenv(name);
    return value != nullptr && *value != '\0' ? path_from_utf8(value) : std::filesystem::path{};
}

} // namespace

std::string path_to_utf8(const std::filesystem::path &path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

std::filesystem::path path_from_utf8(const std::string &text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path user_data_directory() {
    if (const char *dir = portablekit::env("DATA_DIR"); dir != nullptr && *dir != '\0') return path_from_utf8(dir);
#if !defined(PORTABLEKIT_ANDROID_APP)
    // Portable: a folder named "portable" beside the executable holds the
    // data, saves and settings instead of the user's data folder, so the
    // port can be moved or kept on a removable drive.
    if (const std::filesystem::path beside = executable_directory(); !beside.empty()) {
        std::error_code ec;
        if (std::filesystem::is_directory(beside / kPortableFolder, ec)) return beside / kPortableFolder;
    }
#endif
#if defined(PORTABLEKIT_HAS_SDL)
    char *pref = SDL_GetPrefPath(portablekit::game().data_organization, portablekit::game().data_application);
    if (pref == nullptr) throw psprecomp::Error(std::string("Cannot determine the user data directory: ") + SDL_GetError());
    std::filesystem::path result = path_from_utf8(pref);
    SDL_free(pref);
    return result;
#else
    const auto dir = default_data_directory(portablekit::game().data_organization, portablekit::game().data_application);
    if (dir.empty()) throw psprecomp::Error(std::string("Cannot determine the user data directory; set ") + portablekit::env_name("DATA_DIR"));
    return dir;
#endif
}

std::filesystem::path default_data_directory(const char *organization, const char *application) {
    // The locations SDL_GetPrefPath uses, so builds with and without SDL agree.
    std::filesystem::path base;
#if defined(_WIN32)
    base = environment_path("APPDATA");
#elif defined(__APPLE__)
    if (const auto home = environment_path("HOME"); !home.empty()) base = home / "Library" / "Application Support";
#else
    base = environment_path("XDG_DATA_HOME");
    if (const auto home = environment_path("HOME"); base.empty() && !home.empty()) base = home / ".local" / "share";
#endif
    return base.empty() ? base : base / organization / application;
}

SettingsEntries read_settings_file(const std::filesystem::path &data_dir) {
    SettingsEntries entries;
    std::ifstream in(data_dir / kSettingsFile);
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = trim(line.substr(0, equals));
        if (!key.empty()) entries[key] = trim(line.substr(equals + 1u));
    }
    return entries;
}

void write_settings_file(const std::filesystem::path &data_dir, const SettingsEntries &entries) {
    std::filesystem::create_directories(data_dir);
    const std::filesystem::path target = data_dir / kSettingsFile;
    const std::filesystem::path partial = data_dir / (std::string(kSettingsFile) + ".part");
    {
        std::ofstream out(partial, std::ios::trunc);
        out << "# Written by " << portablekit::game().app_name
            << ": the installer and the in-game menu (Esc, or L3+R3 on a gamepad).\n"
            << "# disc_image: the disc image to play from; a relative path is inside this directory.\n";
        for (const auto &[key, value] : entries) out << key << "=" << value << "\n";
        if (!out) throw psprecomp::Error("Cannot write " + path_to_utf8(partial));
    }
    std::filesystem::rename(partial, target);
}

UserSettings load_settings(const std::filesystem::path &data_dir) {
    UserSettings settings;
    const SettingsEntries entries = read_settings_file(data_dir);
    if (const auto found = entries.find("disc_image"); found != entries.end() && !found->second.empty())
        settings.disc_image = path_from_utf8(found->second);
    return settings;
}

void save_settings(const std::filesystem::path &data_dir, const UserSettings &settings) {
    SettingsEntries entries = read_settings_file(data_dir);
    entries["disc_image"] = path_to_utf8(settings.disc_image);
    write_settings_file(data_dir, entries);
}

std::optional<Installation> find_installation(const std::filesystem::path &data_dir) {
    std::error_code ec;
    const std::filesystem::path executable = data_dir / kExecutableFile;
    if (!std::filesystem::is_regular_file(executable, ec)) return std::nullopt;
    const UserSettings settings = load_settings(data_dir);
    Installation installation;
    installation.executable = executable;
    installation.disc_image = settings.disc_image.is_absolute() ? settings.disc_image : data_dir / settings.disc_image;
    installation.image_copied = !settings.disc_image.is_absolute();
    return installation;
}

namespace {

// Passes what a standard stream writes on to the buffer it had, and copies
// it to the log. It keeps nothing itself, so threads that share a stream
// share nothing more here.
class LogCopy : public std::streambuf {
public:
    LogCopy(std::streambuf *before, std::FILE *log) : before_(before), log_(log) {}

protected:
    int overflow(int c) override {
        if (traits_type::eq_int_type(c, traits_type::eof())) return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        std::fwrite(&ch, 1u, 1u, log_);
        return before_->sputc(ch);
    }
    std::streamsize xsputn(const char *text, std::streamsize count) override {
        std::fwrite(text, 1u, static_cast<std::size_t>(count), log_);
        return before_->sputn(text, count);
    }
    int sync() override { return before_->pubsync(); }

private:
    std::streambuf *before_;
    std::FILE *log_;
};

} // namespace

void start_log_file() {
#if !defined(PORTABLEKIT_ANDROID_APP)
    static bool started = false;
    if (started || portablekit::env("NO_LOG_FILE") != nullptr) return;
    started = true;
    std::FILE *log = nullptr;
    try {
        const std::filesystem::path dir = user_data_directory();
        const std::string app = game().app_name;
        std::error_code ec;
        std::filesystem::rename(dir / (app + ".log"), dir / (app + "-previous.log"), ec);
#if defined(_WIN32)
        log = _wfopen((dir / (app + ".log")).c_str(), L"wb");
#else
        log = std::fopen((dir / (app + ".log")).c_str(), "wb");
#endif
    } catch (const std::exception &) {
    }
    if (log == nullptr) return;
    // Unbuffered: a crash keeps every line written before it.
    std::setvbuf(log, nullptr, _IONBF, 0);
    // Never freed, nor the file closed: the streams outlive every static object.
    for (std::ostream *stream : {static_cast<std::ostream *>(&std::cout), static_cast<std::ostream *>(&std::cerr),
                                 static_cast<std::ostream *>(&std::clog)})
        stream->rdbuf(new LogCopy(stream->rdbuf(), log));
#endif
}

} // namespace portablekit::install
