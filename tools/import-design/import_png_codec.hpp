// Minimal PNG ↔ RGBA8 codec for the import CLI.
//
// AssetManager::decode_png only stores raw bytes plus IHDR dimensions — the
// real decode happens in the Skia renderer, which is not linked into the
// GPU-off importer build. Passes that need actual pixels (fader/meter skin
// sampling, per-control sprite cropping) decode here with miniz, which the CLI
// already links.
//
// Scope is deliberately the shape design tools emit: 8-bit, non-interlaced,
// colour types 0/2/4/6. Anything else fails closed and the caller skips its
// pixel-derived step rather than guessing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pulp::import_design {

/// Decoded pixels, RGBA8 row-major, tightly packed.
struct ImportPngImage {
    std::vector<std::uint8_t> rgba;
    int width = 0;
    int height = 0;

    bool valid() const {
        return !rgba.empty() && width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4;
    }
};

/// Read a PNG's pixel dimensions from its IHDR header without decoding the
/// pixel data. Returns {0, 0} for anything unreadable or not a PNG. Used to
/// recover a source's true aspect ratio so imported art is never skewed.
std::pair<int, int> read_png_dimensions(const std::string& path);

/// Decode an 8-bit, non-interlaced PNG to RGBA8. An unsupported PNG shape or a
/// malformed stream yields an image for which `valid()` is false.
ImportPngImage decode_png_rgba(const std::uint8_t* data, std::size_t size);

/// Re-encode RGBA8 pixels as an 8-bit RGBA PNG. Lossless against
/// decode_png_rgba, so a decode → edit-pixels → encode round trip leaves the
/// untouched pixels byte-identical. Empty on failure or invalid input.
std::vector<std::uint8_t> encode_png_rgba(const ImportPngImage& image);

}  // namespace pulp::import_design
