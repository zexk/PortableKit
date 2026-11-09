#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace portablekit::gpu {

// Top row first, four bytes per pixel. Window captures use opaque=true when
// the source framebuffer's alpha channel is unused; texture dumps retain it.
bool write_png(const std::filesystem::path &path, std::span<const std::uint8_t> pixels,
               std::uint32_t width, std::uint32_t height, bool bgra, bool opaque = false);

} // namespace portablekit::gpu
