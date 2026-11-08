#pragma once

// PNG encoding for the `pulp audio` image verbs, defined in cmd_audio.cpp
// beside the scope renderer that already owns the render-layer include.
// Keeping the include there means the image sources do not pull the canvas
// and render headers into their translation units.

#include <cstdint>
#include <string>
#include <vector>

// Encodes an RGBA8 buffer (row-major, top row first) as PNG. Returns empty
// bytes on failure and records the reason in `error_out` when given.
std::vector<std::uint8_t> encode_png_rgba(std::uint32_t width, std::uint32_t height,
                                          const std::vector<std::uint8_t>& rgba,
                                          std::string* error_out);
