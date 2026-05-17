// design_import_shortcuts.cpp — keyboard-shortcut extraction +
// JSON serialization + key-string ↔ keycode mapping for the
// design-import pipeline.
//
// Extracted from design_import.cpp in the 2026-05 A3 refactor (first
// cut). Splits the keyboard-shortcut concern off the 4670-line
// design_import.cpp monolith. The full A3 split (claude_bundle.cpp,
// design_codegen.cpp, design_tokens.cpp) is tracked as the A3
// follow-up.
//
// Public API (declared in pulp/view/design_import.hpp):
//   * extract_keyboard_shortcuts(source, filename) → vector<DetectedShortcut>
//   * serialize_detected_shortcuts(...)            → JSON string
//   * key_string_to_keycode(key)                   → KeyCode int / 0
//   * modifier_strings_to_mask(mods)               → bitmask of kMod*
//
// File-local helpers (anonymous namespace): line_for_offset,
// collect_modifiers, extract_handler_excerpt.

#include <pulp/view/design_import.hpp>
#include <pulp/view/input_events.hpp>
#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <string>
#include <vector>

namespace pulp::view {

// ── Keyboard shortcut extraction (UX best-practice default) ─────────────

namespace {

// Compute 1-based line number for an offset into `source`. Used to surface
// source locations in the detected-shortcut manifest.
int line_for_offset(const std::string& source, size_t offset) {
    int line = 1;
    size_t scan_end = std::min(offset, source.size());
    for (size_t i = 0; i < scan_end; ++i) {
        if (source[i] == '\n') ++line;
    }
    return line;
}

// Walk a window around the matched key check and pull every modifier
// reference the surrounding boolean expression touches. `e.metaKey` or
// `event.metaKey` is "meta"; the `e.metaKey || e.ctrlKey` cross-platform
// idiom maps to a single "meta" entry (de-duped) because both flags fire
// the same Pulp shortcut on macOS vs other hosts. Window size chosen
// empirically — long enough to cover multi-line `&&`/`||` chains in
// React handler bodies, short enough to avoid bleeding into the next
// statement.
std::vector<std::string> collect_modifiers(const std::string& source, size_t key_offset) {
    // Scope the scan to the enclosing `if (...)` condition only — walking
    // a fixed character window backward picks up modifier checks from
    // sibling branches (`if (e.metaKey ...) ...; if (e.key === 'Escape')`)
    // and produces false-positive modifier sets. Walk left, tracking paren
    // depth, until we find the unbalanced `(` that opens this match's
    // enclosing condition (depth crosses to 1 going left from a balanced
    // expression). Bound the search with a generous backward window for
    // safety against multi-line conditions; never bleed past a `;` `}` or
    // `=>` at depth 0.
    constexpr size_t kMaxBack = 400;
    size_t back_start = key_offset > kMaxBack ? key_offset - kMaxBack : 0;

    size_t scope_start = key_offset;
    int depth = 0;
    for (size_t i = key_offset; i > back_start; --i) {
        char c = source[i - 1];
        if (c == ')') ++depth;
        else if (c == '(') {
            if (depth == 0) { scope_start = i - 1; break; }
            --depth;
        } else if (depth == 0 && (c == ';' || c == '{' || c == '}')) {
            // Crossed a statement boundary without finding an open `(` —
            // the match isn't inside an `if (...)` (could be a JSX prop
            // value or a bare expression). Fall back to a tight 40-char
            // window so multi-modifier inline conditions still resolve.
            scope_start = i;
            break;
        }
    }

    std::string ctx = source.substr(scope_start, key_offset - scope_start + 24);

    std::vector<std::string> mods;
    auto add = [&](const std::string& m) {
        for (const auto& existing : mods) {
            if (existing == m) return;
        }
        mods.push_back(m);
    };
    // Emit `meta` and `ctrl` separately (Codex P1 review on #2119). The
    // common cross-platform `e.metaKey || e.ctrlKey` idiom yields BOTH
    // entries; generate_pulp_js() detects that combination and emits two
    // registerShortcut calls (one per physical chord). When the source
    // author writes only `e.ctrlKey` (Win/Linux-only) or only `e.metaKey`
    // (macOS-only), we preserve that intent — earlier collapse turned
    // every Ctrl-only handler into a Cmd-only binding and dropped the
    // ctrlKey flag in the synthetic event.
    if (ctx.find(".metaKey") != std::string::npos) add("meta");
    if (ctx.find(".ctrlKey") != std::string::npos) add("ctrl");
    if (ctx.find(".altKey")  != std::string::npos) add("alt");
    if (ctx.find(".shiftKey") != std::string::npos) add("shift");
    return mods;
}

// Pull a short excerpt of the handler body following the key check, for
// the manifest reviewer. Stops at the next `}` or `;` after the key
// match, capped to ~80 chars. Newlines collapsed to spaces.
std::string extract_handler_excerpt(const std::string& source, size_t key_offset) {
    constexpr size_t kMax = 80;
    size_t scan_end = std::min(source.size(), key_offset + 240);
    std::string excerpt;
    bool past_paren = false;
    int depth = 0;
    for (size_t i = key_offset; i < scan_end && excerpt.size() < kMax; ++i) {
        char c = source[i];
        if (!past_paren) {
            if (c == ')') past_paren = true;
            continue;
        }
        if (c == '{') { ++depth; continue; }
        if (c == '}') { if (depth == 0) break; --depth; continue; }
        if (c == ';' && depth == 0) break;
        if (c == '\n' || c == '\r' || c == '\t') {
            if (!excerpt.empty() && excerpt.back() != ' ') excerpt += ' ';
        } else {
            excerpt += c;
        }
    }
    // Trim leading whitespace from excerpt.
    size_t first = excerpt.find_first_not_of(' ');
    if (first != std::string::npos) excerpt.erase(0, first);
    return excerpt;
}

} // namespace

std::vector<DetectedShortcut> extract_keyboard_shortcuts(
    const std::string& source, const std::string& filename) {
    std::vector<DetectedShortcut> out;
    if (source.empty()) return out;

    // Matches:
    //   e.key === 'X'    e.key === "X"    event.key === 'X'
    //   e.code === 'X'   event.code === "X"
    // Quotes balanced; key/code is any non-empty sequence of [A-Za-z0-9_+-/]
    // (covers `Escape`, `Enter`, `ArrowLeft`, `+`, `/`, `F1`, `s`, etc.).
    std::regex re(R"((\w+)\.(key|code)\s*===\s*(['"])([A-Za-z0-9_+\-/]+)\3)");

    auto begin = std::sregex_iterator(source.begin(), source.end(), re);
    auto end = std::sregex_iterator{};
    for (auto it = begin; it != end; ++it) {
        const auto& m = *it;
        DetectedShortcut s;
        s.key = m[4].str();
        s.modifiers = collect_modifiers(source, static_cast<size_t>(m.position()));
        s.pattern = m[1].str() + "." + m[2].str() + " === " + m[3].str() +
                    s.key + m[3].str();
        int line = line_for_offset(source, static_cast<size_t>(m.position()));
        s.source_location = filename.empty()
            ? (":" + std::to_string(line))
            : (filename + ":" + std::to_string(line));
        s.handler_excerpt = extract_handler_excerpt(
            source, static_cast<size_t>(m.position()));
        out.push_back(std::move(s));
    }

    // De-dupe identical (key, modifiers) pairs — multiple checks in the
    // same source for the same chord (e.g. nested branches) shouldn't
    // produce duplicate manifest entries. Keep first occurrence (lowest
    // source location), which is what stable sort preserves.
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        if (a.key != b.key) return a.key < b.key;
        if (a.modifiers != b.modifiers) return a.modifiers < b.modifiers;
        return a.source_location < b.source_location;
    });
    out.erase(std::unique(out.begin(), out.end(),
                          [](const auto& a, const auto& b) {
                              return a.key == b.key && a.modifiers == b.modifiers;
                          }),
              out.end());
    return out;
}

std::string serialize_detected_shortcuts(const std::vector<DetectedShortcut>& shortcuts) {
    auto root = choc::value::createObject("");
    auto arr = choc::value::createEmptyArray();
    for (const auto& s : shortcuts) {
        auto obj = choc::value::createObject("");
        obj.addMember("key", s.key);
        auto mods = choc::value::createEmptyArray();
        for (const auto& m : s.modifiers) mods.addArrayElement(m);
        obj.addMember("modifiers", mods);
        obj.addMember("pattern", s.pattern);
        obj.addMember("source_location", s.source_location);
        obj.addMember("handler_excerpt", s.handler_excerpt);
        arr.addArrayElement(obj);
    }
    root.addMember("shortcuts", arr);
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

int key_string_to_keycode(const std::string& key) {
    if (key.empty()) return 0;

    // Single ASCII printable — KeyCode for letters/digits is the ASCII
    // code itself (per input_events.hpp). Lowercase for letters; digits
    // map to KeyCode::num0..num9 which are also their ASCII codes.
    if (key.size() == 1) {
        char c = key[0];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ') {
            return static_cast<int>(c);
        }
    }

    // KeyboardEvent.code letter / digit forms (Codex P2 on #2119). The
    // extractor pulls `event.code === 'KeyS'` and `event.code === 'Digit1'`
    // patterns; before this they fell through to 0 and the codegen loop
    // skipped the whole entry as "unmapped". Map them to the same ASCII
    // codes single-char keys produce, so downstream code (registerShortcut
    // mask + synthetic event payload) treats both forms identically.
    if (key.size() == 4 && (key[0] == 'K' || key[0] == 'k') &&
        (key[1] == 'e' || key[1] == 'E') && (key[2] == 'y' || key[2] == 'Y')) {
        char c = key[3];
        if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A' + 'a');
        if (c >= 'a' && c <= 'z') return static_cast<int>(c);
    }
    if (key.size() == 6 && (key.compare(0, 5, "Digit") == 0 ||
                            key.compare(0, 5, "digit") == 0)) {
        char c = key[5];
        if (c >= '0' && c <= '9') return static_cast<int>(c);
    }

    // Multi-char named keys. W3C uses both `key` ("ArrowLeft") and `code`
    // ("ArrowLeft") variants — we accept either since the extractor pulls
    // whatever the source author wrote.
    static const std::map<std::string, KeyCode> table = {
        {"escape",    KeyCode::escape},
        {"esc",       KeyCode::escape},
        {"enter",     KeyCode::enter},
        {"return",    KeyCode::enter},
        {"tab",       KeyCode::tab},
        {"backspace", KeyCode::backspace},
        {"delete",    KeyCode::delete_},
        {"del",       KeyCode::delete_},
        {"space",     static_cast<KeyCode>(' ')},
        {"spacebar",  static_cast<KeyCode>(' ')},
        {"arrowleft",  KeyCode::left},
        {"arrowright", KeyCode::right},
        {"arrowup",    KeyCode::up},
        {"arrowdown",  KeyCode::down},
        {"left",      KeyCode::left},
        {"right",     KeyCode::right},
        {"up",        KeyCode::up},
        {"down",      KeyCode::down},
        {"home",      KeyCode::home},
        {"end",       KeyCode::end_},
        {"pageup",    KeyCode::page_up},
        {"pagedown",  KeyCode::page_down},
        {"f1", KeyCode::f1}, {"f2", KeyCode::f2}, {"f3", KeyCode::f3},
        {"f4", KeyCode::f4}, {"f5", KeyCode::f5}, {"f6", KeyCode::f6},
        {"f7", KeyCode::f7}, {"f8", KeyCode::f8}, {"f9", KeyCode::f9},
        {"f10", KeyCode::f10}, {"f11", KeyCode::f11}, {"f12", KeyCode::f12},
    };
    std::string lower = key;
    for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto it = table.find(lower);
    if (it == table.end()) return 0;
    return static_cast<int>(it->second);
}

int modifier_strings_to_mask(const std::vector<std::string>& mods) {
    int mask = 0;
    for (const auto& m : mods) {
        if (m == "shift") mask |= kModShift;
        else if (m == "ctrl") mask |= kModCtrl;
        else if (m == "alt")  mask |= kModAlt;
        // The extractor's "meta" already collapses `metaKey || ctrlKey`
        // (cross-platform idiom). Map to kModCmd — the platform-primary
        // modifier — so Cmd on macOS and Ctrl on other platforms both
        // resolve through the same shortcut entry.
        else if (m == "meta") mask |= kModCmd;
    }
    return mask;
}

} // namespace pulp::view
