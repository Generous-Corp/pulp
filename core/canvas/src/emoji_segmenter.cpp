// emoji_segmenter.cpp — see header for the contract.
//
// Implementation strategy:
//   1. Decode UTF-8 into a temporary `[(byte_offset, codepoint)]` list.
//   2. Classify every codepoint as one of:
//        Emoji, EmojiTextDefault, ZWJ, RegionalIndicator, KeycapMark,
//        Modifier, TagSpec, VariationEmoji (FE0F), VariationText (FE0E),
//        Extender (combining), Default.
//      Classification uses a small set of curated Unicode 16.0 ranges
//      that cover all real emoji as of the Pulp cutoff. The ranges are
//      intentionally conservative (we'd rather classify a non-emoji as
//      `Default` than break a non-emoji cluster).
//   3. Walk codepoints forming grapheme clusters per UAX #29 / UTS #51:
//      - A cluster may extend rightward across ZWJ, FE0F, FE0E, tags,
//        keycap mark, skin-tone modifiers, and emoji extenders.
//      - Regional Indicator clusters are paired exactly (two RIs = one
//        flag; an isolated odd RI starts a new cluster).
//      - Tag-base + tag-spec + cancel (E007F) groups as one cluster.
//   4. For each cluster, decide the cluster role:
//      - Contains FE0E → Default (text presentation overrides).
//      - Contains any Emoji / RegionalIndicator / Keycap / Modifier /
//        TagSpec / FE0F → Emoji.
//      - Otherwise → Default.
//   5. Coalesce adjacent same-role clusters into runs.
//
// Important non-decisions:
//   - We do NOT call HarfBuzz here. The point is to pre-segment runs so
//     the downstream shaper sees one cluster per font run.
//   - We do NOT trust the Unicode property database in `external/icu/` —
//     Pulp's Skia prebuilt bundles ICU, but pulling it in for a pure
//     segmenter would be a big hammer. The curated ranges below cover
//     U16.0 (cutoff 2025-09).

#include <pulp/canvas/emoji_segmenter.hpp>

#include <algorithm>
#include <array>

namespace pulp::canvas {
namespace {

enum class CodepointKind : unsigned char {
    Default,
    EmojiDefault,           // Has Emoji_Presentation = Yes (color by default)
    EmojiTextDefault,       // Has Emoji = Yes but Text presentation default
    ZWJ,                    // U+200D
    RegionalIndicator,      // U+1F1E6..U+1F1FF
    KeycapMark,             // U+20E3
    SkinToneModifier,       // U+1F3FB..U+1F3FF
    VariationEmoji,         // U+FE0F
    VariationText,          // U+FE0E
    TagBase,                // U+1F3F4 (the only emoji currently used as a tag base)
    TagSpec,                // U+E0020..U+E007E
    TagCancel,              // U+E007F
};

constexpr bool is_in_range(char32_t cp, char32_t lo, char32_t hi) {
    return cp >= lo && cp <= hi;
}

// Curated ranges that have Emoji_Presentation = Yes (render in color
// even without FE0F). Conservative — when in doubt, leave it out.
constexpr bool is_emoji_presentation_default(char32_t cp) {
    return is_in_range(cp, 0x1F300, 0x1F5FF) ||  // Misc symbols / pictographs
           is_in_range(cp, 0x1F600, 0x1F64F) ||  // Emoticons
           is_in_range(cp, 0x1F680, 0x1F6FF) ||  // Transport / map
           is_in_range(cp, 0x1F7E0, 0x1F7EB) ||  // Coloured circles / squares
           is_in_range(cp, 0x1F7F0, 0x1F7F0) ||  // Heavy equals
           is_in_range(cp, 0x1F900, 0x1F9FF) ||  // Supplemental symbols
           is_in_range(cp, 0x1FA70, 0x1FAFF) ||  // Symbols ext-A (subset)
           is_in_range(cp, 0x2600, 0x26FF) ||    // Misc symbols (some — partial)
           is_in_range(cp, 0x2700, 0x27BF);      // Dingbats (some — partial)
}

// Codepoints with Emoji = Yes but text presentation by default — they
// require FE0F to render in color. Common cases: digits 0-9, #, *, etc.
// We treat them as emoji-capable so the cluster logic can still group
// them with FE0F + 20E3 into a keycap cluster.
constexpr bool is_emoji_text_default(char32_t cp) {
    // ASCII digits, '#', '*' that combine into keycaps with FE0F + 20E3.
    if (cp == 0x0023 || cp == 0x002A) return true;
    if (cp >= 0x0030 && cp <= 0x0039) return true;
    // The selection-of-symbols emoji-with-text-default that we care to
    // promote when paired with FE0F. Subset that meaningfully appears
    // in real text:
    switch (cp) {
        case 0x00A9:  // ©
        case 0x00AE:  // ®
        case 0x203C:  // ‼
        case 0x2049:  // ⁉
        case 0x2122:  // ™
        case 0x2139:  // ℹ
        case 0x2194: case 0x2195: case 0x2196: case 0x2197: case 0x2198: case 0x2199:
        case 0x21A9: case 0x21AA:
        case 0x231A: case 0x231B:  // watch / hourglass (emoji-default but historically text)
        case 0x2328:
        case 0x23CF:
        case 0x23E9: case 0x23EA: case 0x23EB: case 0x23EC: case 0x23ED: case 0x23EE:
        case 0x23EF:
        case 0x23F0:
        case 0x23F1: case 0x23F2: case 0x23F3:
        case 0x23F8: case 0x23F9: case 0x23FA:
        case 0x24C2:
        case 0x25AA: case 0x25AB:
        case 0x25B6: case 0x25C0:
        case 0x25FB: case 0x25FC: case 0x25FD: case 0x25FE:
        case 0x2934: case 0x2935:
        case 0x2B05: case 0x2B06: case 0x2B07:
        case 0x2B1B: case 0x2B1C:
        case 0x2B50:
        case 0x2B55:
        case 0x3030:
        case 0x303D:
        case 0x3297:
        case 0x3299:
            return true;
        default:
            return false;
    }
}

constexpr CodepointKind classify(char32_t cp) {
    if (cp == 0x200D) return CodepointKind::ZWJ;
    if (cp == 0xFE0F) return CodepointKind::VariationEmoji;
    if (cp == 0xFE0E) return CodepointKind::VariationText;
    if (cp == 0x20E3) return CodepointKind::KeycapMark;
    if (cp == 0x1F3F4) return CodepointKind::TagBase;
    if (is_in_range(cp, 0x1F1E6, 0x1F1FF)) return CodepointKind::RegionalIndicator;
    if (is_in_range(cp, 0x1F3FB, 0x1F3FF)) return CodepointKind::SkinToneModifier;
    if (cp == 0xE007F) return CodepointKind::TagCancel;
    if (is_in_range(cp, 0xE0020, 0xE007E)) return CodepointKind::TagSpec;
    if (is_emoji_presentation_default(cp)) return CodepointKind::EmojiDefault;
    if (is_emoji_text_default(cp)) return CodepointKind::EmojiTextDefault;
    return CodepointKind::Default;
}

struct DecodedCodepoint {
    std::size_t byte_offset;
    std::size_t byte_length;
    char32_t cp;
    CodepointKind kind;
};

// Minimal UTF-8 decoder. Malformed sequences are emitted as one-byte
// `Default` codepoints with cp = the raw byte. This guarantees the
// segmenter never crashes on garbage and that runs still cover the
// full byte span.
std::vector<DecodedCodepoint> decode_utf8(std::string_view text) {
    std::vector<DecodedCodepoint> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char b0 = static_cast<unsigned char>(text[i]);
        std::size_t len = 0;
        char32_t cp = 0;
        if (b0 < 0x80) {
            cp = b0;
            len = 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            if ((b1 & 0xC0) == 0x80) {
                cp = (static_cast<char32_t>(b0 & 0x1F) << 6)
                   | static_cast<char32_t>(b1 & 0x3F);
                len = 2;
            }
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80) {
                cp = (static_cast<char32_t>(b0 & 0x0F) << 12)
                   | (static_cast<char32_t>(b1 & 0x3F) << 6)
                   | static_cast<char32_t>(b2 & 0x3F);
                len = 3;
            }
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[i + 1]);
            unsigned char b2 = static_cast<unsigned char>(text[i + 2]);
            unsigned char b3 = static_cast<unsigned char>(text[i + 3]);
            if ((b1 & 0xC0) == 0x80 && (b2 & 0xC0) == 0x80 && (b3 & 0xC0) == 0x80) {
                cp = (static_cast<char32_t>(b0 & 0x07) << 18)
                   | (static_cast<char32_t>(b1 & 0x3F) << 12)
                   | (static_cast<char32_t>(b2 & 0x3F) << 6)
                   | static_cast<char32_t>(b3 & 0x3F);
                len = 4;
            }
        }
        if (len == 0) {
            // Malformed lead byte or truncated tail — emit as single
            // byte with kind = Default so we don't lose coverage.
            cp = b0;
            len = 1;
        }
        out.push_back({i, len, cp, classify(cp)});
        i += len;
    }
    return out;
}

// One grapheme cluster spans [start_idx, end_idx) in `decoded` and is
// tagged with a role. The end_idx is exclusive in codepoint-index land.
struct Cluster {
    std::size_t start_idx;
    std::size_t end_idx;
    FontRunRole role;
};

bool cluster_extends_with(const DecodedCodepoint& prev,
                          const DecodedCodepoint& current,
                          bool prev_cluster_is_emoji) {
    // ZWJ joins emoji clusters: only extend if the previous codepoint
    // was part of an emoji-typed cluster.
    if (current.kind == CodepointKind::ZWJ) {
        return prev_cluster_is_emoji;
    }
    // The codepoint AFTER a ZWJ extends the cluster iff it is emoji-
    // capable. We can't tell that without looking at `prev` — the
    // caller handles this by tracking whether prev was a ZWJ.
    if (prev.kind == CodepointKind::ZWJ) {
        return current.kind == CodepointKind::EmojiDefault
            || current.kind == CodepointKind::EmojiTextDefault
            || current.kind == CodepointKind::TagBase;
    }
    // Variation selectors extend the previous cluster unconditionally
    // (FE0F promotes, FE0E demotes).
    if (current.kind == CodepointKind::VariationEmoji
     || current.kind == CodepointKind::VariationText) {
        return true;
    }
    // Skin-tone modifiers extend a preceding emoji-typed codepoint.
    if (current.kind == CodepointKind::SkinToneModifier) {
        return prev.kind == CodepointKind::EmojiDefault
            || prev.kind == CodepointKind::EmojiTextDefault
            || prev.kind == CodepointKind::VariationEmoji;
    }
    // Keycap mark extends a preceding text-default emoji + FE0F.
    if (current.kind == CodepointKind::KeycapMark) {
        return prev.kind == CodepointKind::VariationEmoji
            || prev.kind == CodepointKind::EmojiTextDefault
            || prev.kind == CodepointKind::EmojiDefault;
    }
    // Tag specs / cancel extend a tag-base cluster.
    if (current.kind == CodepointKind::TagSpec
     || current.kind == CodepointKind::TagCancel) {
        return prev_cluster_is_emoji;
    }
    return false;
}

} // namespace

std::vector<FontRun> segment_emoji_runs(std::string_view utf8_text) {
    std::vector<FontRun> runs;
    if (utf8_text.empty()) return runs;

    const auto decoded = decode_utf8(utf8_text);
    if (decoded.empty()) return runs;

    // Pass 1: form grapheme clusters with roles.
    std::vector<Cluster> clusters;
    clusters.reserve(decoded.size());

    std::size_t i = 0;
    while (i < decoded.size()) {
        Cluster c{i, i + 1, FontRunRole::Default};

        // Determine initial role from this codepoint.
        const auto& cp0 = decoded[i];
        bool has_emoji_marker = false;
        bool has_text_marker = false;

        auto mark_emoji = [&](const DecodedCodepoint& dc) {
            switch (dc.kind) {
                case CodepointKind::EmojiDefault:
                case CodepointKind::RegionalIndicator:
                case CodepointKind::SkinToneModifier:
                case CodepointKind::KeycapMark:
                case CodepointKind::TagBase:
                case CodepointKind::TagSpec:
                case CodepointKind::VariationEmoji:
                    has_emoji_marker = true;
                    break;
                case CodepointKind::VariationText:
                    has_text_marker = true;
                    break;
                default:
                    break;
            }
        };

        mark_emoji(cp0);

        // Regional Indicator: pair exactly two if available.
        if (cp0.kind == CodepointKind::RegionalIndicator) {
            if (i + 1 < decoded.size()
             && decoded[i + 1].kind == CodepointKind::RegionalIndicator) {
                c.end_idx = i + 2;
                mark_emoji(decoded[i + 1]);
            }
            c.role = FontRunRole::Emoji;
            clusters.push_back(c);
            i = c.end_idx;
            continue;
        }

        // Walk forward extending the cluster as long as the next
        // codepoint is a cluster-extender.
        while (c.end_idx < decoded.size()) {
            const auto& current = decoded[c.end_idx];
            const auto& prev = decoded[c.end_idx - 1];
            const bool prev_is_emoji = has_emoji_marker && !has_text_marker;
            if (!cluster_extends_with(prev, current, prev_is_emoji)) break;
            mark_emoji(current);
            ++c.end_idx;
        }

        // Decide cluster role:
        //  - Explicit text presentation (FE0E) always wins.
        //  - Otherwise any emoji marker → emoji.
        if (has_text_marker) {
            c.role = FontRunRole::Default;
        } else if (has_emoji_marker
                   && (cp0.kind == CodepointKind::EmojiDefault
                       || cp0.kind == CodepointKind::RegionalIndicator
                       || cp0.kind == CodepointKind::TagBase
                       || (cp0.kind == CodepointKind::EmojiTextDefault
                           && has_emoji_marker))) {
            c.role = FontRunRole::Emoji;
        }

        clusters.push_back(c);
        i = c.end_idx;
    }

    // Pass 2: coalesce adjacent same-role clusters into runs.
    runs.reserve(clusters.size());
    for (const auto& c : clusters) {
        std::size_t byte_start = decoded[c.start_idx].byte_offset;
        std::size_t byte_end = (c.end_idx < decoded.size())
            ? decoded[c.end_idx].byte_offset
            : utf8_text.size();
        if (!runs.empty() && runs.back().role == c.role
         && runs.back().byte_end == byte_start) {
            runs.back().byte_end = byte_end;
        } else {
            runs.push_back({byte_start, byte_end, c.role});
        }
    }

    return runs;
}

bool contains_emoji(std::string_view utf8_text) {
    // Pure-ASCII fast path: no codepoint in the Latin block can ever
    // be an emoji-relevant character, so we can skip the UTF-8 decode
    // entirely. This is the hot path for canvas2d labels.
    bool has_high_byte = false;
    for (unsigned char b : utf8_text) {
        if (b >= 0x80) { has_high_byte = true; break; }
    }
    if (!has_high_byte) return false;

    const auto decoded = decode_utf8(utf8_text);
    for (const auto& dc : decoded) {
        switch (dc.kind) {
            case CodepointKind::EmojiDefault:
            case CodepointKind::RegionalIndicator:
            case CodepointKind::SkinToneModifier:
            case CodepointKind::KeycapMark:
            case CodepointKind::TagBase:
            case CodepointKind::TagSpec:
            case CodepointKind::VariationEmoji:
                return true;
            default:
                break;
        }
    }
    return false;
}

} // namespace pulp::canvas
