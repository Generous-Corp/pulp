#pragma once

// The font files Pulp embeds (external/fonts/*.ttf through
// pulp_add_binary_data). Skia-free so both text backends read one table:
// the Skia font manager (bundled_fonts.cpp) and, on Apple, the CoreText
// registry that the CoreGraphics canvas paints and measures with.

#include <cstddef>
#include <span>

namespace pulp::canvas::detail {

struct BundledBlob {
    const char* asset_name; // Original file name, for diagnostics.
    const unsigned char* data;
    std::size_t size;
};

std::span<const BundledBlob> bundled_font_blobs();

} // namespace pulp::canvas::detail
