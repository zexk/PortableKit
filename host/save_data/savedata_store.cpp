#include "save_data/savedata_store.hpp"

#include "save_data/param_sfo.hpp"
#include "crypto_keys.hpp"
#include "save_data/savedata_crypto.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace portablekit::savedata {
namespace {

constexpr const char *kParamSfo = "PARAM.SFO";
// SAVEDATA_FILE_LIST: 99 entries of a 13-byte name, its 16-byte hash and 3
// bytes of padding.
constexpr std::size_t kFileListEntries = 99u;
constexpr std::size_t kFileListEntrySize = 32u;
constexpr std::size_t kFileListNameSize = 13u;
// The mode a PSP picks for a game built with SDK 4 or later that passes a
// key, which is what this game does.
constexpr CryptMode kSaveMode = CryptMode::Mode5;

// Nothing for a file that is not there or cannot be read: libstdc++ throws
// on a read error, which would end the game in the middle of a load.
std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::nullopt;
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

bool write_file(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes) {
    // Write beside the target, put it on the disk and rename it over the
    // target, so neither an interrupted save nor a machine that goes down
    // soon after leaves a half-written or empty file in place of a good one.
    std::filesystem::path temporary = path;
    temporary += ".tmp";
#if defined(_WIN32)
    std::FILE *out = _wfopen(temporary.c_str(), L"wb");
#else
    std::FILE *out = std::fopen(temporary.c_str(), "wb");
#endif
    if (out == nullptr) return false;
    bool written = std::fwrite(bytes.data(), 1u, bytes.size(), out) == bytes.size() && std::fflush(out) == 0;
#if defined(_WIN32)
    written = written && _commit(_fileno(out)) == 0;
#else
    written = written && fsync(fileno(out)) == 0;
#endif
    written = std::fclose(out) == 0 && written;
    std::error_code ec;
    // On Windows a virus scanner or an indexer can hold a file it has just
    // seen written, and the rename is refused while it does: try again for
    // half a second before calling the save failed.
    for (int attempt = 0; written && attempt < 10; ++attempt) {
        std::filesystem::rename(temporary, path, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // Nothing half-made stays in the folder, which is exported as it is.
    std::filesystem::remove(temporary, ec);
    return false;
}

// A hash stored in the file list for `name`, if any.
std::optional<Block> listed_hash(const ParamSfo &sfo, const std::string &name) {
    const auto *list = sfo.binary("SAVEDATA_FILE_LIST");
    if (list == nullptr) return std::nullopt;
    for (std::size_t offset = 0; offset + kFileListEntrySize <= list->size(); offset += kFileListEntrySize) {
        std::string entry_name;
        for (std::size_t i = 0; i < kFileListNameSize && (*list)[offset + i] != 0u; ++i)
            entry_name.push_back(static_cast<char>((*list)[offset + i]));
        if (entry_name != name) continue;
        Block hash{};
        std::copy_n(list->begin() + static_cast<std::ptrdiff_t>(offset + kFileListNameSize), hash.size(), hash.begin());
        return hash;
    }
    return std::nullopt;
}

Block random_block() {
    std::random_device device;
    std::mt19937_64 engine(device() ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    Block block{};
    for (auto &b : block) b = static_cast<std::uint8_t>(engine());
    return block;
}

} // namespace

std::filesystem::path savedata_root(const std::filesystem::path &memory_stick) {
    return memory_stick / "PSP" / "SAVEDATA";
}

std::filesystem::path save_folder(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    return savedata_root(memory_stick) / (files.game_name + files.save_name);
}

bool save_exists(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    std::error_code ec;
    const auto folder = save_folder(memory_stick, files);
    return std::filesystem::is_directory(folder, ec) && std::filesystem::is_regular_file(folder / kParamSfo, ec);
}

namespace {

// Where write_save keeps the save a folder held before its last write:
// beside the save folders, so a folder stays exactly as a PSP has it.
std::filesystem::path previous_folder(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    return savedata_root(memory_stick) / ".previous" / (files.game_name + files.save_name);
}

// The save in one folder: its PARAM.SFO and data file, checked against each other.
LoadResult load_folder(const std::filesystem::path &folder, const SaveFiles &files) {
    LoadResult result;
    const auto sfo_bytes = read_file(folder / kParamSfo);
    const auto sfo = sfo_bytes ? ParamSfo::parse(*sfo_bytes) : std::nullopt;
    const auto file = read_file(folder / files.file_name);
    std::error_code ec;
    // The folder exists but the game's file does not.
    if (!file && !std::filesystem::exists(folder / files.file_name, ec)) return result;
    result.status = LoadStatus::Broken;
    if (!file) {
        result.reason = files.file_name + " cannot be read";
        return result;
    }
    if (!sfo) {
        result.reason = "PARAM.SFO is unreadable";
        return result;
    }

    const auto *params = sfo->binary("SAVEDATA_PARAMS");
    const std::uint8_t flags = params != nullptr && !params->empty() ? (*params)[kParamsFlagsOffset] : 0u;
    if (flags != 0u) {
        const auto mode = mode_from_flags(flags);
        if (!mode) {
            result.reason = "unknown SAVEDATA_PARAMS flags " + std::to_string(flags);
            return result;
        }
        if (!savedata_keys_available()) {
            result.reason = "the save is encrypted, and reading it needs the console's keys (add a keys file)";
            return result;
        }
        const Block *key = files.key ? &*files.key : nullptr;
        if (*mode != CryptMode::Mode1 && key == nullptr) {
            result.reason = "the save is encrypted with a game key but the game passed none";
            return result;
        }
        if (const auto expected = listed_hash(*sfo, files.file_name);
            expected && *expected != data_file_hash(*file, *mode, key)) {
            result.reason = files.file_name + " does not match its hash in PARAM.SFO";
            return result;
        }
        auto plain = decrypt_data(*file, *mode, key);
        if (!plain) {
            result.reason = files.file_name + " is too short to be encrypted";
            return result;
        }
        result.contents.data = std::move(*plain);
    } else {
        result.contents.data = *file;
    }
    result.contents.title = sfo->string("TITLE").value_or("");
    result.contents.savedata_title = sfo->string("SAVEDATA_TITLE").value_or("");
    result.contents.detail = sfo->string("SAVEDATA_DETAIL").value_or("");
    result.contents.parental_level = sfo->integer("PARENTAL_LEVEL").value_or(0u);
    result.status = LoadStatus::Ok;
    return result;
}

} // namespace

LoadResult load_save(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    if (!save_exists(memory_stick, files)) return {};
    LoadResult result = load_folder(save_folder(memory_stick, files), files);
    if (result.status != LoadStatus::Broken) return result;
    // A save cut short, between its files or by a machine that went down
    // before they reached the disk: the one before it, which write_save kept.
    LoadResult kept = load_folder(previous_folder(memory_stick, files), files);
    if (kept.status != LoadStatus::Ok) return result;
    kept.reason = result.reason + "; loaded the save before it instead";
    return kept;
}

bool write_save(const std::filesystem::path &memory_stick, const SaveFiles &files, const SaveContents &contents,
                std::string &error) {
    const auto folder = save_folder(memory_stick, files);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        error = "cannot create " + folder.string() + ": " + ec.message();
        return false;
    }

    // The save this one replaces, if it loads, is kept for load_save to
    // fall back on; one that does not leaves the last good one in place.
    if (load_folder(folder, files).status == LoadStatus::Ok) {
        const auto kept = previous_folder(memory_stick, files);
        std::filesystem::create_directories(kept, ec);
        for (const std::string &name : {std::string(kParamSfo), files.file_name}) {
            const auto bytes = read_file(folder / name);
            if (!ec && bytes && write_file(kept / name, *bytes)) continue;
            std::cerr << "[savedata] cannot keep the previous save in " << kept.string() << "\n";
            break;
        }
    }

    // Keep what an existing PARAM.SFO lists for other files.
    std::optional<ParamSfo> previous;
    if (const auto bytes = read_file(folder / kParamSfo)) previous = ParamSfo::parse(*bytes);
    std::vector<std::uint8_t> file_list(kFileListEntries * kFileListEntrySize, 0u);
    if (previous) {
        if (const auto *list = previous->binary("SAVEDATA_FILE_LIST"); list != nullptr)
            std::copy_n(list->begin(), std::min(list->size(), file_list.size()), file_list.begin());
    }

    std::vector<std::uint8_t> stored;
    if (files.key) {
        stored = encrypt_data(contents.data, kSaveMode, &*files.key, random_block());
        const Block hash = data_file_hash(stored, kSaveMode, &*files.key);
        // Replace this file's entry, or take the first free one.
        std::size_t slot = file_list.size();
        for (std::size_t offset = 0; offset < file_list.size(); offset += kFileListEntrySize) {
            const std::string name(reinterpret_cast<const char *>(&file_list[offset]),
                                   strnlen(reinterpret_cast<const char *>(&file_list[offset]), kFileListNameSize));
            if (name == files.file_name) {
                slot = offset;
                break;
            }
            if (name.empty() && slot == file_list.size()) slot = offset;
        }
        if (slot == file_list.size()) {
            error = "SAVEDATA_FILE_LIST is full";
            return false;
        }
        std::fill_n(file_list.begin() + static_cast<std::ptrdiff_t>(slot), kFileListEntrySize, 0u);
        std::copy_n(files.file_name.begin(), std::min(files.file_name.size(), kFileListNameSize - 1u),
                    file_list.begin() + static_cast<std::ptrdiff_t>(slot));
        std::copy(hash.begin(), hash.end(), file_list.begin() + static_cast<std::ptrdiff_t>(slot + kFileListNameSize));
    } else {
        stored = contents.data;
    }

    ParamSfo sfo;
    sfo.set_string("CATEGORY", "MS", 4u);
    sfo.set_integer("PARENTAL_LEVEL", contents.parental_level);
    sfo.set_string("SAVEDATA_DETAIL", contents.detail, 1024u);
    sfo.set_string("SAVEDATA_DIRECTORY", files.game_name + files.save_name, 64u);
    sfo.set_binary("SAVEDATA_FILE_LIST", file_list, static_cast<std::uint32_t>(file_list.size()));
    sfo.set_binary("SAVEDATA_PARAMS", std::vector<std::uint8_t>(kParamsSize, 0u), kParamsSize);
    sfo.set_string("SAVEDATA_TITLE", contents.savedata_title, 128u);
    sfo.set_string("TITLE", contents.title, 128u);
    std::vector<std::uint8_t> sfo_bytes = sfo.serialize();
    if (files.key) sign_param_sfo(sfo_bytes, *sfo.data_offset("SAVEDATA_PARAMS"), kSaveMode);

    const std::pair<const char *, const std::vector<std::uint8_t> *> media[] = {
        {"ICON0.PNG", &contents.icon0}, {"ICON1.PMF", &contents.icon1},
        {"PIC1.PNG", &contents.pic1}, {"SND0.AT3", &contents.snd0}};
    for (const auto &[name, bytes] : media) {
        if (bytes->empty()) continue;
        if (!write_file(folder / name, *bytes)) {
            error = std::string("cannot write ") + name;
            return false;
        }
    }
    // The data file first and PARAM.SFO last: PARAM.SFO carries the data
    // file's hash, so a save interrupted in between is reported as broken
    // rather than loaded with mismatched contents.
    if (!write_file(folder / files.file_name, stored)) {
        error = "cannot write " + files.file_name;
        return false;
    }
    if (!write_file(folder / kParamSfo, sfo_bytes)) {
        error = "cannot write PARAM.SFO";
        return false;
    }
    return true;
}

bool delete_save(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    std::error_code ec;
    // The kept one goes too: a later save there must not fall back on it.
    std::filesystem::remove_all(previous_folder(memory_stick, files), ec);
    const auto removed = std::filesystem::remove_all(save_folder(memory_stick, files), ec);
    return !ec && removed > 0u;
}

std::uint64_t save_size(const std::filesystem::path &memory_stick, const SaveFiles &files) {
    std::error_code ec;
    std::uint64_t total = 0u;
    const auto folder = save_folder(memory_stick, files);
    if (!std::filesystem::is_directory(folder, ec)) return 0u;
    for (const auto &entry : std::filesystem::directory_iterator(folder, ec))
        if (entry.is_regular_file(ec)) total += entry.file_size(ec);
    return total;
}

} // namespace portablekit::savedata
