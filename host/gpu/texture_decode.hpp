#pragma once

#include "ge_state.hpp"

#include <cstdint>
#include <vector>

namespace portablekit::gpu {

// ge.frag's framebuffer sampling flags. Indexed views reconstruct the source
// pixel's packed bits before applying the CLUT shift, mask and palette bank.
// Keep these below 2^24 so a float push constant carries every bit exactly.
[[nodiscard]] inline std::uint32_t framebuffer_texture_flags(const TextureState &texture,
                                                           std::uint32_t source_format) {
    const auto format = static_cast<std::uint32_t>(texture.format);
    if (texture.swizzled || source_format > 3u) return 0u;
    if (format <= 3u)
        return (format == 3u) == (source_format == 3u) ? 1u : 0u;
    if (format != (source_format == 3u ? 7u : 6u)) return 0u;
    return 9u | source_format << 4u | (texture.clut_shift & 31u) << 6u |
           (texture.clut_mask & 255u) << 11u | (texture.clut_offset & 31u) << 19u;
}

// Decodes a PSP texture into RGBA8888 (one std::uint32_t per texel, red in the
// low byte). Handles the direct colour formats, 4/8/16/32-bit palettes, DXT1/3/5
// and the swizzled layout. Returns false when the texture cannot be read.
bool decode_texture(const GuestMemory &memory, const TextureState &texture, std::vector<std::uint32_t> &out);

// What decoding a texture reads from guest memory, copied, so that it can be
// decoded on another thread while the game goes on changing memory.
struct TextureSnapshot {
    TextureState texture;
    std::uint32_t row_bytes{};
    std::vector<std::uint8_t> texels;
    std::vector<std::uint8_t> clut;  // the palette's first 512 entries, for the indexed formats
};
// Copies what decode_texture() would read; false for a texture it cannot copy
// that way (a block format, a run not contiguous in host memory), which is
// then decoded at once.
bool snapshot_texture(const GuestMemory &memory, const TextureState &texture, TextureSnapshot &snapshot);
// Decodes a snapshot to the texels decode_texture() gives. Safe on any thread;
// unswizzles the snapshot's texels in place.
bool decode_snapshot(TextureSnapshot &snapshot, std::vector<std::uint32_t> &out);

// Key that identifies the decoded contents of a texture for caching.
[[nodiscard]] std::uint64_t texture_key(const GuestMemory &memory, const TextureState &texture);

} // namespace portablekit::gpu
