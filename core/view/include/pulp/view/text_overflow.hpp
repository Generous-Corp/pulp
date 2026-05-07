// SPDX-License-Identifier: MIT
//
// pulp #1407 — CSS `text-overflow: ellipsis` truncation helper.
//
// Translates a string to its ellipsised form when its measured width
// exceeds the available content-box. Operates on UTF-8 codepoints so
// truncation never splits a multibyte sequence, and uses U+2026 (…)
// rather than three ASCII dots, matching CSS behavior.

#pragma once

#include <pulp/canvas/canvas.hpp>
#include <string>
#include <string_view>

namespace pulp::view {

/// UTF-8 horizontal ellipsis character (U+2026, "…"). Three bytes:
/// 0xE2 0x80 0xA6. Exposed for tests that need to assert paint output.
inline constexpr const char* kEllipsis = "\xe2\x80\xa6";

/// Walk forward over UTF-8 leading bytes. Returns the byte index after
/// `count` codepoints, or text.size() if `count` runs off the end.
inline std::size_t utf8_advance(std::string_view text, std::size_t count) {
    std::size_t i = 0;
    while (count > 0 && i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80)        i += 1;
        else if (c < 0xC0)   i += 1;        // stray continuation byte → skip
        else if (c < 0xE0)   i += 2;
        else if (c < 0xF0)   i += 3;
        else                 i += 4;
        if (i > text.size()) i = text.size();
        --count;
    }
    return i;
}

/// Count the number of UTF-8 leading-byte codepoints in `text`. Stray
/// continuation bytes are skipped silently (best-effort, never panics).
inline std::size_t utf8_codepoint_count(std::string_view text) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80)        i += 1;
        else if (c < 0xC0)   i += 1;
        else if (c < 0xE0)   i += 2;
        else if (c < 0xF0)   i += 3;
        else                 i += 4;
        if (i > text.size()) i = text.size();
        ++n;
    }
    return n;
}

/// If `text` already fits within `available_width`, return it as-is.
/// Otherwise return the longest UTF-8 prefix of `text` that — appended
/// with U+2026 — still fits. Falls back to "…" alone if no prefix fits.
///
/// `available_width <= 0.0f` (zero / collapsed content-box) returns
/// "…" rather than the original — leaking the full text on a tiny
/// button defeats the whole point of the feature.
///
/// Uses binary search over codepoint indices, so cost is
/// O(log N · measure_text). The caller is responsible for setting the
/// canvas's font state before invoking; this helper only measures.
inline std::string truncate_to_width(canvas::Canvas& canvas,
                                     std::string_view text,
                                     float available_width) {
    if (available_width <= 0.0f) return std::string(kEllipsis);
    if (canvas.measure_text(std::string(text)) <= available_width)
        return std::string(text);

    const std::size_t total = utf8_codepoint_count(text);
    if (total == 0) return std::string(text);

    // Reserve at least the ellipsis itself.
    if (canvas.measure_text(kEllipsis) > available_width) {
        // Even "…" doesn't fit — return "…" anyway so callers can still
        // signal truncation. The widget will visually clip via its
        // bounding box.
        return std::string(kEllipsis);
    }

    // Binary search for the largest codepoint prefix that fits with "…".
    std::size_t lo = 0;          // always fits (with ellipsis) — invariant: 0-prefix + "…"
    std::size_t hi = total;      // never fits at hi (we know total doesn't fit)
    while (lo + 1 < hi) {
        std::size_t mid = (lo + hi) / 2;
        std::string candidate(text.substr(0, utf8_advance(text, mid)));
        candidate.append(kEllipsis);
        if (canvas.measure_text(candidate) <= available_width) lo = mid;
        else                                                    hi = mid;
    }

    std::string out(text.substr(0, utf8_advance(text, lo)));
    out.append(kEllipsis);
    return out;
}

} // namespace pulp::view
