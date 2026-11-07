#pragma once

// Texture-to-buffer readback layout shared by Renderer3D and GpuTarget.
//
// WebGPU requires `bytesPerRow` of a texture-to-buffer copy to be a multiple
// of 256, so a readback buffer holds each row padded to that alignment and
// the CPU copy has to drop the padding again. Dawn-free and pure, so the row
// arithmetic is tested without a GPU.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pulp::render::detail {

inline constexpr std::uint32_t kCopyBytesPerRowAlignment = 256;

constexpr std::uint32_t align_up(std::uint32_t value, std::uint32_t alignment) {
    return ((value + alignment - 1u) / alignment) * alignment;
}

struct PaddedRowLayout {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t compact_bytes_per_row = 0;
    std::uint32_t padded_bytes_per_row = 0;
    /// Size of the readback buffer: every row padded.
    std::uint64_t buffer_size = 0;
};

constexpr PaddedRowLayout padded_row_layout(std::uint32_t width, std::uint32_t height,
                                            std::uint32_t bytes_per_pixel) {
    PaddedRowLayout layout;
    layout.width = width;
    layout.height = height;
    layout.compact_bytes_per_row = width * bytes_per_pixel;
    layout.padded_bytes_per_row = align_up(layout.compact_bytes_per_row, kCopyBytesPerRowAlignment);
    layout.buffer_size = static_cast<std::uint64_t>(layout.padded_bytes_per_row) * height;
    return layout;
}

/// Copy `layout.height` padded rows from `padded` into a tightly packed
/// buffer. `padded` must hold `layout.buffer_size` bytes.
inline std::vector<std::uint8_t> compact_padded_rows(const std::uint8_t* padded,
                                                     const PaddedRowLayout& layout) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(layout.compact_bytes_per_row) *
                                  layout.height);
    for (std::uint32_t y = 0; y < layout.height; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * layout.compact_bytes_per_row,
                    padded + static_cast<std::size_t>(y) * layout.padded_bytes_per_row,
                    layout.compact_bytes_per_row);
    }
    return out;
}

} // namespace pulp::render::detail
