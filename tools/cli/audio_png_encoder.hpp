#pragma once

// Dependency-free PNG writer for the `pulp audio` image verbs. The pictures
// are plain pixel buffers (scope trace, waveform, spectrogram), so encoding
// needs only zlib framing and a CRC, not a render layer.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp_cli {

/// Encode an RGBA8 buffer (row-major, top row first, `width * height * 4`
/// bytes) as an 8-bit RGBA PNG with no interlacing. Returns empty bytes and
/// records the reason in `error_out` when the size does not match the buffer,
/// a dimension is zero, or compression fails.
std::vector<std::uint8_t> encode_png_rgba8(std::uint32_t width, std::uint32_t height,
                                           const std::vector<std::uint8_t>& rgba,
                                           std::string* error_out);

} // namespace pulp_cli
