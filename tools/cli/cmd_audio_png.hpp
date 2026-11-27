#pragma once

// PNG encoding for the `pulp audio` image verbs, defined in cmd_audio.cpp
// beside the scope renderer. The encoder itself is audio_png_encoder.hpp.

#include <cstdint>
#include <string>
#include <vector>

// Encodes an RGBA8 buffer (row-major, top row first) as PNG. Returns empty
// bytes on failure and records the reason in `error_out` when given.
std::vector<std::uint8_t> encode_png_rgba(std::uint32_t width, std::uint32_t height,
                                          const std::vector<std::uint8_t>& rgba,
                                          std::string* error_out);
