#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace pulp::canvas {

/// Return the largest prefix length <= byte_index that ends on a valid UTF-8
/// scalar boundary. This is intentionally conservative: malformed/truncated
/// input stops at the last known-good boundary so platform text APIs never see
/// an invalid substring while computing caret offsets.
inline std::size_t safe_utf8_prefix_size(const std::string& text,
                                         std::size_t byte_index) noexcept {
    const std::size_t limit = std::min(byte_index, text.size());
    std::size_t i = 0;
    std::size_t last_good = 0;

    auto cont = [](unsigned char b) noexcept { return (b & 0xC0) == 0x80; };
    while (i < limit) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        if (b0 < 0x80) {
            len = 1;
        } else if (b0 >= 0xC2 && b0 <= 0xDF) {
            len = 2;
        } else if (b0 >= 0xE0 && b0 <= 0xEF) {
            len = 3;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            len = 4;
        } else {
            break;
        }

        if (i + len > limit) break;
        if (len >= 2 && !cont(static_cast<unsigned char>(text[i + 1]))) break;
        if (len >= 3 && !cont(static_cast<unsigned char>(text[i + 2]))) break;
        if (len >= 4 && !cont(static_cast<unsigned char>(text[i + 3]))) break;
        if (len == 3) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b0 == 0xE0 && b1 < 0xA0) || (b0 == 0xED && b1 >= 0xA0)) break;
        } else if (len == 4) {
            const auto b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b0 == 0xF0 && b1 < 0x90) || (b0 == 0xF4 && b1 >= 0x90)) break;
        }

        i += len;
        last_good = i;
    }
    return last_good;
}

} // namespace pulp::canvas
