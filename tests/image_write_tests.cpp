#include "gpu/image_write.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("portablekit_image_tests_" + std::to_string(std::random_device{}()));
    fs::create_directories(root);
    const auto path = root / "image.png";
    const std::array<std::uint8_t, 16> rgba{255, 0, 0, 0, 0, 255, 0, 128,
                                           0, 0, 255, 255, 123, 45, 67, 255};
    int failures = 0;
    const auto check = [&](bool ok, const char *what) {
        if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
    };
    for (bool bgra : {false, true}) {
        for (bool opaque : {false, true}) {
            auto input = rgba;
            if (bgra)
                for (unsigned i = 0; i < 16; i += 4) std::swap(input[i], input[i + 2]);
            check(portablekit::gpu::write_png(path, input, 2, 2, bgra, opaque), "PNG written");
            std::ifstream file(path, std::ios::binary);
            std::vector<unsigned char> encoded{std::istreambuf_iterator<char>(file), {}};
            int width = 0, height = 0, channels = 0;
            auto *decoded = stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()),
                                                   &width, &height, &channels, 4);
            check(decoded && width == 2 && height == 2, "PNG decodes at original resolution");
            if (decoded)
                for (unsigned i = 0; i < 16; ++i)
                    check(decoded[i] == (opaque && i % 4 == 3 ? 255 : rgba[i]),
                          "pixel order, colours and alpha survive round trip");
            stbi_image_free(decoded);
        }
    }
    check(!portablekit::gpu::write_png(path, rgba, 0, 2, false), "empty image refused");
    check(!portablekit::gpu::write_png(path, rgba, 3, 2, false), "short buffer refused");
    check(!portablekit::gpu::write_png(root / "missing" / "image.png", rgba, 2, 2, false),
          "write failure reported");
    fs::remove_all(root);
    if (failures) return 1;
    std::cout << "PNG screenshot round trips passed\n";
}
