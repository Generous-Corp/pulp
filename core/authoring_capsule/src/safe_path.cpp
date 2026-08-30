/// @file safe_path.cpp
/// Path admission for capsule members.
///
/// A member path arrives from an untrusted machine and is eventually joined to
/// a real staging directory, so every rule here fails closed: an input this
/// file cannot prove safe is rejected rather than repaired. Nothing is
/// silently rewritten — an accepted path is returned byte-for-byte as it
/// arrived, so the manifest digest and the extracted name always agree.
///
/// ## Unicode subset — read this before trusting the NFC claim
///
/// The capsule substrate deliberately links neither Skia nor ICU, so no
/// Unicode character database is available here. This file therefore does
/// **not** implement Unicode normalization. It implements the fail-closed
/// alternative: a path is accepted only when every code point in it is drawn
/// from a hand-vetted allowlist whose members are all `ccc = 0`,
/// `NFC_Quick_Check = Yes`, and not Hangul conjoining jamo. A string built
/// exclusively from such code points is already in NFC, because canonical
/// reordering only moves combining marks (all rejected here) and canonical
/// composition only fires between a starter and a following combining mark or
/// between conjoining jamo (also all rejected here).
///
/// The allowlist is:
///
/// | Range | Why it is provably NFC-stable |
/// |---|---|
/// | U+0020..U+007E | ASCII graphic and space; no decompositions |
/// | U+00C0..U+00FF (less U+00D7, U+00F7) | Latin-1 letters; all are the *composed* form, `NFC_QC=Yes` |
/// | U+0100..U+017F | Latin Extended-A; composed forms, no canonical singletons |
/// | U+3041..U+3096 | Hiragana syllables; voiced forms here are the composed form |
/// | U+30A1..U+30FA, U+30FC | Katakana syllables and the prolonged sound mark |
/// | U+3400..U+4DBF, U+4E00..U+9FFF | CJK unified ideographs; no canonical decompositions |
/// | U+AC00..U+D7A3 | Precomposed Hangul syllables |
///
/// Everything else — every combining mark, every conjoining jamo, every
/// script not listed, every CJK *compatibility* ideograph (those are canonical
/// singletons, hence `NFC_QC=No`), and every supplementary-plane code point —
/// is rejected with `path_rejected` and the `nfc-verifiable-subset` rule. That
/// is the honest boundary: this file can prove a path is NFC, and it can
/// refuse; it cannot normalize one. Widening the allowlist means vetting each
/// added range against `DerivedNormalizationProps` for `NFC_QC=Yes` and
/// `Canonical_Combining_Class=0`, or linking a real normalizer.
///
/// A practical consequence worth knowing: the decomposed spelling of an
/// accented name (`e` + U+0301) is rejected, while the precomposed spelling
/// (U+00E9) is accepted. Exporters therefore have to emit NFC, which is what
/// the format already requires of them.

#include <pulp/authoring_capsule/safe_path.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::authoring_capsule {
namespace {

using runtime::Err;
using runtime::Ok;

// ── Rule tokens ─────────────────────────────────────────────────────────────
//
// These land in `CapsuleError::required` and name the exact rule that failed.
// They are machine-readable and stable: a product keys its explanatory copy
// off them, so a token is never reworded in place.

constexpr std::string_view kRuleNonEmpty        = "non-empty-path";
constexpr std::string_view kRuleMaxBytes        = "max-path-bytes";
constexpr std::string_view kRuleMaxDepth        = "max-path-depth";
constexpr std::string_view kRuleUtf8            = "well-formed-utf8";
constexpr std::string_view kRuleNfcSubset       = "nfc-verifiable-subset";
constexpr std::string_view kRuleControl         = "no-control-characters";
constexpr std::string_view kRuleBackslash       = "forward-slash-separator";
constexpr std::string_view kRuleColon           = "no-colon";
constexpr std::string_view kRuleUnc             = "no-unc-prefix";
constexpr std::string_view kRuleDriveLetter     = "no-drive-letter";
constexpr std::string_view kRuleRelative        = "relative-path";
constexpr std::string_view kRuleTrailingSep     = "no-trailing-separator";
constexpr std::string_view kRuleEmptyComponent  = "non-empty-component";
constexpr std::string_view kRuleDotComponent    = "no-dot-component";
constexpr std::string_view kRuleTrailingDotSp   = "no-trailing-dot-or-space";
constexpr std::string_view kRuleReservedChar    = "no-reserved-character";
constexpr std::string_view kRuleReservedName    = "no-reserved-device-name";

runtime::Result<std::string, CapsuleError> reject(std::string_view raw, std::string_view rule) {
    return Err(CapsuleError{CapsuleStatus::path_rejected,
                            std::string(raw),
                            std::string(rule),
                            std::string{}});
}

// ── UTF-8 ───────────────────────────────────────────────────────────────────

struct Decoded {
    char32_t    code_point = 0;
    std::size_t length     = 0;
};

/// Strict UTF-8: shortest form only, no surrogates, nothing past U+10FFFF.
/// A lenient decoder would let two distinct byte strings denote one path,
/// which is exactly the aliasing this module exists to prevent.
bool decode_utf8(std::string_view text, std::size_t offset, Decoded& out) {
    const std::size_t available = text.size() - offset;
    const auto        byte0     = static_cast<unsigned char>(text[offset]);

    auto continuation = [&](std::size_t index, unsigned char low, unsigned char high) {
        const auto byte = static_cast<unsigned char>(text[offset + index]);
        return byte >= low && byte <= high;
    };

    if (byte0 < 0x80) {
        out = Decoded{static_cast<char32_t>(byte0), 1};
        return true;
    }
    if (byte0 >= 0xC2 && byte0 <= 0xDF) {
        if (available < 2 || !continuation(1, 0x80, 0xBF)) return false;
        out = Decoded{static_cast<char32_t>(((byte0 & 0x1FU) << 6) |
                                            (static_cast<unsigned char>(text[offset + 1]) & 0x3FU)),
                      2};
        return true;
    }
    if (byte0 >= 0xE0 && byte0 <= 0xEF) {
        // E0 bars the overlong two-byte range; ED bars the UTF-16 surrogates.
        const unsigned char low  = (byte0 == 0xE0) ? 0xA0 : 0x80;
        const unsigned char high = (byte0 == 0xED) ? 0x9F : 0xBF;
        if (available < 3 || !continuation(1, low, high) || !continuation(2, 0x80, 0xBF)) return false;
        out = Decoded{static_cast<char32_t>(((byte0 & 0x0FU) << 12) |
                                            ((static_cast<unsigned char>(text[offset + 1]) & 0x3FU) << 6) |
                                            (static_cast<unsigned char>(text[offset + 2]) & 0x3FU)),
                      3};
        return true;
    }
    if (byte0 >= 0xF0 && byte0 <= 0xF4) {
        // F0 bars the overlong three-byte range; F4 caps the range at U+10FFFF.
        const unsigned char low  = (byte0 == 0xF0) ? 0x90 : 0x80;
        const unsigned char high = (byte0 == 0xF4) ? 0x8F : 0xBF;
        if (available < 4 || !continuation(1, low, high) || !continuation(2, 0x80, 0xBF) ||
            !continuation(3, 0x80, 0xBF)) {
            return false;
        }
        out = Decoded{static_cast<char32_t>(((byte0 & 0x07U) << 18) |
                                            ((static_cast<unsigned char>(text[offset + 1]) & 0x3FU) << 12) |
                                            ((static_cast<unsigned char>(text[offset + 2]) & 0x3FU) << 6) |
                                            (static_cast<unsigned char>(text[offset + 3]) & 0x3FU)),
                      4};
        return true;
    }
    return false;  // continuation byte in leading position, C0/C1, or F5..FF
}

void encode_utf8(char32_t code_point, std::string& out) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

// ── Character classes ───────────────────────────────────────────────────────

bool is_control(char32_t cp) noexcept {
    // C0 (NUL included), DEL, and C1. None of them survive a round trip through
    // a shell, an archive listing, or a terminal intact, and several are used
    // to hide the real tail of a name.
    return cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F);
}

/// Characters no Windows filesystem accepts in a name. A member carrying one
/// is unextractable on a supported platform, so it is refused at admission
/// rather than half-way through extraction.
bool is_reserved_character(char32_t cp) noexcept {
    return cp == U'<' || cp == U'>' || cp == U'"' || cp == U'|' || cp == U'?' || cp == U'*';
}

/// True when `cp` is provably already in NFC. See the file header for why this
/// is an allowlist and what its exact boundary is.
bool is_nfc_verifiable(char32_t cp) noexcept {
    if (cp >= 0x20 && cp <= 0x7E) return true;                       // ASCII graphic + space
    if (cp >= 0xC0 && cp <= 0xFF) return cp != 0xD7 && cp != 0xF7;   // Latin-1 letters
    if (cp >= 0x0100 && cp <= 0x017F) return true;                   // Latin Extended-A
    if (cp >= 0x3041 && cp <= 0x3096) return true;                   // Hiragana
    if (cp >= 0x30A1 && cp <= 0x30FA) return true;                   // Katakana
    if (cp == 0x30FC) return true;                                   // Katakana-Hiragana prolonged sound
    if (cp >= 0x3400 && cp <= 0x4DBF) return true;                   // CJK unified, extension A
    if (cp >= 0x4E00 && cp <= 0x9FFF) return true;                   // CJK unified
    if (cp >= 0xAC00 && cp <= 0xD7A3) return true;                   // Precomposed Hangul syllables
    return false;
}

// ── Case folding and confusables ────────────────────────────────────────────

/// Simple case folding over the accepted allowlist. Full folding (which would
/// expand U+00DF to "ss") is deliberately out of scope: it is a one-to-many
/// mapping, and no accepted code point other than U+00DF needs it.
char32_t fold_case(char32_t cp) noexcept {
    if (cp >= U'A' && cp <= U'Z') return cp + 0x20;
    if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 0x20;
    // Latin Extended-A alternates upper/lower, but the parity of the pair
    // flips twice across the block, so the runs are spelled out.
    if (cp >= 0x0100 && cp <= 0x0137) return (cp % 2 == 0) ? cp + 1 : cp;
    if (cp >= 0x0139 && cp <= 0x0148) return (cp % 2 == 1) ? cp + 1 : cp;
    if (cp >= 0x014A && cp <= 0x0177) return (cp % 2 == 0) ? cp + 1 : cp;
    if (cp == 0x0178) return 0xFF;  // Ÿ folds out of the block, to ÿ
    if (cp >= 0x0179 && cp <= 0x017E) return (cp % 2 == 1) ? cp + 1 : cp;
    return cp;
}

/// The documented confusable set, applied after case folding. Each entry is a
/// pair a person reading a file list cannot reliably tell apart, so treating
/// them as one key turns a silent overwrite into a rejected capsule. Folding
/// two distinct names together can only cause a false collision, which fails
/// closed; missing one causes a silent overwrite, which does not.
///
/// | Folded input | Representative |
/// |---|---|
/// | `o`, `0` | `o` |
/// | `i`, `l`, `1`, `ı` (U+0131, and U+0130 via case folding) | `i` |
/// | `ſ` (U+017F) | `s` |
/// | `ĸ` (U+0138) | `k` |
char32_t fold_confusable(char32_t cp) noexcept {
    switch (cp) {
        case U'0':
            return U'o';
        case U'l':
        case U'1':
        case 0x0131:
            return U'i';
        case 0x017F:
            return U's';
        case 0x0138:
            return U'k';
        default:
            return cp;
    }
}

// ── Component rules ─────────────────────────────────────────────────────────

/// Windows resolves these names to devices regardless of directory or
/// extension, so `aux.wav` is not a file there. Rejecting them keeps a capsule
/// extractable on every supported platform.
/// Windows resolves these names as devices no matter which directory they
/// appear in, so a member called `CON` or `COM1` is a path that opens a device
/// rather than a file. They are rejected everywhere, not only on Windows: a
/// capsule that cannot be extracted on one platform is a capsule that does not
/// travel, and finding that out on the recipient's machine is the wrong time.
///
/// Two forms are easy to miss. `CLOCK$`, `CONIN$` and `CONOUT$` are longer than
/// the four characters a naive length check allows. And Windows accepts the
/// SUPERSCRIPT digits — `COM¹`, `COM²`, `COM³` (U+00B9, U+00B2, U+00B3) — as
/// COM1, COM2 and COM3, which is three UTF-8 bytes wide and looks like an
/// ordinary name to a byte-oriented check. The NFC allowlist happens to reject
/// those code points today, but relying on that would make this protection
/// depend on a list maintained for an unrelated reason: widen the allowlist and
/// the device name walks through. So this owns it directly.
bool is_reserved_device_name(std::string_view component) noexcept {
    const std::size_t stem_end = component.find('.');
    std::string_view stem =
        component.substr(0, stem_end == std::string_view::npos ? component.size() : stem_end);
    if (stem.empty()) return false;

    // Fold the superscript digits down to their ASCII equivalents first, so the
    // comparisons below see one spelling of a device name rather than four.
    std::string lowered;
    lowered.reserve(stem.size());
    for (std::size_t i = 0; i < stem.size(); ++i) {
        const auto byte = static_cast<unsigned char>(stem[i]);
        if (byte == 0xC2 && i + 1 < stem.size()) {
            const auto next = static_cast<unsigned char>(stem[i + 1]);
            if (next == 0xB9) { lowered.push_back('1'); ++i; continue; }
            if (next == 0xB2) { lowered.push_back('2'); ++i; continue; }
            if (next == 0xB3) { lowered.push_back('3'); ++i; continue; }
        }
        lowered.push_back((byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte + 0x20)
                                                       : static_cast<char>(byte));
    }

    if (lowered == "con" || lowered == "prn" || lowered == "aux" || lowered == "nul") return true;
    if (lowered == "clock$" || lowered == "conin$" || lowered == "conout$") return true;
    if (lowered.size() == 4 && (lowered.compare(0, 3, "com") == 0 || lowered.compare(0, 3, "lpt") == 0))
        return lowered[3] >= '1' && lowered[3] <= '9';
    return false;
}

}  // namespace

runtime::Result<std::string, CapsuleError>
admit_member_path(std::string_view raw, const CapsuleLimits& limits) {
    if (raw.empty()) return reject(raw, kRuleNonEmpty);

    // Bound the work before decoding. The normalized form is byte-identical to
    // the input — nothing here rewrites bytes — so the raw length is the
    // normalized length the budget is written against.
    if (raw.size() > limits.max_path_bytes) return reject(raw, kRuleMaxBytes);

    // Prefix shapes first, so each gets its own precise rule instead of being
    // swallowed by the generic separator and component checks below.
    if (raw.size() >= 2 && (raw.compare(0, 2, "//") == 0 || raw.compare(0, 2, "\\\\") == 0)) {
        return reject(raw, kRuleUnc);
    }
    if (raw.size() >= 2 && raw[1] == ':' &&
        ((raw[0] >= 'A' && raw[0] <= 'Z') || (raw[0] >= 'a' && raw[0] <= 'z'))) {
        return reject(raw, kRuleDriveLetter);
    }
    if (raw.front() == '/') return reject(raw, kRuleRelative);
    if (raw.back() == '/') return reject(raw, kRuleTrailingSep);

    // Code-point pass: decode strictly, then admit only what is provably safe.
    for (std::size_t offset = 0; offset < raw.size();) {
        Decoded decoded{};
        if (!decode_utf8(raw, offset, decoded)) return reject(raw, kRuleUtf8);

        const char32_t cp = decoded.code_point;
        if (is_control(cp)) return reject(raw, kRuleControl);
        if (cp == U'\\') return reject(raw, kRuleBackslash);
        if (cp == U':') return reject(raw, kRuleColon);
        if (is_reserved_character(cp)) return reject(raw, kRuleReservedChar);
        if (!is_nfc_verifiable(cp)) return reject(raw, kRuleNfcSubset);

        offset += decoded.length;
    }

    // Component pass. Every component is checked, including the last, so a
    // traversal segment cannot hide behind a well-formed prefix.
    std::size_t depth = 0;
    std::size_t start = 0;
    while (true) {
        const std::size_t      slash     = raw.find('/', start);
        const std::string_view component = raw.substr(start, slash == std::string_view::npos
                                                                 ? std::string_view::npos
                                                                 : slash - start);

        if (component.empty()) return reject(raw, kRuleEmptyComponent);
        if (component == "." || component == "..") return reject(raw, kRuleDotComponent);
        // Windows silently strips a trailing dot or space, so `evil.` and
        // `evil` would land on the same file after extraction.
        if (component.back() == '.' || component.back() == ' ') return reject(raw, kRuleTrailingDotSp);
        if (is_reserved_device_name(component)) return reject(raw, kRuleReservedName);

        ++depth;
        if (depth > limits.max_path_depth) return reject(raw, kRuleMaxDepth);

        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }

    return Ok(std::string(raw));
}

std::string collision_key(std::string_view normalized_path) {
    std::string key;
    key.reserve(normalized_path.size());

    for (std::size_t offset = 0; offset < normalized_path.size();) {
        Decoded decoded{};
        if (!decode_utf8(normalized_path, offset, decoded)) {
            // The documented precondition is a path that already passed
            // admit_member_path, so this byte cannot occur in a real
            // capsule. Copying it through keeps the function total and keeps
            // distinct inputs distinct, rather than folding malformed bytes
            // onto one key.
            key.push_back(normalized_path[offset]);
            ++offset;
            continue;
        }
        encode_utf8(fold_confusable(fold_case(decoded.code_point)), key);
        offset += decoded.length;
    }
    return key;
}

runtime::Result<void, CapsuleError> check_collisions(const std::vector<std::string>& normalized) {
    // Ordered by first appearance so the reported pair is deterministic for a
    // given member order, which makes a rejection reproducible off the archive
    // alone.
    std::map<std::string, std::size_t> first_seen;

    for (std::size_t index = 0; index < normalized.size(); ++index) {
        auto [position, inserted] = first_seen.emplace(collision_key(normalized[index]), index);
        if (!inserted) {
            return Err(CapsuleError{CapsuleStatus::path_collision,
                                    normalized[position->second],
                                    std::string{},
                                    normalized[index]});
        }
    }
    return {};
}

}  // namespace pulp::authoring_capsule
