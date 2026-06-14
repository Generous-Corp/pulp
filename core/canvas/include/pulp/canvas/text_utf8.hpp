#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::canvas {

/// Return the UTF-8 byte offset of the grapheme-cluster boundary one step
/// forward or backward from `byte_offset`.
std::size_t cluster_step(const std::string& text, std::size_t byte_offset,
                         bool forward);

/// Return the largest prefix length <= byte_index that ends on a valid UTF-8
/// scalar boundary. Malformed or truncated input stops at the last known-good
/// boundary so platform text APIs never receive an invalid substring while
/// computing caret offsets.
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

namespace detail {

struct Utf8ScalarForOffsets {
    std::uint32_t codepoint = 0xFFFDu;
    std::size_t bytes = 1;
};

inline bool utf8_continuation(unsigned char b) noexcept { return (b & 0xC0u) == 0x80u; }

inline Utf8ScalarForOffsets decode_utf8_scalar_for_offsets(const std::string& text,
                                                           std::size_t i) noexcept {
    if (i >= text.size()) return {};
    const auto b0 = static_cast<unsigned char>(text[i]);
    if (b0 < 0x80u) return {b0, 1};

    if (b0 >= 0xC2u && b0 <= 0xDFu && i + 1 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        if (utf8_continuation(b1))
            return {static_cast<std::uint32_t>(((b0 & 0x1Fu) << 6) | (b1 & 0x3Fu)), 2};
    }

    if (b0 >= 0xE0u && b0 <= 0xEFu && i + 2 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        const auto b2 = static_cast<unsigned char>(text[i + 2]);
        const bool valid_prefix = (b0 != 0xE0u || b1 >= 0xA0u) && (b0 != 0xEDu || b1 < 0xA0u);
        if (valid_prefix && utf8_continuation(b1) && utf8_continuation(b2)) {
            return {static_cast<std::uint32_t>(((b0 & 0x0Fu) << 12)
                    | ((b1 & 0x3Fu) << 6) | (b2 & 0x3Fu)), 3};
        }
    }

    if (b0 >= 0xF0u && b0 <= 0xF4u && i + 3 < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        const auto b2 = static_cast<unsigned char>(text[i + 2]);
        const auto b3 = static_cast<unsigned char>(text[i + 3]);
        const bool valid_prefix = (b0 != 0xF0u || b1 >= 0x90u) && (b0 != 0xF4u || b1 < 0x90u);
        if (valid_prefix && utf8_continuation(b1) && utf8_continuation(b2) && utf8_continuation(b3)) {
            return {static_cast<std::uint32_t>(((b0 & 0x07u) << 18)
                    | ((b1 & 0x3Fu) << 12) | ((b2 & 0x3Fu) << 6) | (b3 & 0x3Fu)), 4};
        }
    }

    return {};
}

inline std::size_t utf16_units_for_codepoint(std::uint32_t cp) noexcept {
    return cp >= 0x10000u ? 2u : 1u;
}

} // namespace detail

/// Convert a UTF-16 code-unit offset into the nearest safe UTF-8 byte offset.
/// Native text APIs such as Cocoa NSTextInputClient express marked-text ranges
/// in UTF-16, while Pulp stores editor positions as UTF-8 byte offsets.
inline std::size_t utf8_offset_for_utf16_offset(const std::string& text,
                                                std::size_t utf16_offset) noexcept {
    std::size_t utf16_cursor = 0;
    std::size_t byte_cursor = 0;
    while (byte_cursor < text.size()) {
        if (utf16_cursor >= utf16_offset) return byte_cursor;
        const auto scalar = detail::decode_utf8_scalar_for_offsets(text, byte_cursor);
        const auto next_utf16 = utf16_cursor + detail::utf16_units_for_codepoint(scalar.codepoint);
        if (utf16_offset < next_utf16) return byte_cursor;
        byte_cursor += std::max<std::size_t>(1, scalar.bytes);
        utf16_cursor = next_utf16;
        if (utf16_cursor == utf16_offset) return byte_cursor;
    }
    return text.size();
}

/// Convert a UTF-8 byte offset into a UTF-16 code-unit offset. Offsets inside a
/// UTF-8 scalar snap back to the scalar boundary, matching safe prefix handling.
inline std::size_t utf16_offset_for_utf8_offset(const std::string& text,
                                                std::size_t byte_offset) noexcept {
    byte_offset = safe_utf8_prefix_size(text, byte_offset);
    std::size_t utf16_cursor = 0;
    std::size_t byte_cursor = 0;
    while (byte_cursor < byte_offset && byte_cursor < text.size()) {
        const auto scalar = detail::decode_utf8_scalar_for_offsets(text, byte_cursor);
        byte_cursor += std::max<std::size_t>(1, scalar.bytes);
        utf16_cursor += detail::utf16_units_for_codepoint(scalar.codepoint);
    }
    return utf16_cursor;
}

} // namespace pulp::canvas
