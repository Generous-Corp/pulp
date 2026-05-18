// font_registry_stubs.cpp — non-Skia fallback for the public font-registration
// API + Skia-independent helpers in `pulp/canvas/bundled_fonts.hpp` (pulp #1150).
//
// pulp #2163 — font v2 Slice 3.6. `cluster_step` is a UAX #29-lite
// walker over UTF-8 input, byte-encoding-only (no Skia / no platform
// dependency). It compiles in BOTH Skia and non-Skia builds; the rest
// of this TU (`register_font` family, `validate_font_bytes`,
// `register_font_woff2`) stays under the `#ifndef PULP_HAS_SKIA` guard
// since those are stubs replaced by the real impls in
// `bundled_fonts.cpp` when Skia is linked.

#include <pulp/canvas/bundled_fonts.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pulp::canvas {

namespace {

// Read one UTF-8 scalar at `p` (returns codepoint + byte length).
// Returns {0,1} on malformed input so the walker advances by 1 byte
// rather than getting stuck — matches the leniency of the legacy
// naive walker this replaces.
struct Utf8Decoded { std::uint32_t cp; std::size_t bytes; };

// Codex review on PR #2185 (P2): validate every trailing byte is a
// proper UTF-8 continuation (top two bits == 0b10) before accepting
// a multi-byte scalar. Malformed inputs like `0xC2 0x41` must fall
// back to {0,1} so cluster_step doesn't skip bytes on invalid UTF-8.
inline bool is_utf8_cont(unsigned char b) noexcept {
    return (b & 0xC0u) == 0x80u;
}

inline Utf8Decoded utf8_decode_at(const std::string& s, std::size_t i) noexcept {
    if (i >= s.size()) return {0, 0};
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return {c, 1};
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        if (!is_utf8_cont(b1)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x1Fu) << 6)
                                         | (b1 & 0x3Fu)), 2};
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        if (!is_utf8_cont(b1) || !is_utf8_cont(b2)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x0Fu) << 12)
                                         | ((b1 & 0x3Fu) << 6)
                                         | (b2 & 0x3Fu)), 3};
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
        unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
        unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
        unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
        if (!is_utf8_cont(b1) || !is_utf8_cont(b2) || !is_utf8_cont(b3)) return {0, 1};
        return {static_cast<std::uint32_t>(((c & 0x07u) << 18)
                                         | ((b1 & 0x3Fu) << 12)
                                         | ((b2 & 0x3Fu) << 6)
                                         | (b3 & 0x3Fu)), 4};
    }
    return {0, 1};
}

// UAX #29-lite cluster joiners. A cluster_step starting on a base
// codepoint absorbs any of these "extend" codepoints that follow:
//   * Combining marks (U+0300..U+036F, U+1AB0..U+1AFF, U+1DC0..U+1DFF,
//     U+20D0..U+20FF, U+FE20..U+FE2F) — Latin / IPA / generic
//   * Variation selectors (U+FE00..U+FE0F, U+E0100..U+E01EF) — emoji
//     presentation switches
//   * Devanagari/Indic virama signs (U+094D, U+09CD, U+0A4D, U+0ACD,
//     U+0B4D, U+0BCD, U+0C4D, U+0CCD, U+0D4D, U+0DCA) — explicit
//     conjunct formers
//   * Zero-Width Joiner (U+200D) — emoji ZWJ sequences
//   * Emoji modifiers / skin-tone (U+1F3FB..U+1F3FF)
//   * Hangul T (trailing) jamo (U+11A8..U+11FF, U+D7CB..U+D7FB)
inline bool is_cluster_extend(std::uint32_t cp) noexcept {
    if (cp >= 0x0300 && cp <= 0x036F) return true;
    if (cp >= 0x1AB0 && cp <= 0x1AFF) return true;
    if (cp >= 0x1DC0 && cp <= 0x1DFF) return true;
    if (cp >= 0x20D0 && cp <= 0x20FF) return true;
    if (cp >= 0xFE00 && cp <= 0xFE0F) return true;
    if (cp >= 0xFE20 && cp <= 0xFE2F) return true;
    if (cp == 0x200D)                  return true;  // ZWJ
    if (cp == 0x094D || cp == 0x09CD || cp == 0x0A4D || cp == 0x0ACD ||
        cp == 0x0B4D || cp == 0x0BCD || cp == 0x0C4D || cp == 0x0CCD ||
        cp == 0x0D4D || cp == 0x0DCA) return true;
    if (cp >= 0x1F3FB && cp <= 0x1F3FF) return true;  // skin tone
    if (cp >= 0xE0100 && cp <= 0xE01EF) return true;  // variation selectors
    if (cp >= 0x11A8  && cp <= 0x11FF)  return true;  // Hangul T jamo
    if (cp >= 0xD7CB  && cp <= 0xD7FB)  return true;  // Hangul T jamo extended
    return false;
}

// Regional indicator symbols (RI), used to form flag clusters as
// PAIRS. Two consecutive RIs collapse to one cluster; a third RI
// starts a new pair.
inline bool is_regional_indicator(std::uint32_t cp) noexcept {
    return cp >= 0x1F1E6 && cp <= 0x1F1FF;
}

// Pre-ZWJ extension: when the current codepoint follows a ZWJ, the
// pair joins regardless of category. This lets ZWJ-joined emoji
// sequences collapse into one cluster even when the post-ZWJ glyph
// isn't itself an extender.
inline bool joins_after_zwj(std::uint32_t cp) noexcept {
    // Pictographic-ish ranges that are commonly ZWJ targets. Not a
    // full Unicode `Extended_Pictographic` table; covers BMP symbols
    // + emoji-block codepoints which is the practical 95% case for
    // the corpus this slice targets.
    if (cp >= 0x261D  && cp <= 0x270D)  return true;  // misc dingbats
    if (cp >= 0x2600  && cp <= 0x26FF)  return true;  // misc symbols
    if (cp >= 0x2700  && cp <= 0x27BF)  return true;  // dingbats
    if (cp >= 0x1F300 && cp <= 0x1F9FF) return true;  // emoji blocks
    if (cp >= 0x1FA70 && cp <= 0x1FAFF) return true;  // symbols + pictographs extended
    return false;
}

inline std::size_t skip_utf8_back_to_lead(const std::string& s, std::size_t i) noexcept {
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

} // namespace

// Indic virama codepoints — explicit conjunct formers. A virama
// chains the NEXT consonant into the same cluster (different from
// ordinary extenders which only attach themselves).
inline bool is_virama(std::uint32_t cp) noexcept {
    return cp == 0x094D || cp == 0x09CD || cp == 0x0A4D || cp == 0x0ACD
        || cp == 0x0B4D || cp == 0x0BCD || cp == 0x0C4D || cp == 0x0CCD
        || cp == 0x0D4D || cp == 0x0DCA;
}

// Forward cluster boundary from `i` (which must be at a codepoint
// start). Returns the byte offset of the next cluster boundary, or
// `text.size()` if no further boundary exists.
inline std::size_t cluster_boundary_after(const std::string& text, std::size_t i) noexcept {
    if (i >= text.size()) return text.size();
    Utf8Decoded base = utf8_decode_at(text, i);
    if (base.bytes == 0) return text.size();
    std::size_t cursor = i + base.bytes;

    // Regional indicator pair: UAX #29 GB12/GB13 — do not break
    // between RI symbols if there is an ODD number of RIs before
    // the break point. Equivalently: pair forward only when the
    // run of preceding RIs is even (`i` is at the start of a fresh
    // pair). Without the parity check, `cluster_step` called mid-
    // sequence (e.g. inside `🇺🇸🇯🇵` at offset 4) would greedily
    // absorb the next RI and skip the boundary that exists between
    // the two flags. (Codex review on PR #2185, P2.)
    if (is_regional_indicator(base.cp)) {
        std::size_t preceding_ri_count = 0;
        std::size_t cursor_back = i;
        while (cursor_back > 0) {
            std::size_t lead = skip_utf8_back_to_lead(text, cursor_back - 1);
            Utf8Decoded prev = utf8_decode_at(text, lead);
            if (prev.bytes == 0 || !is_regional_indicator(prev.cp)) break;
            ++preceding_ri_count;
            cursor_back = lead;
        }
        const bool pair_forward = (preceding_ri_count % 2 == 0);
        if (pair_forward && cursor < text.size()) {
            Utf8Decoded nxt = utf8_decode_at(text, cursor);
            if (nxt.bytes && is_regional_indicator(nxt.cp)) {
                cursor += nxt.bytes;
            }
        }
        return cursor;
    }

    // General extension + ZWJ-joined + post-virama absorbing loop.
    bool prev_was_zwj = false;
    bool prev_was_virama = is_virama(base.cp);
    while (cursor < text.size()) {
        Utf8Decoded nxt = utf8_decode_at(text, cursor);
        if (nxt.bytes == 0) break;
        // Post-virama: absorb the next consonant regardless of class.
        if (prev_was_virama) {
            prev_was_virama = is_virama(nxt.cp);
            cursor += nxt.bytes;
            continue;
        }
        if (is_cluster_extend(nxt.cp)) {
            prev_was_zwj    = (nxt.cp == 0x200D);
            prev_was_virama = is_virama(nxt.cp);
            cursor += nxt.bytes;
            continue;
        }
        if (prev_was_zwj && joins_after_zwj(nxt.cp)) {
            prev_was_zwj = false;
            cursor += nxt.bytes;
            continue;
        }
        break;
    }
    return cursor;
}

// pulp #2163 — font v2 Slice 3.6. UAX #29-lite cluster boundaries.
// Always defined regardless of PULP_HAS_SKIA — pure UTF-8 walker.
//
// Backward direction implements the standard "scan from start" trick:
// cluster boundaries are not purely locally-decidable (a backward
// codepoint by itself doesn't tell you if it's inside a ZWJ chain or
// post-virama sequence), so we walk forward from byte 0 collecting
// boundaries until we cross `byte_offset`, then return the most
// recent boundary strictly before it. O(n) per call but inputs are
// label-sized; cursor APIs that need O(1) backward step can cache
// boundaries via the Phase 1 UnicodeIndexMap.
std::size_t cluster_step(const std::string& text, std::size_t byte_offset,
                         bool forward) {
    if (forward) {
        if (byte_offset >= text.size()) return text.size();
        // Walk to the start of the codepoint at byte_offset.
        std::size_t i = byte_offset;
        if (i > 0 && (static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) {
            i = skip_utf8_back_to_lead(text, i);
        }
        std::size_t next = cluster_boundary_after(text, i);
        if (next <= byte_offset) return std::min(byte_offset + 1, text.size());
        return next;
    } else {
        if (byte_offset == 0) return 0;
        // Forward scan collecting boundaries until we cross
        // byte_offset; return the last boundary strictly before it.
        std::size_t prev = 0;
        std::size_t cursor = 0;
        while (cursor < text.size() && cursor < byte_offset) {
            std::size_t next = cluster_boundary_after(text, cursor);
            if (next <= cursor) break; // safety: never advance backward
            if (next < byte_offset) {
                prev = next;
                cursor = next;
            } else {
                // `next` is at or past byte_offset: the cluster we're
                // inside started at `cursor`; that's the answer.
                return cursor;
            }
        }
        return prev;
    }
}

} // namespace pulp::canvas

#ifndef PULP_HAS_SKIA

namespace pulp::canvas {

bool register_font(const std::uint8_t*, std::size_t, const std::string&) {
    return false;
}

bool register_font_file(const std::string&, const std::string&) {
    return false;
}

bool is_font_registered(const std::string&) {
    return false;
}

std::uint64_t font_registration_generation() noexcept { return 0; }
void bump_font_registration_generation() noexcept {}

// pulp #2163 — font v2 Slice 2.8 skeleton. See bundled_fonts.hpp.
bool validate_font_bytes(const std::uint8_t* data, std::size_t size) {
    // Non-Skia builds: accept any non-empty buffer. Real
    // sanitizer arrives with the Phase 2 implementation slice.
    return data != nullptr && size > 0;
}

// pulp #2163 — font v2 Phase 3 skeletons.
bool register_font_woff2(const std::uint8_t*, std::size_t, const std::string&) {
    return false;  // Phase 3 impl wires Brotli decompression + sanitizer.
}

} // namespace pulp::canvas

#endif // !PULP_HAS_SKIA
