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
#include <set>
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

    // Single ASCII printable — KeyCode for letters/digits/punctuation
    // is the ASCII code itself (per input_events.hpp). Lowercase for
    // letters; everything else passes through as-is so `,`, `.`, `/`,
    // `?` etc. map correctly. Outside ASCII printable range → 0 (the
    // codegen layer treats 0 as unmapped and skips emission).
    if (key.size() == 1) {
        unsigned char c = static_cast<unsigned char>(key[0]);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        if (c >= 0x20 && c <= 0x7E) {  // printable ASCII (space..tilde)
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

// ── Default shortcuts (Phase A: source-matched) ─────────────────────────

namespace {

const char* pattern_name(DefaultShortcutPattern p) {
    switch (p) {
        case DefaultShortcutPattern::settings:   return "settings";
        case DefaultShortcutPattern::help:       return "help";
        case DefaultShortcutPattern::cheatsheet: return "cheatsheet";
        case DefaultShortcutPattern::new_file:   return "new";
        case DefaultShortcutPattern::open_file:  return "open";
        case DefaultShortcutPattern::save_file:  return "save";
        case DefaultShortcutPattern::find:       return "find";
    }
    return "unknown";
}

bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a))
                == std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

// Find all top-level JSX/identifier names that look like component
// declarations or usages — a lexical pass over the source, no parser.
// Captures `function FooBar(`, `const FooBar =`, `class FooBar`,
// and JSX opening tags `<FooBar`. Lowercased dedupe.
std::vector<std::string> collect_component_names(const std::string& source) {
    std::vector<std::string> names;
    auto seen = std::set<std::string>{};

    auto add = [&](const std::string& n) {
        if (n.empty() || !std::isupper(static_cast<unsigned char>(n[0]))) return;
        auto lower = n;
        for (auto& c : lower) c = std::tolower(static_cast<unsigned char>(c));
        if (seen.insert(lower).second) names.push_back(n);
    };

    // function FooBar( ... )
    {
        std::regex re(R"(\bfunction\s+([A-Z][A-Za-z0-9_]*)\s*\()");
        auto begin = std::sregex_iterator(source.begin(), source.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) add((*it)[1].str());
    }
    // const FooBar = ... / let FooBar = ...
    {
        std::regex re(R"(\b(?:const|let|var)\s+([A-Z][A-Za-z0-9_]*)\s*=)");
        auto begin = std::sregex_iterator(source.begin(), source.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) add((*it)[1].str());
    }
    // class FooBar ...
    {
        std::regex re(R"(\bclass\s+([A-Z][A-Za-z0-9_]*)\b)");
        auto begin = std::sregex_iterator(source.begin(), source.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) add((*it)[1].str());
    }
    // <FooBar  (JSX opening tag) — captures usages even if component is imported
    {
        std::regex re(R"(<([A-Z][A-Za-z0-9_]*))");
        auto begin = std::sregex_iterator(source.begin(), source.end(), re);
        auto end   = std::sregex_iterator();
        for (auto it = begin; it != end; ++it) add((*it)[1].str());
    }
    return names;
}

// Map a pattern to the keyword set the component name + ARIA + heading
// signals should match.  Order matters for cheatsheet vs help disambig:
// cheatsheet is checked first; if it matches AND <kbd> appears nearby,
// it's a cheatsheet; otherwise help/about/docs wins on prose modals.
const std::vector<std::string>& pattern_keywords(DefaultShortcutPattern p) {
    static const std::vector<std::string> kw_settings    = {"settings", "preferences", "preference", "options"};
    static const std::vector<std::string> kw_help        = {"help", "about", "documentation", "docs"};
    static const std::vector<std::string> kw_cheatsheet  = {"cheatsheet", "shortcuts", "shortcutshelp", "keyboardshortcuts", "keymap"};
    static const std::vector<std::string> kw_new         = {"newproject", "newfile", "newdocument", "newbutton"};
    static const std::vector<std::string> kw_open        = {"openfile", "openproject", "opendocument", "openbutton"};
    static const std::vector<std::string> kw_save        = {"savefile", "savebutton", "savedocument", "saveproject"};
    static const std::vector<std::string> kw_find        = {"searchinput", "searchbox", "findbox", "findinput"};
    switch (p) {
        case DefaultShortcutPattern::settings:   return kw_settings;
        case DefaultShortcutPattern::help:       return kw_help;
        case DefaultShortcutPattern::cheatsheet: return kw_cheatsheet;
        case DefaultShortcutPattern::new_file:   return kw_new;
        case DefaultShortcutPattern::open_file:  return kw_open;
        case DefaultShortcutPattern::save_file:  return kw_save;
        case DefaultShortcutPattern::find:       return kw_find;
    }
    return kw_settings;
}

// Score one candidate component against one default pattern. Returns the
// set of signals that fired; an empty result means "no match".
std::vector<std::string> score_signals(
    const std::string& source,
    const std::string& component,
    DefaultShortcutPattern pattern) {
    std::vector<std::string> signals;

    // Signal 1: component name contains a pattern keyword.
    const auto& kws = pattern_keywords(pattern);
    bool name_hit = false;
    for (const auto& kw : kws) {
        if (icontains(component, kw)) { name_hit = true; break; }
    }
    if (name_hit) signals.push_back("component-name:" + component);

    // Signal 1b (canonical-name bonus): exact matches against the canonical
    // shape `XYZ{Modal,Dialog,Panel,Popover,Sheet,Window,Drawer}` for the
    // pattern carry enough specificity to be a second signal on their own.
    // Real-world apps (Spectr's `SettingsModal`, `HelpPopover`) use
    // inline-styled divs without `role="dialog"`, so the strict ≥2 ARIA-
    // shape gate would skip every one of them. We only add this for
    // structurally-unambiguous names — not generic "Settings*" with
    // suffixes that could mean anything.
    auto canonical_name_hit = [&](const std::string& comp) -> bool {
        // Single-word root part (Settings, Preferences, Help, About,
        // Shortcuts, ...) followed by exactly one of the canonical kind
        // suffixes. Reject anything beyond that.
        std::vector<std::string> kinds = {
            "Modal", "Dialog", "Panel", "Popover", "Sheet", "Window", "Drawer"
        };
        for (const auto& kw : kws) {
            // Title-case the keyword for prefix matching: settings → Settings
            std::string root = kw;
            if (!root.empty()) root[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(root[0])));
            for (const auto& kind : kinds) {
                if (comp == root + kind) return true;
            }
            // Also accept the canonical name itself with no suffix
            // (HelpPopover is a kind already; same root). Reject single-
            // word "Settings" alone — too generic.
        }
        return false;
    };
    if (name_hit && canonical_name_hit(component)) {
        signals.push_back("canonical-name");
    }

    // The remaining signals look at the body of the component declaration.
    // Locate the definition, then truncate at the NEXT same-shape
    // declaration so we don't bleed sibling components into this one's
    // body (which would let `<kbd>` from a cheatsheet component count as
    // a signal for an unrelated settings modal that happens to live in
    // the same file).
    std::string body;
    {
        std::regex decl_re(R"((?:function\s+|const\s+|let\s+|var\s+|class\s+))" + component + R"(\b)");
        std::smatch m;
        if (std::regex_search(source, m, decl_re)) {
            size_t start = static_cast<size_t>(m.position(0));
            size_t end = std::min(source.size(), start + 4000);

            // Walk forward from start+1 (skip our own decl) and clamp `end`
            // at the next top-level component declaration.
            std::regex next_decl(
                R"((?:function|const|let|var|class)\s+[A-Z][A-Za-z0-9_]*\b)");
            auto search_from = source.begin() + start + 1;
            auto search_to = source.begin() + end;
            std::smatch next_m;
            if (std::regex_search(search_from, search_to, next_m, next_decl)) {
                end = static_cast<size_t>(start + 1 + next_m.position(0));
            }
            body = source.substr(start, end - start);
        }
    }

    if (!body.empty()) {
        // Signal 2: role="dialog" / role="alertdialog" / role="menu" in body.
        if (std::regex_search(body,
                std::regex(R"(role\s*=\s*['"](?:dialog|alertdialog|menu|listbox)['"])"))) {
            signals.push_back("aria-role:dialog");
        }

        // Signal 3: aria-label or aria-labelledby referencing a pattern keyword.
        for (const auto& kw : kws) {
            std::regex aria_re(R"(aria-label\s*=\s*['"][^'"]*)" + kw + R"([^'"]*['"])",
                               std::regex::icase);
            if (std::regex_search(body, aria_re)) {
                signals.push_back("aria-label:" + kw);
                break;
            }
        }

        // Signal 4: an <h1>/<h2>/<h3>/title text in body contains a pattern keyword.
        for (const auto& kw : kws) {
            std::regex h_re(R"(<h[1-3][^>]*>\s*[^<]*)" + kw + R"([^<]*</h[1-3]>)",
                            std::regex::icase);
            if (std::regex_search(body, h_re)) {
                signals.push_back("heading:" + kw);
                break;
            }
        }

        // Signal 5 (cheatsheet disambiguator): <kbd> tag presence in body.
        if (pattern == DefaultShortcutPattern::cheatsheet) {
            if (body.find("<kbd") != std::string::npos) {
                signals.push_back("kbd-tag-present");
            }
        }
    }

    return signals;
}

}  // namespace

DefaultShortcutScan detect_default_shortcuts(
    const std::string& source,
    const std::vector<DetectedShortcut>& existing_extracted) {
    DefaultShortcutScan scan;
    if (source.empty()) return scan;

    auto components = collect_component_names(source);

    auto patterns = std::vector<DefaultShortcutPattern>{
        DefaultShortcutPattern::settings,
        DefaultShortcutPattern::help,
        DefaultShortcutPattern::cheatsheet,
        DefaultShortcutPattern::new_file,
        DefaultShortcutPattern::open_file,
        DefaultShortcutPattern::save_file,
        DefaultShortcutPattern::find,
    };

    // Track which (key, mods) chords are already claimed by an extracted
    // shortcut.  Extracted always wins — we suppress same-chord defaults.
    auto extracted_claims = std::set<std::string>{};
    for (const auto& s : existing_extracted) {
        std::string sig = s.key;
        for (const auto& m : s.modifiers) sig += "|" + m;
        extracted_claims.insert(sig);
    }
    auto chord_for = [](DefaultShortcutPattern p) -> std::string {
        // Used only for the extracted-shortcut collision check, so always
        // use the macOS chord (extracted shortcuts in the source already
        // collapse cross-platform via the meta||ctrl idiom).
        switch (p) {
            case DefaultShortcutPattern::settings:   return ",|meta";
            case DefaultShortcutPattern::help:       return "?|meta";
            case DefaultShortcutPattern::cheatsheet: return "?";  // bare
            case DefaultShortcutPattern::new_file:   return "n|meta";
            case DefaultShortcutPattern::open_file:  return "o|meta";
            case DefaultShortcutPattern::save_file:  return "s|meta";
            case DefaultShortcutPattern::find:       return "f|meta";
        }
        return "";
    };

    for (auto p : patterns) {
        // Suppress this default if a same-chord extracted shortcut exists.
        if (extracted_claims.count(chord_for(p))) continue;

        struct Match { std::string component; std::vector<std::string> signals; };
        std::vector<Match> matches;

        for (const auto& comp : components) {
            auto signals = score_signals(source, comp, p);
            if (signals.size() >= 2) matches.push_back({comp, std::move(signals)});
        }

        // Cheatsheet vs help disambiguation: if a component matched as
        // cheatsheet BUT had no <kbd> signal, demote it (a "ShortcutsModal"
        // with prose only is really a help dialog).
        if (p == DefaultShortcutPattern::cheatsheet) {
            matches.erase(
                std::remove_if(matches.begin(), matches.end(), [](const Match& m) {
                    return std::find(m.signals.begin(), m.signals.end(),
                                     std::string("kbd-tag-present")) == m.signals.end();
                }),
                matches.end());
        }

        if (matches.empty()) continue;

        if (matches.size() == 1) {
            scan.accepted.push_back(DefaultShortcutCandidate{
                p, matches[0].component,
                matches[0].signals.size() >= 3 ? "high" : "medium",
                std::move(matches[0].signals)});
        } else {
            std::vector<std::string> candidate_names;
            for (auto& m : matches) candidate_names.push_back(m.component);
            scan.collisions.push_back(DefaultShortcutCollision{
                p, std::move(candidate_names),
                "multiple components match — no default bound"});
        }
    }

    return scan;
}

std::vector<DetectedShortcut> apply_default_shortcuts(
    const std::vector<DefaultShortcutCandidate>& accepted,
    TargetPlatform platform) {
    std::vector<DetectedShortcut> out;
    out.reserve(accepted.size());
    const bool mac = (platform == TargetPlatform::macos);

    for (const auto& c : accepted) {
        DetectedShortcut s;
        s.pattern = std::string("default:") + pattern_name(c.pattern) + " (" + c.target + ")";
        s.handler_excerpt = "auto-bound default: " + c.confidence;

        switch (c.pattern) {
            case DefaultShortcutPattern::settings:
                s.key = ",";  s.modifiers = mac ? std::vector<std::string>{"meta"} : std::vector<std::string>{"ctrl"};
                break;
            case DefaultShortcutPattern::help:
                if (mac) { s.key = "?"; s.modifiers = {"meta"}; }
                else     { s.key = "F1"; s.modifiers = {}; }
                break;
            case DefaultShortcutPattern::cheatsheet:
                // Bare `?` cross-platform. Focus-guard (#2120) suppresses
                // it while a TextEditor has focus, so it's safe.
                s.key = "?";  s.modifiers = {};
                break;
            case DefaultShortcutPattern::new_file:
                s.key = "n";  s.modifiers = mac ? std::vector<std::string>{"meta"} : std::vector<std::string>{"ctrl"};
                break;
            case DefaultShortcutPattern::open_file:
                s.key = "o";  s.modifiers = mac ? std::vector<std::string>{"meta"} : std::vector<std::string>{"ctrl"};
                break;
            case DefaultShortcutPattern::save_file:
                s.key = "s";  s.modifiers = mac ? std::vector<std::string>{"meta"} : std::vector<std::string>{"ctrl"};
                break;
            case DefaultShortcutPattern::find:
                s.key = "f";  s.modifiers = mac ? std::vector<std::string>{"meta"} : std::vector<std::string>{"ctrl"};
                break;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::string serialize_default_shortcut_scan(const DefaultShortcutScan& scan) {
    auto root = choc::value::createObject("");

    auto defaults = choc::value::createEmptyArray();
    for (const auto& c : scan.accepted) {
        auto obj = choc::value::createObject("");
        obj.addMember("pattern", std::string(pattern_name(c.pattern)));
        obj.addMember("target", c.target);
        obj.addMember("confidence", c.confidence);
        auto sigs = choc::value::createEmptyArray();
        for (const auto& s : c.signals) sigs.addArrayElement(s);
        obj.addMember("signals", sigs);
        defaults.addArrayElement(obj);
    }
    root.addMember("defaults", defaults);

    auto collisions = choc::value::createEmptyArray();
    for (const auto& col : scan.collisions) {
        auto obj = choc::value::createObject("");
        obj.addMember("pattern", std::string(pattern_name(col.pattern)));
        auto cands = choc::value::createEmptyArray();
        for (const auto& c : col.candidates) cands.addArrayElement(c);
        obj.addMember("candidates", cands);
        obj.addMember("reason", col.reason);
        collisions.addArrayElement(obj);
    }
    root.addMember("collisions", collisions);

    return choc::json::toString(root, /*useLineBreaks=*/true);
}


} // namespace pulp::view
