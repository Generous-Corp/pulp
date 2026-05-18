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
#include <future>
#include <string>
#include <vector>

// pulp #2163 — font v2 Slice 2.4. Probe for ICU's BreakIterator
// headers. The bundled Skia binaries statically link `libskunicode_icu.a`
// (so `SkUnicode_icu::makeBreakIterator` lives in libskia at link time),
// but ICU's own public headers (`<unicode/brkiter.h>`) are *not* shipped
// in `external/skia-build/include/`. Builds that happen to have a system
// ICU on the include path (e.g. Homebrew `icu4c` with an explicit
// `-I/opt/homebrew/opt/icu4c/include` and matching `-L`) will pick up
// the real implementation; otherwise we fall back to the documented
// degraded path: `word_break_step` defers to `cluster_step`, and
// `line_break_opportunities` returns the trailing offset plus every
// ASCII-space boundary. The API is always linkable.
#if defined(PULP_HAS_SKIA) && __has_include(<unicode/brkiter.h>) && __has_include(<unicode/locid.h>)
#  define PULP_HAS_ICU_BREAK_ITERATOR 1
#  include <unicode/brkiter.h>
#  include <unicode/locid.h>
#  include <unicode/unistr.h>
#  include <unicode/utext.h>
#  include <memory>
#else
#  define PULP_HAS_ICU_BREAK_ITERATOR 0
#endif

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

// ── Slice 2.4 — locale-aware word + line breaking (pulp #2163) ─────────
//
// The public surface (`word_break_step`, `line_break_opportunities`)
// is always defined regardless of whether ICU's public headers are on
// the include path. When PULP_HAS_ICU_BREAK_ITERATOR is 1, both
// functions walk an `icu::BreakIterator`. Otherwise we fall back to:
//   * `word_break_step` → `cluster_step` (the same single-cluster
//     advance the editor already uses for caret movement).
//   * `line_break_opportunities` → trailing offset only, plus every
//     ASCII-space byte position. This is enough to keep paragraph
//     layout from collapsing to a single run, and matches the
//     degraded path documented in the header.

#if PULP_HAS_ICU_BREAK_ITERATOR

namespace {

inline icu::Locale icu_locale_for(const std::string& bcp47) {
    if (bcp47.empty()) return icu::Locale::getRoot();
    UErrorCode status = U_ZERO_ERROR;
    icu::Locale loc = icu::Locale::forLanguageTag(bcp47.c_str(), status);
    if (U_FAILURE(status)) return icu::Locale::getRoot();
    return loc;
}

// Build an ICU UnicodeString from UTF-8 input and a parallel index
// from UTF-16 code-unit offsets back to UTF-8 byte offsets. ICU
// BreakIterator returns offsets in code units (UTF-16), but our
// callers speak UTF-8 byte offsets, so we translate at the boundary.
struct Utf8Utf16Bridge {
    icu::UnicodeString utf16;
    // For each UTF-16 code-unit position `k` (0 .. utf16.length()),
    // utf8_for_utf16[k] is the corresponding UTF-8 byte offset in
    // the original input. Length is utf16.length() + 1; the final
    // entry is the input's byte length.
    std::vector<std::size_t> utf8_for_utf16;
};

// Inline UTF-8 scalar decode used by the bridge. The anonymous-
// namespace helper above is not reachable here (different TU scope);
// keep this duplicate trivially small.
inline std::size_t utf8_scalar_len(const std::string& s, std::size_t i) {
    if (i >= s.size()) return 0;
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) return 2;
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) return 3;
    if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) return 4;
    return 1; // malformed: advance by 1 byte
}

inline Utf8Utf16Bridge build_bridge(const std::string& text) {
    Utf8Utf16Bridge b;
    b.utf16 = icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(),
                                                            static_cast<std::int32_t>(text.size())));
    b.utf8_for_utf16.reserve(static_cast<std::size_t>(b.utf16.length()) + 1);

    // Walk UTF-8 input one scalar at a time; for each scalar, record
    // its starting UTF-8 byte offset against the UTF-16 unit position
    // we have advanced to in `b.utf16` (1 unit for BMP, 2 units for
    // supplementary planes).
    std::size_t utf8_pos = 0;
    while (utf8_pos < text.size()) {
        b.utf8_for_utf16.push_back(utf8_pos);
        const std::size_t bytes = utf8_scalar_len(text, utf8_pos);
        // Determine how many UTF-16 units that scalar consumed:
        // 4-byte UTF-8 => supplementary => 2 UTF-16 units.
        const bool supplementary = (bytes == 4);
        if (supplementary) {
            // Surrogate pair: the trailing UTF-16 surrogate also
            // points to the same UTF-8 byte offset start. ICU may
            // return a break offset *between* the surrogates; we
            // collapse those back to the scalar's UTF-8 start.
            b.utf8_for_utf16.push_back(utf8_pos);
        }
        utf8_pos += bytes;
    }
    // Trailing sentinel: last UTF-16 position maps to text.size().
    b.utf8_for_utf16.push_back(text.size());
    return b;
}

inline std::size_t utf16_for_utf8(const Utf8Utf16Bridge& b,
                                  std::size_t utf8_offset) {
    // Binary search via std::lower_bound on the sorted (monotonic
    // non-decreasing) utf8_for_utf16 vector.
    auto it = std::lower_bound(b.utf8_for_utf16.begin(),
                               b.utf8_for_utf16.end(),
                               utf8_offset);
    if (it == b.utf8_for_utf16.end()) {
        return b.utf8_for_utf16.size() - 1;
    }
    return static_cast<std::size_t>(it - b.utf8_for_utf16.begin());
}

} // namespace

#endif // PULP_HAS_ICU_BREAK_ITERATOR

std::size_t word_break_step(const std::string& text,
                            std::size_t byte_offset,
                            const std::string& locale,
                            bool forward) {
#if PULP_HAS_ICU_BREAK_ITERATOR
    if (text.empty()) return 0;
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> it(
        icu::BreakIterator::createWordInstance(icu_locale_for(locale), status));
    if (U_FAILURE(status) || !it) {
        return cluster_step(text, byte_offset, forward);
    }
    Utf8Utf16Bridge bridge = build_bridge(text);
    it->setText(bridge.utf16);
    const std::int32_t cursor16 =
        static_cast<std::int32_t>(utf16_for_utf8(bridge, byte_offset));
    std::int32_t next16 = forward ? it->following(cursor16)
                                  : it->preceding(cursor16);
    if (next16 == icu::BreakIterator::DONE) {
        return forward ? text.size() : 0;
    }
    if (next16 < 0) next16 = 0;
    if (static_cast<std::size_t>(next16) >= bridge.utf8_for_utf16.size()) {
        return text.size();
    }
    return bridge.utf8_for_utf16[static_cast<std::size_t>(next16)];
#else
    (void)locale;
    return cluster_step(text, byte_offset, forward);
#endif
}

std::vector<std::size_t> line_break_opportunities(const std::string& text,
                                                  const std::string& locale) {
    std::vector<std::size_t> out;
#if PULP_HAS_ICU_BREAK_ITERATOR
    if (text.empty()) {
        out.push_back(0);
        return out;
    }
    UErrorCode status = U_ZERO_ERROR;
    std::unique_ptr<icu::BreakIterator> it(
        icu::BreakIterator::createLineInstance(icu_locale_for(locale), status));
    if (U_FAILURE(status) || !it) {
        // Degraded fallback: trailing offset only.
        out.push_back(text.size());
        return out;
    }
    Utf8Utf16Bridge bridge = build_bridge(text);
    it->setText(bridge.utf16);

    out.reserve(8);
    for (std::int32_t pos = it->first();
         pos != icu::BreakIterator::DONE;
         pos = it->next()) {
        if (pos < 0) continue;
        const std::size_t u8 =
            (static_cast<std::size_t>(pos) < bridge.utf8_for_utf16.size())
                ? bridge.utf8_for_utf16[static_cast<std::size_t>(pos)]
                : text.size();
        if (out.empty() || out.back() != u8) {
            out.push_back(u8);
        }
    }
    if (out.empty() || out.back() != text.size()) {
        out.push_back(text.size());
    }
    return out;
#else
    (void)locale;
    if (text.empty()) {
        out.push_back(0);
        return out;
    }
    // ASCII-space boundaries: emit the byte position immediately AFTER
    // each space, matching ICU's UAX #14 behaviour for plain English
    // ("Hello world" breaks at offset 6, i.e. just after the space).
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == ' ' || text[i] == '\t') {
            const std::size_t boundary = i + 1;
            if (boundary < text.size()) out.push_back(boundary);
        }
    }
    out.push_back(text.size());
    return out;
#endif
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

// pulp #2163 — font v2 Slice 2.1 (non-Skia). register_font_url has
// no register_font to land on, so every URL resolves to Failed.
// Matches the contract of the Skia-side implementation: never block
// the caller, always return a ready future.
std::future<FontState> register_font_url(const std::string&,
                                         const std::string&) {
    std::promise<FontState> p;
    p.set_value(FontState::Failed);
    return p.get_future();
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

// pulp #2163 — font v2 Slice 3.5 (non-Skia). Without Skia we can't
// register a typeface even if we had a decoder, so the result is
// always false. Still validates the WOFF2 magic bytes so the
// "is this even a WOFF2 file?" test surface returns the same answer
// on both Skia and non-Skia builds — that property keeps the
// `register_font_woff2` test suite uniform regardless of how Skia
// was configured.
bool register_font_woff2(const std::uint8_t* woff2_data, std::size_t size,
                         const std::string&) {
    if (!woff2_data || size < 4) return false;
    const std::uint32_t magic =
        (static_cast<std::uint32_t>(woff2_data[0]) << 24)
      | (static_cast<std::uint32_t>(woff2_data[1]) << 16)
      | (static_cast<std::uint32_t>(woff2_data[2]) <<  8)
      |  static_cast<std::uint32_t>(woff2_data[3]);
    if (magic != 0x774F4632u) return false;   // 'wOF2'
    // Magic is correct but we have neither a decoder nor Skia to
    // hand the result to. Surface as a clean failure; consult
    // `woff2_decoder_available()` ahead of time to avoid this path.
    return false;
}

bool woff2_decoder_available() noexcept {
    // Non-Skia builds never carry the decoder regardless of whether
    // `<woff2/decode.h>` happens to be present in the include path,
    // because `register_font_woff2` has nowhere to land the
    // decompressed sfnt — `register_font` is itself a `return false`
    // stub on this build.
    return false;
}

} // namespace pulp::canvas

#endif // !PULP_HAS_SKIA
