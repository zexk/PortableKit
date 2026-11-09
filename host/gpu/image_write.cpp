#include "image_write.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

// Shared with the texture dumper; one copy of the existing PNG encoder.
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace portablekit::gpu {

bool write_png(const std::filesystem::path &path, std::span<const std::uint8_t> pixels,
               std::uint32_t width, std::uint32_t height, bool bgra, bool opaque) {
    const std::uint64_t count = std::uint64_t(width) * height;
    if (width == 0 || height == 0 || count > std::numeric_limits<int>::max() / 4u ||
        count * 4u != pixels.size()) return false;
    std::vector<std::uint8_t> converted;
    if (bgra || opaque) {
        converted.assign(pixels.begin(), pixels.end());
        for (std::size_t i = 0; i < count; ++i) {
            if (bgra) std::swap(converted[i * 4], converted[i * 4 + 2]);
            if (opaque) converted[i * 4 + 3] = 255;
        }
        pixels = converted;
    }
    int length = 0;
    std::unique_ptr<unsigned char, decltype(&std::free)> png(
        stbi_write_png_to_mem(pixels.data(), static_cast<int>(width * 4u), static_cast<int>(width),
                              static_cast<int>(height), 4, &length), &std::free);
    if (!png) return false;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png.get()), length);
    return static_cast<bool>(out);
}

} // namespace portablekit::gpu
