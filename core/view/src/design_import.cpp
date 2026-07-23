#include <pulp/view/design_import.hpp>
#include "design_import_internal.hpp"
#include <pulp/view/anchor_strategy.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/zip.hpp>
#include <pulp/view/view.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include <pulp/state/store.hpp>
#include <choc/text/choc_JSON.h>
#include <choc/text/choc_StringUtilities.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <limits>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <regex>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulp::view {
namespace fs = std::filesystem;

// ── Design source helpers ───────────────────────────────────────────────

std::optional<DesignSource> parse_design_source(const std::string& name) {
    if (name == "figma")    return DesignSource::figma;
    if (name == "stitch")   return DesignSource::stitch;
    if (name == "v0")       return DesignSource::v0;
    if (name == "pencil")   return DesignSource::pencil;
    if (name == "claude")   return DesignSource::claude;
    if (name == "designmd") return DesignSource::designmd;
    if (name == "jsx")      return DesignSource::jsx;
    if (name == "figma-plugin") return DesignSource::figma_plugin;
    return std::nullopt;
}

const char* design_source_name(DesignSource source) {
    switch (source) {
        case DesignSource::figma:    return "Figma";
        case DesignSource::stitch:   return "Stitch";
        case DesignSource::v0:       return "v0";
        case DesignSource::pencil:   return "Pencil";
        case DesignSource::claude:   return "Claude Design";
        case DesignSource::designmd: return "DESIGN.md";
        case DesignSource::jsx:          return "JSX instrument";
        case DesignSource::figma_plugin: return "Figma plugin export";
    }
    return "unknown";
}

const IRAssetRef* IRAssetManifest::resolve(std::string_view asset_id) const {
    if (asset_id.empty()) return nullptr;
    for (const auto& asset : assets) {
        if (asset.asset_id == asset_id) return &asset;
    }
    return nullptr;
}

/// Motion provenance vendor key. Matches the `source` token the import CLI
/// accepts, lowercased + slash-friendly so the resulting `source_id`
/// (e.g. "figma:LevelMeter/Panel") is easy to grep through fixtures. Stable
/// across releases — fixtures depend on these strings.
const char* design_source_vendor_key(DesignSource source) {
    switch (source) {
        case DesignSource::figma:    return "figma";
        case DesignSource::stitch:   return "stitch";
        case DesignSource::v0:       return "v0";
        case DesignSource::pencil:   return "pencil";
        case DesignSource::claude:   return "claude";
        case DesignSource::designmd: return "designmd";
        case DesignSource::jsx:      return "jsx";
        case DesignSource::figma_plugin: return "figma-plugin";
    }
    return "unknown";
}

WidgetPromotionSignal classify_interactive_signal(const IRNode& node) {
    if (node.type != "frame") {
        return WidgetPromotionSignal::none;
    }

    if (node.attributes.count("onclick") || node.attributes.count("onClick")) {
        return WidgetPromotionSignal::onclick_attribute;
    }

    if (auto it = node.attributes.find("role");
        it != node.attributes.end() && it->second == "button") {
        return WidgetPromotionSignal::aria_role_button;
    }

    if (node.style.cursor && *node.style.cursor == "pointer") {
        if (auto it = node.attributes.find("role");
            it != node.attributes.end() && it->second == "presentation") {
            return WidgetPromotionSignal::none;
        }
        return WidgetPromotionSignal::cursor_pointer;
    }

    return WidgetPromotionSignal::none;
}

std::size_t promote_interactive_frames(IRNode& root) {
    std::size_t promoted = 0;
    std::vector<IRNode*> worklist;
    worklist.push_back(&root);

    while (!worklist.empty()) {
        IRNode* node = worklist.back();
        worklist.pop_back();

        if (classify_interactive_signal(*node) != WidgetPromotionSignal::none) {
            node->type = "button";
            ++promoted;
            continue;
        }

        for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
            worklist.push_back(&*it);
        }
    }

    return promoted;
}

DesignIR parse_claude_html(const std::string& html) {
    // Claude Design exports the same HTML+CSS shape as other web tools,
    // so parse with the existing Stitch HTML pipeline and re-tag the
    // source. Users hand the exported HTML over directly — no Anthropic API
    // integration.
    auto ir = parse_stitch_html(html);
    ir.source = DesignSource::claude;
    ir.source_adapter = "claude-design-html";
    // Re-tag provenance after the Stitch parser stamped it. Anchors were
    // already assigned by parse_stitch_html with the content-hash strategy —
    // same strategy claude-design-html uses (see DEFAULT_ANCHOR_STRATEGY in
    // anchors.ts), so we don't reassign.
    // parse_stitch_html always populates provenance on both its JSON and
    // regex-fallback paths, so the optional is always set here.
    if (ir.root.provenance) ir.root.provenance->adapter = "claude-design-html";
    return ir;
}

// ── Claude Design classname extraction ───────────────────────────────────
//
// Mirrors Spectr's `tools/extract-html-bundle/extract.mjs` classname
// pass: pull every `<style>...</style>` block, parse the CSS rules, and
// emit `classname → { cssProp(camelCase): cssValue, ... }`. The
// `@pulp/css-adapt` layer downstream consumes this map to merge
// class-based styles into inline before forwarding to bridge calls.

namespace {

// Convert a CSS hyphen-cased property name (`font-family`) to a
// JS-friendly camelCase key (`fontFamily`). Mirrors Spectr's
// `parseDeclarationsToCamelCase` so the artifacts stay byte-compatible.
// Pure string-ops — no allocation beyond the result.
std::string css_prop_to_camel_case(const std::string& prop) {
    std::string out;
    out.reserve(prop.size());
    bool upper_next = false;
    for (char c : prop) {
        if (c == '-') {
            upper_next = true;
        } else if (upper_next) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upper_next = false;
        } else {
            out += c;
        }
    }
    return out;
}

// Strip CSS `/* ... */` comments from a block. Multi-line safe. This
// runs before the rule walker so brace counting there never has to
// reason about braces that live inside a comment.
std::string strip_css_comments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            auto end = css.find("*/", i + 2);
            if (end == std::string::npos) break;  // unterminated — drop rest
            i = end + 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

// Parse a CSS declaration block body (the text between `{` and `}`)
// into camelCase prop → value pairs. Splits on `;`, then on the first
// `:` per declaration. Skips empty declarations and bare colons.
std::map<std::string, std::string> parse_css_declarations(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        auto semi = body.find(';', i);
        std::string decl = body.substr(i, (semi == std::string::npos ? body.size() : semi) - i);
        i = (semi == std::string::npos) ? body.size() : semi + 1;
        auto colon = decl.find(':');
        if (colon == std::string::npos) continue;
        auto prop = std::string(choc::text::trim(std::string_view(decl).substr(0, colon)));
        auto value = std::string(choc::text::trim(std::string_view(decl).substr(colon + 1)));
        if (prop.empty() || value.empty()) continue;
        out[css_prop_to_camel_case(prop)] = value;
    }
    return out;
}

// Advance past a CSS string literal. `quote_at` indexes the opening
// `'` or `"`; returns the index just past the closing quote, or
// `css.size()` if the literal is unterminated. Backslash escapes are
// honored so `content: "\""` doesn't end the literal early.
size_t skip_css_string(const std::string& css, size_t quote_at) {
    const char quote = css[quote_at];
    for (size_t i = quote_at + 1; i < css.size(); ++i) {
        if (css[i] == '\\') {
            ++i;  // consume the escaped char
            continue;
        }
        if (css[i] == quote) return i + 1;
    }
    return css.size();
}

// Find the `}` that closes the block opening at `open`, counting
// nested braces so an at-rule body (`@media { .a {} .b {} }`) is
// consumed whole rather than stopping at its first inner `}`. Braces
// inside string literals don't count. Returns npos when unbalanced.
size_t find_matching_brace(const std::string& css, size_t open) {
    int depth = 0;
    for (size_t i = open; i < css.size(); ++i) {
        const char c = css[i];
        if (c == '"' || c == '\'') {
            i = skip_css_string(css, i) - 1;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// Walk a CSS source string (the inside of one `<style>` block) and
// merge every top-level `.classname { ... }` rule into `into`. Skips
// at-rules (anything that begins with `@`), `:root` blocks, descendant
// / pseudo-class selectors (anything with whitespace, `:`, `>` or `,`
// between the dot and the `{`), and `.scheme-*` selectors (those are
// already handled as theme-mode token overrides upstream).
//
// Rules nested inside an at-rule are deliberately NOT collected: a
// `@media` / `@supports` body is conditional, so hoisting its rules
// into the unconditional classname map would make a responsive or
// dark-mode override apply always. Consuming each at-rule body whole
// (balanced braces) is what keeps the walker in sync — stopping at the
// first inner `}` would both leak the at-rule's later rules and
// misparse the following top-level rule's selector.
//
// We hand-walk character-by-character rather than regex because:
//   1. Brace nesting is unbounded (`@media { @supports { … } }`), which
//      a regex cannot match.
//   2. The selector list can include commas, so we need to split on
//      `,` and apply the same body to every classname in the list.
void collect_classnames_from_css(const std::string& css_in,
                                 ClaudeClassNameRules& into) {
    auto css = strip_css_comments(css_in);
    size_t i = 0;
    while (i < css.size()) {
        // Skip whitespace.
        while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= css.size()) break;

        // Scan the prelude for whichever comes first: the `{` that
        // opens a body, or the `;` that terminates a body-less
        // statement at-rule (`@import url(…);`, `@charset "utf-8";`).
        // Quoted spans are stepped over so a `{` or `;` inside a string
        // doesn't split the prelude.
        size_t scan = i;
        size_t open = std::string::npos;
        bool statement_at_rule = false;
        while (scan < css.size()) {
            const char c = css[scan];
            if (c == '"' || c == '\'') {
                scan = skip_css_string(css, scan);
                continue;
            }
            if (c == '{') { open = scan; break; }
            if (c == ';') { statement_at_rule = true; break; }
            ++scan;
        }

        // A body-less statement — consume it and move on. Without this
        // the `;` would be swallowed into the *next* rule's selector
        // list, disqualifying an otherwise plain classname rule.
        if (statement_at_rule) {
            i = scan + 1;
            continue;
        }
        if (open == std::string::npos) break;
        std::string selector_list = css.substr(i, open - i);

        // Find the matching `}`, counting nested braces.
        auto close = find_matching_brace(css, open);
        if (close == std::string::npos) break;
        std::string body = css.substr(open + 1, close - (open + 1));
        i = close + 1;

        // Skip at-rules (`@media`, `@font-face`, `@keyframes`, etc.).
        // The first non-whitespace char of the selector tells us. The
        // whole balanced body is already consumed above, so nested
        // at-rules are skipped along with it.
        auto first_non_ws = selector_list.find_first_not_of(" \t\r\n");
        if (first_non_ws == std::string::npos) continue;
        if (selector_list[first_non_ws] == '@') continue;

        // Split selector_list on top-level commas.
        std::vector<std::string> selectors;
        size_t s_start = 0;
        for (size_t k = 0; k <= selector_list.size(); ++k) {
            if (k == selector_list.size() || selector_list[k] == ',') {
                selectors.push_back(std::string(choc::text::trim(
                    std::string_view(selector_list).substr(s_start, k - s_start))));
                s_start = k + 1;
            }
        }

        // Parse the body once — every matching selector references the
        // same map.
        std::optional<std::map<std::string, std::string>> decls;

        for (auto& sel : selectors) {
            // Only accept simple `.classname` selectors. The classname
            // grammar matches Spectr's regex: `[a-zA-Z][a-zA-Z0-9_-]*`.
            // A trailing chained selector (`.foo .bar`, `.foo > .bar`,
            // `.foo:hover`, `.foo[data-x]`) means this isn't a plain
            // classname rule — skip it.
            if (sel.empty() || sel[0] != '.') continue;
            std::string name;
            size_t k = 1;
            if (k >= sel.size() || !(std::isalpha(static_cast<unsigned char>(sel[k])) || sel[k] == '_'))
                continue;
            while (k < sel.size() &&
                   (std::isalnum(static_cast<unsigned char>(sel[k])) ||
                    sel[k] == '_' || sel[k] == '-')) {
                name += sel[k++];
            }
            // Anything left over → not a plain classname selector.
            if (k != sel.size()) continue;
            if (name.empty()) continue;
            // Theme-scope selectors are handled upstream as token
            // overrides, not classname rules.
            if (name.rfind("scheme-", 0) == 0) continue;

            if (!decls) decls = parse_css_declarations(body);
            if (decls->empty()) continue;

            // Cascade: later blocks override earlier ones for the same
            // classname. Per-prop merge keeps unrelated declarations
            // from being lost when two blocks define the same class.
            auto& existing = into[name];
            for (auto& [prop, val] : *decls) {
                existing[prop] = val;
            }
        }
    }
}

// Pull every `<style>...</style>` block out of an HTML string, in
// document order. Skips `<style>` blocks whose first 200 chars contain
// `font-face` (matches Spectr's filter — those carry only `@font-face`
// rules, no classnames). Returns the inner CSS bodies.
std::vector<std::string> extract_html_style_blocks(const std::string& html) {
    std::vector<std::string> blocks;
    static const std::regex style_re(
        R"RX(<style\b[^>]*>([\s\S]*?)</style>)RX",
        std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), style_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string body = (*it)[1].str();
        // Spectr's filter: skip blocks whose head looks like @font-face.
        // The head check (first 200 chars) keeps a normal classname
        // block that happens to mention `font-face` later from being
        // dropped.
        std::string head = body.substr(0, std::min<size_t>(body.size(), 200));
        std::transform(head.begin(), head.end(), head.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head.find("font-face") != std::string::npos) continue;
        blocks.push_back(std::move(body));
    }
    return blocks;
}

std::optional<std::string> extract_bundler_template_html(const std::string& html) {
    const std::string opener_dq = "<script type=\"__bundler/template\"";
    const std::string opener_sq = "<script type='__bundler/template'";
    size_t tag_start = html.find(opener_dq);
    size_t header_len = opener_dq.size();
    if (tag_start == std::string::npos) {
        tag_start = html.find(opener_sq);
        header_len = opener_sq.size();
        if (tag_start == std::string::npos) return std::nullopt;
    }

    const size_t open_end = html.find('>', tag_start + header_len);
    if (open_end == std::string::npos) return std::nullopt;

    const size_t close = html.find("</script>", open_end + 1);
    if (close == std::string::npos) return std::nullopt;

    try {
        auto value = choc::json::parseValue(html.substr(open_end + 1, close - (open_end + 1)));
        if (!value.isString()) return std::nullopt;
        return std::string(value.getString());
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

ClaudeClassNameRules extract_claude_classnames(const std::string& html) {
    ClaudeClassNameRules rules;

    // Walk the raw HTML's <style> blocks first. For non-bundled
    // exports (the `--no-bundle` flow, or hand-written test fixtures)
    // this is the only source.
    for (auto& css : extract_html_style_blocks(html)) {
        collect_classnames_from_css(css, rules);
    }

    // For self-bundled Claude Design exports, the actual app CSS lives inside
    // the JSON-encoded `<script type="__bundler/template">` payload. Decode
    // only that template here so the static importer stays linkable from the
    // core view library without pulling in the runtime-import JS harness.
    if (auto template_html = extract_bundler_template_html(html)) {
        for (auto& css : extract_html_style_blocks(*template_html)) {
            collect_classnames_from_css(css, rules);
        }
    }

    return rules;
}

bool looks_like_bundler_entry(const std::string& html) {
    if (html.empty()) return false;

    auto contains = [&](const char* needle) {
        return html.find(needle) != std::string::npos;
    };

    // Standard mount points (React, Vue, Svelte, @pulp/react).
    const bool has_mount_root =
        contains("id=\"root\"")        || contains("id='root'") ||
        contains("id=\"app\"")         || contains("id='app'")  ||
        contains("id=\"__pulp_root\"") || contains("id='__pulp_root'");

    // Script tags that pull in a bundled JS entry. We don't try to
    // identify whether the script *is* a bundle — just that the page
    // is structured to load one.
    const bool has_script_src =
        contains("<script src=")                  ||
        contains("<script type=\"module\" src=")  ||
        contains("import(\"./")                   || contains("import('./");

    // Bundler-emitted markers (`__bundler_*`, "Unpacking..." status, the
    // @pulp/react runtime, React dev-tools hooks). These rarely show up
    // in hand-authored Claude Design HTML, so a single hit is enough.
    const bool has_bundler_hint =
        contains("__bundler")        || contains("Unpacking")     ||
        contains("data-reactroot")   || contains("@pulp/react");

    // Either (mount + script) — vanilla shell — or any unambiguous
    // bundler-specific marker.
    return (has_mount_root && has_script_src) || has_bundler_hint;
}

std::string serialize_claude_classnames(const ClaudeClassNameRules& rules) {
    // Use choc::value::createObject for stable, well-escaped JSON. The
    // outer map is a std::map so keys arrive in alphabetical order
    // already; per-class declaration maps are also std::map for the
    // same property-order guarantee. choc::json::toString preserves
    // insertion order, so the resulting JSON is deterministic.
    auto root = choc::value::createObject("");
    for (const auto& [name, decls] : rules) {
        auto obj = choc::value::createObject("");
        for (const auto& [prop, val] : decls) {
            obj.addMember(prop, val);
        }
        root.addMember(name, obj);
    }
    // Pretty-print with line breaks so the artifact is human-readable
    // and diff-friendly (matches Spectr's `JSON.stringify(_, null, 2)`
    // output shape for parity with the existing tooling).
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

// Keyboard-shortcut extraction (extract_keyboard_shortcuts,
// serialize_detected_shortcuts, key_string_to_keycode,
// modifier_strings_to_mask) moved to design_import_shortcuts.cpp.



// ── Audio widget detection ──────────────────────────────────────────────

// Split a layer name into lowercase WORD tokens. Boundaries: any non-alnum
// char, camelCase (lower→Upper), acronym→Word (Upper→Upper-then-lower), and
// letter↔digit. So "GainKnob"→{gain,knob}, "VUMeter"→{vu,meter},
// "FilterXYPad"→{filter,xy,pad}, "knob_01"→{knob,01} — but "Dialog"→{dialog}
// and "Parameter"→{parameter} (NOT split into dial/meter). Word-boundary
// matching is what kills the substring false-positives below.
static std::vector<std::string> tokenize_name(const std::string& name) {
    std::vector<std::string> tokens;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) { tokens.push_back(cur); cur.clear(); } };
    const std::size_t n = name.size();
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c)) { flush(); continue; }
        if (!cur.empty()) {
            const unsigned char p = static_cast<unsigned char>(name[i - 1]);
            bool boundary = false;
            if (std::islower(p) && std::isupper(c)) boundary = true;            // aB → a|B
            else if (std::isupper(p) && std::isupper(c) && i + 1 < n
                     && std::islower(static_cast<unsigned char>(name[i + 1])))
                boundary = true;                                               // ABc → A|Bc (acronym→Word)
            else if ((std::isdigit(p) != 0) != (std::isdigit(c) != 0))
                boundary = true;                                               // a1 / 1a
            if (boundary) flush();
        }
        cur += static_cast<char>(std::tolower(c));
    }
    flush();
    return tokens;
}

// Recognize audio widgets by WHOLE-WORD name tokens, not substrings. The old
// substring match promoted any name *containing* "dial"/"meter"/… — so
// "Dialog"/"Radial"/"Medallion" became knobs and "Parameter"/"diameter" became
// meters. Token matching (pulp design-import gap survey) fixes those while
// keeping "xy_pad"/"xypad" and acronym names like "VUMeter".
AudioWidgetType detect_audio_widget(const std::string& name) {
    const auto toks = tokenize_name(name);
    const std::unordered_set<std::string> t(toks.begin(), toks.end());
    // Whole-token match, tolerant of a simple English plural ("Knobs"/"Faders"
    // → knob/fader) which the old substring match also caught — but never a
    // substring ("Dialog"/"Parameter" stay unmatched).
    auto has = [&](const std::string& w) {
        return t.find(w) != t.end() || t.find(w + "s") != t.end();
    };
    // A "label" token names the TEXT that annotates a control, never the control
    // itself: "sound / knob label", "fader label", "value label" are all text.
    // Without this the "knob" in "knob label" promoted the label frame to a
    // built-in knob, painting a stock knob disc over a text label ("Classic"
    // under a filter-mode picker rendered as a dark knob instead of the word).
    if (has("label"))                                          return AudioWidgetType::none;
    if (has("knob") || has("dial"))                            return AudioWidgetType::knob;
    if (has("fader") || has("slider"))                         return AudioWidgetType::fader;
    if (has("meter") || has("level") || has("vu"))             return AudioWidgetType::meter;
    if (has("xypad") || (has("xy") && has("pad")))             return AudioWidgetType::xy_pad;
    if (has("waveform") || has("oscilloscope"))                return AudioWidgetType::waveform;
    if (has("spectrum") || has("analyzer") || has("analyser")) return AudioWidgetType::spectrum;
    return AudioWidgetType::none;
}

AudioWidgetType audio_widget_from_id(const std::string& id) {
    const auto lower = choc::text::toLowerCase(id);
    if (lower == "knob") return AudioWidgetType::knob;
    if (lower == "fader") return AudioWidgetType::fader;
    if (lower == "meter") return AudioWidgetType::meter;
    if (lower == "xy_pad" || lower == "xypad" || lower == "xy-pad") return AudioWidgetType::xy_pad;
    if (lower == "waveform") return AudioWidgetType::waveform;
    if (lower == "spectrum") return AudioWidgetType::spectrum;
    return AudioWidgetType::none;
}

static std::string strip_js_comments_and_literals(std::string_view source) {
    enum class State {
        normal,
        line_comment,
        block_comment,
        single_quote,
        double_quote,
        template_literal
    };

    struct TemplateExpression {
        int brace_depth = 0;
    };

    std::string out;
    out.reserve(source.size());
    State state = State::normal;
    bool escaped = false;
    std::vector<TemplateExpression> template_expressions;

    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        const char next = (i + 1 < source.size()) ? source[i + 1] : '\0';

        switch (state) {
            case State::normal:
                if (!template_expressions.empty() && c == '}') {
                    out.push_back(' ');
                    auto& expr = template_expressions.back();
                    --expr.brace_depth;
                    if (expr.brace_depth == 0) {
                        template_expressions.pop_back();
                        state = State::template_literal;
                        escaped = false;
                    }
                } else if (!template_expressions.empty() && c == '{') {
                    out.push_back(c);
                    ++template_expressions.back().brace_depth;
                } else if (c == '/' && next == '/') {
                    out.push_back(' ');
                    out.push_back(' ');
                    ++i;
                    state = State::line_comment;
                } else if (c == '/' && next == '*') {
                    out.push_back(' ');
                    out.push_back(' ');
                    ++i;
                    state = State::block_comment;
                } else if (c == '\'') {
                    out.push_back(' ');
                    state = State::single_quote;
                    escaped = false;
                } else if (c == '"') {
                    out.push_back(' ');
                    state = State::double_quote;
                    escaped = false;
                } else if (c == '`') {
                    out.push_back(' ');
                    state = State::template_literal;
                    escaped = false;
                } else {
                    out.push_back(c);
                }
                break;

            case State::line_comment:
                if (c == '\n' || c == '\r') {
                    out.push_back(c);
                    state = State::normal;
                } else {
                    out.push_back(' ');
                }
                break;

            case State::block_comment:
                if (c == '*' && next == '/') {
                    out.push_back(' ');
                    out.push_back(' ');
                    ++i;
                    state = State::normal;
                } else {
                    out.push_back((c == '\n' || c == '\r') ? c : ' ');
                }
                break;

            case State::single_quote:
                out.push_back((c == '\n' || c == '\r') ? c : ' ');
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '\'') {
                    state = State::normal;
                }
                break;

            case State::double_quote:
                out.push_back((c == '\n' || c == '\r') ? c : ' ');
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    state = State::normal;
                }
                break;

            case State::template_literal:
                if (escaped) {
                    out.push_back((c == '\n' || c == '\r') ? c : ' ');
                    escaped = false;
                } else if (c == '$' && next == '{') {
                    out.push_back(' ');
                    out.push_back(' ');
                    ++i;
                    template_expressions.push_back({1});
                    state = State::normal;
                } else if (c == '\\') {
                    out.push_back(' ');
                    escaped = true;
                } else if (c == '`') {
                    out.push_back(' ');
                    state = State::normal;
                } else {
                    out.push_back((c == '\n' || c == '\r') ? c : ' ');
                }
                break;
        }
    }

    return out;
}

static bool regex_present(const std::string& source, const char* pattern) {
    return std::regex_search(source, std::regex(pattern));
}

SnapshotDynamicApiScan detect_jsx_snapshot_dynamic_apis(std::string_view source) {
    const auto searchable = strip_js_comments_and_literals(source);
    SnapshotDynamicApiScan scan;
    auto add_if_present = [&](const char* pattern, std::string label) {
        if (!regex_present(searchable, pattern)) return;
        if (std::find(scan.tokens.begin(), scan.tokens.end(), label) == scan.tokens.end())
            scan.tokens.push_back(std::move(label));
    };

    add_if_present(R"(\bsetInterval\s*\()", "setInterval");
    add_if_present(R"(\bsetTimeout\s*\()", "setTimeout");
    add_if_present(R"(\brequestAnimationFrame\s*\()", "requestAnimationFrame");
    add_if_present(R"(\bDate\s*\.\s*now\s*\()", "Date.now");
    add_if_present(R"(\bnew\s+Date\s*\()", "new Date");
    add_if_present(R"(\bperformance\s*\.\s*now\s*\()", "performance.now");
    add_if_present(R"(\bMath\s*\.\s*random\s*\()", "Math.random");
    add_if_present(R"(\bfetch\s*\()", "fetch");
    return scan;
}

static bool has_prefix(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size()
        && value.substr(0, prefix.size()) == prefix;
}

static bool is_network_url(std::string_view value) {
    return has_prefix(value, "https://") || has_prefix(value, "http://");
}

static bool is_data_uri(std::string_view value) {
    return has_prefix(value, "data:");
}

static std::string normalize_url_path(std::string_view path) {
    const bool absolute = !path.empty() && path.front() == '/';
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        auto next = path.find('/', pos);
        auto segment = path.substr(pos, next == std::string_view::npos ? path.size() - pos : next - pos);
        if (segment.empty() || segment == ".") {
            // skip
        } else if (segment == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.emplace_back(segment);
        }
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }

    std::ostringstream out;
    if (absolute) out << '/';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out << '/';
        out << parts[i];
    }
    if (path.size() > 1 && path.back() == '/' && (parts.empty() || out.str().back() != '/'))
        out << '/';
    auto normalized = out.str();
    if (normalized.empty()) return absolute ? "/" : "";
    return normalized;
}

static std::string resolve_url_reference(std::string_view base_url,
                                         std::string_view reference) {
    if (reference.empty() || is_network_url(reference) || is_data_uri(reference)
        || has_prefix(reference, "file://")) {
        return std::string(reference);
    }
    if (!is_network_url(base_url)) return std::string(reference);

    const auto scheme_sep = base_url.find("://");
    if (scheme_sep == std::string_view::npos) return std::string(reference);
    const auto authority_start = scheme_sep + 3;
    const auto path_start = base_url.find('/', authority_start);
    const auto origin = path_start == std::string_view::npos
        ? std::string(base_url)
        : std::string(base_url.substr(0, path_start));
    const auto scheme = std::string(base_url.substr(0, scheme_sep));
    if (has_prefix(reference, "//")) return scheme + ":" + std::string(reference);

    std::string base_path = path_start == std::string_view::npos
        ? "/"
        : std::string(base_url.substr(path_start));
    if (auto hash = base_path.find('#'); hash != std::string::npos)
        base_path.resize(hash);
    if (auto query = base_path.find('?'); query != std::string::npos)
        base_path.resize(query);

    std::string ref_path(reference);
    std::string suffix;
    auto suffix_pos = ref_path.find_first_of("?#");
    if (suffix_pos != std::string::npos) {
        suffix = ref_path.substr(suffix_pos);
        ref_path.resize(suffix_pos);
    }

    std::string combined;
    if (!ref_path.empty() && ref_path.front() == '/') {
        combined = ref_path;
    } else {
        const auto slash = base_path.rfind('/');
        const auto dir = slash == std::string::npos ? std::string("/") : base_path.substr(0, slash + 1);
        combined = dir + ref_path;
    }
    return origin + normalize_url_path(combined) + suffix;
}

static std::string percent_decode(std::string_view input) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
    };
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            int hi = hex(input[i + 1]);
            int lo = hex(input[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(input[i]);
    }
    return out;
}

struct ParsedDataUri {
    std::string mime;
    std::vector<uint8_t> bytes;
    bool valid = false;
};

static ParsedDataUri parse_data_uri(std::string_view uri) {
    ParsedDataUri parsed;
    if (!is_data_uri(uri)) return parsed;
    auto comma = uri.find(',');
    if (comma == std::string_view::npos) return parsed;
    std::string meta(uri.substr(5, comma - 5));
    std::string payload(uri.substr(comma + 1));
    bool base64 = false;
    std::string mime;
    std::stringstream meta_stream(meta);
    std::string segment;
    while (std::getline(meta_stream, segment, ';')) {
        if (segment == "base64") base64 = true;
        else if (mime.empty() && !segment.empty()) mime = segment;
    }
    if (mime.empty()) mime = "text/plain;charset=US-ASCII";
    parsed.mime = mime;
    if (base64) {
        auto decoded = pulp::runtime::base64_decode(payload);
        if (!decoded) return parsed;
        parsed.bytes = std::move(*decoded);
    } else {
        auto decoded = percent_decode(payload);
        parsed.bytes.assign(decoded.begin(), decoded.end());
    }
    parsed.valid = true;
    return parsed;
}

static std::string extension_lower(const fs::path& path) {
    auto ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

static std::string guess_asset_mime_type(const std::string& uri,
                                         const std::vector<uint8_t>& bytes = {}) {
    if (bytes.size() >= 8
        && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')
        return "image/png";
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff)
        return "image/jpeg";
    if (bytes.size() >= 6
        && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F')
        return "image/gif";
    if (bytes.size() >= 12
        && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F'
        && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B' && bytes[11] == 'P')
        return "image/webp";
    if (bytes.size() >= 4 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' && bytes[3] == '2')
        return "font/woff2";
    if (bytes.size() >= 4 && bytes[0] == 'w' && bytes[1] == 'O' && bytes[2] == 'F' && bytes[3] == 'F')
        return "font/woff";
    if (bytes.size() >= 5) {
        std::string head(reinterpret_cast<const char*>(bytes.data()),
                         std::min<size_t>(bytes.size(), 128));
        auto first = head.find_first_not_of(" \t\r\n");
        if (first != std::string::npos
            && head.compare(first, 4, "<svg") == 0)
            return "image/svg+xml";
    }

    auto ext = extension_lower(fs::path(uri));
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".webp") return "image/webp";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".woff") return "font/woff";
    if (ext == ".ttf") return "font/ttf";
    if (ext == ".otf") return "font/otf";
    if (ext == ".json") return "application/json";
    if (ext == ".css") return "text/css";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    return "application/octet-stream";
}

static void fill_png_dimensions(IRAssetRef& asset, const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 24) return;
    if (!(bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G')) return;
    auto be32 = [&](size_t offset) -> int {
        return static_cast<int>((static_cast<uint32_t>(bytes[offset]) << 24)
            | (static_cast<uint32_t>(bytes[offset + 1]) << 16)
            | (static_cast<uint32_t>(bytes[offset + 2]) << 8)
            | static_cast<uint32_t>(bytes[offset + 3]));
    };
    asset.width = be32(16);
    asset.height = be32(20);
}

static std::vector<uint8_t> read_binary_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return {};
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}


static bool write_binary_file(const fs::path& path,
                              const std::vector<uint8_t>& bytes);
static fs::path default_asset_cache_directory();

void enrich_imported_image_asset_metadata(DesignIR& ir,
                                          const IRAssetManifest& manifest,
                                          std::string_view base_directory) {
    if (manifest.assets.empty()) return;
    const fs::path base_dir{std::string(base_directory)};

    std::function<void(IRNode&)> visit = [&](IRNode& node) {
        auto asset_ref = node.attributes.find("asset_ref");
        if (asset_ref != node.attributes.end() && !asset_ref->second.empty()) {
            if (const auto* ref = manifest.resolve(asset_ref->second)) {
                std::vector<uint8_t> bytes;
                std::pair<int, int> dims{ref->width.value_or(0), ref->height.value_or(0)};
                if (ref->local_path && !ref->local_path->empty()) {
                    fs::path path(*ref->local_path);
                    if (path.is_relative() && !base_dir.empty())
                        path = base_dir / path;
                    path = path.lexically_normal();
                    node.attributes["asset_path"] = path.string();
                    bytes = read_binary_file(path);
                    if (!bytes.empty()) {
                        if (auto file_dims = png_dimensions_from_bytes(bytes);
                            file_dims.first > 0 && file_dims.second > 0) {
                            dims = file_dims;
                            node.attributes["png_natural_w"] = std::to_string(file_dims.first);
                            node.attributes["png_natural_h"] = std::to_string(file_dims.second);
                        }
                        if (node.style.render_bounds) {
                            if (auto core = compute_import_opaque_core(bytes)) {
                                node.attributes["art_core_x"] = std::to_string(core->x);
                                node.attributes["art_core_y"] = std::to_string(core->y);
                                node.attributes["art_core_w"] = std::to_string(core->w);
                                node.attributes["art_core_h"] = std::to_string(core->h);
                                node.attributes["png_natural_w"] = std::to_string(core->png_w);
                                node.attributes["png_natural_h"] = std::to_string(core->png_h);
                                dims = {core->png_w, core->png_h};

                                // Captured-art knob: erase the indicator baked
                                // into the disc art (we draw our own rotating
                                // pointer) by re-encoding a cleaned disc PNG.
                                if (node.attributes.count("knob_ind_r_out")) {
                                    if (auto img = decode_png_rgba_for_import(bytes)) {
                                        clean_baked_knob_indicator(*img, *core);
                                        if (auto enc = encode_rgba_png_for_import(*img)) {
                                            // The cleaned disc lands in the durable
                                            // asset cache, not the OS temp dir: this
                                            // path is serialized into asset_path and
                                            // reloaded at RUNTIME by a shipped baked
                                            // UI, so a temp sweep would silently
                                            // unskin the knob. Content-addressed by
                                            // sha256 like the rest of the pipeline —
                                            // std::hash is implementation-defined and
                                            // unstable across runs and compilers.
                                            fs::path dir = default_asset_cache_directory();
                                            std::error_code dec;
                                            fs::create_directories(dir, dec);
                                            const auto digest = pulp::runtime::sha256_hex(
                                                enc->data(), enc->size());
                                            fs::path cleaned = dir /
                                                ("knobclean_" + digest.substr(0, 16) + ".png");
                                            if (write_binary_file(cleaned, *enc))
                                                node.attributes["asset_path"] =
                                                    cleaned.string();
                                        }
                                    }
                                }
                            }
                        }
                        // Shape illustration: sample its OWN gradient so a later
                        // opt-in value-driven fill shows the shape's real colors.
                        // Independent of render_bounds — ELYSIUM's Torus / Triangle
                        // / Pentagon / Cube carry no render_bounds yet are the
                        // colorful fillable shapes. Metadata only (inert until a
                        // fill value is driven), and skipped for captured knobs
                        // (which skin separately and must not fill).
                        if (!node.attributes.count("knob_ind_r_out")) {
                            if (auto core = compute_import_opaque_core(bytes)) {
                                if (auto img = decode_png_rgba_for_import(bytes)) {
                                    auto grad = sample_shape_fill_gradient(*img, *core);
                                    if (!grad.empty())
                                        node.attributes["shape_fill_gradient"] = grad;
                                }
                            }
                        }
                    }
                } else if (dims.first > 0 && dims.second > 0) {
                    node.attributes["png_natural_w"] = std::to_string(dims.first);
                    node.attributes["png_natural_h"] = std::to_string(dims.second);
                }

                // Figma-plugin bundled-image exports are currently 2x PNGs while
                // the IR layout box is in logical design pixels. This only stamps
                // a best-effort bleed hint; render_bounds plus opaque-core metadata
                // remains the stronger placement signal when available.
                constexpr float kExportScale = 2.0f;
                const float layout_w = node.style.width.value_or(0.0f);
                const float layout_h = node.style.height.value_or(0.0f);
                if (dims.first > 0 && dims.second > 0 && layout_w > 0.0f && layout_h > 0.0f) {
                    const float natural_w = static_cast<float>(dims.first) / kExportScale;
                    const float natural_h = static_cast<float>(dims.second) / kExportScale;
                    const float rw = natural_w / layout_w;
                    const float rh = natural_h / layout_h;
                    if (std::max(rw, rh) >= 1.5f)
                        node.attributes["asset_bleed"] = "1";
                }
            }
        }
        for (auto& child : node.children)
            visit(child);
    };
    visit(ir.root);
}

static bool write_binary_file(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

static fs::path default_asset_cache_directory() {
    if (const char* env = std::getenv("PULP_IMPORT_ASSET_CACHE"); env && *env)
        return fs::path(env);
#ifdef _WIN32
    if (const char* local = std::getenv("LOCALAPPDATA"); local && *local)
        return fs::path(local) / "Pulp" / "import-assets";
#endif
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".pulp" / "import-assets";
    return fs::temp_directory_path() / "pulp-import-assets";
}

static std::string asset_id_for(const std::string& stable_key) {
    auto hash = pulp::runtime::sha256_hex(stable_key);
    return "asset-" + hash.substr(0, 16);
}

static void append_unique_asset_alias(IRAssetRef& asset, const std::string& alias) {
    if (alias.empty() || alias == asset.original_uri) return;
    if (std::find(asset.original_uri_aliases.begin(),
                  asset.original_uri_aliases.end(),
                  alias) != asset.original_uri_aliases.end()) {
        return;
    }
    asset.original_uri_aliases.push_back(alias);
}

static std::string url_index_key(const std::string& url) {
    return pulp::runtime::sha256_hex(url).substr(0, 32);
}

static std::optional<std::vector<uint8_t>> fetch_network_asset(
    const std::string& url,
    const fs::path& cache_dir,
    int timeout_ms,
    bool allow_fetch,
    IRAssetRef& asset) {
    const auto url_index = cache_dir / "by-url" / (url_index_key(url) + ".txt");
    std::ifstream index_file(url_index);
    std::string cached_hash;
    if (index_file.good()) {
        std::getline(index_file, cached_hash);
        auto cached_path = cache_dir / "by-hash" / cached_hash;
        if (!cached_hash.empty() && fs::exists(cached_path)) {
            asset.local_path = cached_path.string();
            return read_binary_file(cached_path);
        }
    }

    if (!allow_fetch) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::warning,
            "asset-network-fetch-disabled",
            url,
            "network asset requires --allow-network-fetch",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }

    auto curl = pulp::platform::find_on_path("curl");
    if (!curl) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::error,
            "asset-fetcher-missing",
            asset.original_uri,
            "curl was not found on PATH",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }

    pulp::runtime::TemporaryFile temp_file(".download");
    fs::path temp_path = temp_file.path();
    pulp::platform::ProcessOptions process_opts;
    process_opts.timeout_ms = timeout_ms;
    process_opts.max_output_bytes = 64 * 1024;
    auto result = pulp::platform::ChildProcess::run(
        curl->string(),
        {"-fsSL", "--max-time", std::to_string(std::max(1, timeout_ms / 1000)),
         "--output", temp_path.string(), url},
        process_opts);
    if (result.timed_out) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::error,
            "asset-fetch-timeout",
            asset.original_uri,
            "timed out while fetching asset",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }
    if (result.exit_code != 0) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::error,
            "asset-fetch-failed",
            asset.original_uri,
            result.stderr_output.empty() ? "curl failed while fetching asset" : result.stderr_output,
            ImportDiagnosticKind::unresolved_asset));
        std::error_code ec;
        fs::remove(temp_path, ec);
        return std::nullopt;
    }

    auto bytes = read_binary_file(temp_path);
    if (bytes.empty()) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::error,
            "asset-empty",
            asset.original_uri,
            "fetched asset was empty",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }

    return bytes;
}

static void cache_network_asset(const std::string& url,
                                const fs::path& cache_dir,
                                const std::string& hash,
                                const std::vector<uint8_t>& bytes,
                                IRAssetRef& asset) {
    auto cache_path = cache_dir / "by-hash" / hash;
    if (!write_binary_file(cache_path, bytes))
        return;

    std::error_code ec;
    const auto url_index = cache_dir / "by-url" / (url_index_key(url) + ".txt");
    fs::create_directories(url_index.parent_path(), ec);
    std::ofstream out_index(url_index);
    out_index << hash << "\n";
    asset.local_path = cache_path.string();
}

static std::optional<std::vector<uint8_t>> resolve_local_asset(
    const std::string& uri,
    const fs::path& base_directory,
    IRAssetRef& asset) {
    fs::path path;
    if (has_prefix(uri, "file://")) {
        path = fs::path(uri.substr(7));
    } else {
        path = fs::path(uri);
        if (path.is_relative() && !base_directory.empty())
            path = base_directory / path;
    }
    std::error_code ec;
    if (!fs::exists(path, ec) || fs::is_directory(path, ec)) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::warning,
            "asset-unresolved",
            asset.original_uri,
            "asset file was not found",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }
    auto bytes = read_binary_file(path);
    if (bytes.empty()) {
        asset.diagnostics.push_back(make_import_diagnostic(
            ImportDiagnosticSeverity::warning,
            "asset-empty",
            asset.original_uri,
            "asset file was empty or unreadable",
            ImportDiagnosticKind::unresolved_asset));
        return std::nullopt;
    }
    asset.local_path = path.string();
    return bytes;
}

static void collect_url_tokens(std::string_view value, std::vector<std::string>& out) {
    if (value.empty()) return;
    if (is_data_uri(value) || is_network_url(value) || has_prefix(value, "file://")) {
        out.emplace_back(value);
        return;
    }
    size_t pos = 0;
    while ((pos = value.find("url(", pos)) != std::string_view::npos) {
        pos += 4;
        auto end = value.find(')', pos);
        if (end == std::string_view::npos) break;
        auto token = value.substr(pos, end - pos);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
            token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.remove_suffix(1);
        if (token.size() >= 2
            && ((token.front() == '"' && token.back() == '"')
             || (token.front() == '\'' && token.back() == '\''))) {
            token.remove_prefix(1);
            token.remove_suffix(1);
        }
        if (!token.empty()) out.emplace_back(token);
        pos = end + 1;
    }
}

struct HtmlAssetCandidate {
    std::string uri;
    std::optional<std::string> font_family;
};

static std::string strip_matching_css_quotes(std::string value) {
    value = std::string(choc::text::trim(value));
    if (value.size() >= 2
        && ((value.front() == '"' && value.back() == '"')
         || (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

static void append_html_asset_candidate(std::vector<HtmlAssetCandidate>& out,
                                        std::string uri,
                                        std::optional<std::string> font_family = std::nullopt) {
    if (uri.empty()) return;
    for (auto& candidate : out) {
        if (candidate.uri == uri) {
            if (!candidate.font_family && font_family)
                candidate.font_family = std::move(font_family);
            return;
        }
    }
    out.push_back({std::move(uri), std::move(font_family)});
}

static void collect_font_face_asset_candidates(const std::string& css,
                                               std::vector<HtmlAssetCandidate>& out) {
    static const std::regex font_face_re(
        R"RX(@font-face\s*\{([\s\S]*?)\})RX",
        std::regex::icase);
    static const std::regex font_family_re(
        R"RX(font-family\s*:\s*([^;]+))RX",
        std::regex::icase);
    static const std::regex src_re(
        R"RX(src\s*:\s*([^;]+))RX",
        std::regex::icase);

    auto begin = std::sregex_iterator(css.begin(), css.end(), font_face_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const auto body = (*it)[1].str();
        std::smatch family_match;
        std::optional<std::string> font_family;
        if (std::regex_search(body, family_match, font_family_re))
            font_family = strip_matching_css_quotes(family_match[1].str());

        std::smatch src_match;
        if (!std::regex_search(body, src_match, src_re)) continue;

        std::vector<std::string> tokens;
        collect_url_tokens(src_match[1].str(), tokens);
        for (auto& token : tokens)
            append_html_asset_candidate(out, std::move(token), font_family);
    }
}

static std::vector<HtmlAssetCandidate> collect_html_asset_uris(const std::string& html) {
    std::vector<HtmlAssetCandidate> assets;

    static const std::regex style_block_re(
        R"RX(<style\b[^>]*>([\s\S]*?)</style>)RX",
        std::regex::icase);
    auto style_begin = std::sregex_iterator(html.begin(), html.end(), style_block_re);
    auto style_end = std::sregex_iterator();
    for (auto it = style_begin; it != style_end; ++it) {
        const auto css = (*it)[1].str();
        collect_font_face_asset_candidates(css, assets);
        std::vector<std::string> tokens;
        collect_url_tokens(css, tokens);
        for (auto& token : tokens)
            append_html_asset_candidate(assets, std::move(token));
    }

    static const std::regex style_attr_re(
        R"RX(\bstyle\s*=\s*(['"])([\s\S]*?)\1)RX",
        std::regex::icase);
    auto attr_begin = std::sregex_iterator(html.begin(), html.end(), style_attr_re);
    auto attr_end = std::sregex_iterator();
    for (auto it = attr_begin; it != attr_end; ++it) {
        std::vector<std::string> tokens;
        collect_url_tokens((*it)[2].str(), tokens);
        for (auto& token : tokens)
            append_html_asset_candidate(assets, std::move(token));
    }

    static const std::regex direct_asset_attr_re(
        R"RX(\b(?:src|href)\s*=\s*(['"])([^'"]+)\1)RX",
        std::regex::icase);
    auto direct_begin = std::sregex_iterator(html.begin(), html.end(), direct_asset_attr_re);
    auto direct_end = std::sregex_iterator();
    for (auto it = direct_begin; it != direct_end; ++it) {
        std::string uri = (*it)[2].str();
        if (uri.empty() || uri.front() == '#') continue;
        std::string lower = uri;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (has_prefix(lower, "javascript:") || has_prefix(lower, "mailto:")) continue;
        append_html_asset_candidate(assets, std::move(uri));
    }

    return assets;
}

static void collect_asset_uris_from_node(const IRNode& node, std::vector<std::string>& uris) {
    auto collect = [&](const std::optional<std::string>& value) {
        if (value) collect_url_tokens(*value, uris);
    };
    collect(node.style.background_image);
    collect(node.style.background_gradient);
    collect(node.style.border);
    collect(node.style.filter);
    collect(node.style.backdrop_filter);
    for (const auto& [key, value] : node.attributes) {
        const bool known_asset_key = is_asset_reference_key(key);
        if (known_asset_key || value.find("url(") != std::string::npos
            || is_data_uri(value) || is_network_url(value) || has_prefix(value, "file://")) {
            const auto before = uris.size();
            collect_url_tokens(value, uris);
            if (known_asset_key && uris.size() == before && !value.empty())
                uris.emplace_back(value);
        }
    }
    for (const auto& child : node.children)
        collect_asset_uris_from_node(child, uris);
}

static void collect_font_family_metadata_from_node(
    const IRNode& node,
    std::unordered_map<std::string, std::string>& font_family_by_uri) {
    for (const auto& [key, value] : node.attributes) {
        static constexpr std::string_view kPrefix = "htmlFontFamily";
        if (key.rfind(kPrefix, 0) != 0) continue;
        const auto suffix = key.substr(kPrefix.size());
        const auto asset_key = std::string("htmlAsset") + suffix;
        auto asset = node.attributes.find(asset_key);
        if (asset != node.attributes.end() && !asset->second.empty() && !value.empty())
            font_family_by_uri[asset->second] = value;
    }
    for (const auto& child : node.children)
        collect_font_family_metadata_from_node(child, font_family_by_uri);
}

IRAssetManifest collect_design_ir_assets(const DesignIR& ir,
                                         const DesignIrAssetOptions& options) {
    IRAssetManifest manifest;
    std::vector<std::string> uris;
    collect_asset_uris_from_node(ir.root, uris);
    std::unordered_map<std::string, std::string> font_family_by_uri;
    collect_font_family_metadata_from_node(ir.root, font_family_by_uri);

    std::unordered_map<std::string, size_t> asset_index_by_key;
    const auto cache_dir = options.cache_directory.empty()
        ? default_asset_cache_directory()
        : options.cache_directory;

    for (const auto& uri : uris) {
        const auto resolved_uri = (!is_data_uri(uri) && !is_network_url(uri)
            && !has_prefix(uri, "file://") && !options.base_url.empty())
            ? resolve_url_reference(options.base_url, uri)
            : uri;
        IRAssetRef asset;
        asset.original_uri = uri;
        asset.source_url = is_network_url(resolved_uri)
            ? std::optional<std::string>(resolved_uri)
            : std::nullopt;
        if (auto family = font_family_by_uri.find(uri); family != font_family_by_uri.end())
            asset.font_family = family->second;
        else if (auto resolved_family = font_family_by_uri.find(resolved_uri);
                 resolved_family != font_family_by_uri.end())
            asset.font_family = resolved_family->second;
        std::optional<std::vector<uint8_t>> bytes;

        if (is_data_uri(resolved_uri)) {
            auto parsed = parse_data_uri(uri);
            if (parsed.valid) {
                bytes = std::move(parsed.bytes);
                asset.mime = parsed.mime;
            } else {
                asset.diagnostics.push_back(make_import_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "asset-data-uri-invalid",
                    uri,
                    "data URI could not be decoded",
                    ImportDiagnosticKind::unresolved_asset));
            }
        } else if (is_network_url(resolved_uri)) {
            bytes = fetch_network_asset(resolved_uri, cache_dir, options.network_timeout_ms,
                                        options.allow_network_fetch, asset);
        } else {
            bytes = resolve_local_asset(resolved_uri, options.base_directory, asset);
        }

        if (bytes && !bytes->empty()) {
            asset.content_hash = pulp::runtime::sha256_hex(bytes->data(), bytes->size());
            if (asset.mime.empty()) asset.mime = guess_asset_mime_type(resolved_uri, *bytes);
            fill_png_dimensions(asset, *bytes);
            auto expected = options.expected_hash_by_uri.find(uri);
            if (expected == options.expected_hash_by_uri.end())
                expected = options.expected_hash_by_uri.find(resolved_uri);
            bool hash_mismatch = false;
            if (expected != options.expected_hash_by_uri.end()
                && expected->second != asset.content_hash) {
                hash_mismatch = true;
                asset.diagnostics.push_back(make_import_diagnostic(
                    ImportDiagnosticSeverity::error,
                    "asset-hash-mismatch",
                    uri,
                    "resolved asset hash did not match the expected hash",
                    ImportDiagnosticKind::unresolved_asset));
            }
            if (is_network_url(resolved_uri) && !asset.local_path && !hash_mismatch)
                cache_network_asset(resolved_uri, cache_dir, asset.content_hash, *bytes, asset);
        } else if (asset.mime.empty()) {
            asset.mime = guess_asset_mime_type(resolved_uri);
        }

        const auto dedupe_key = is_data_uri(uri) && !asset.content_hash.empty()
            ? std::string("data:") + asset.content_hash
            : asset.source_url.value_or(asset.local_path.value_or(asset.original_uri));
        auto [known, inserted] = asset_index_by_key.emplace(dedupe_key, manifest.assets.size());
        if (!inserted) {
            auto& existing = manifest.assets[known->second];
            append_unique_asset_alias(existing, asset.original_uri);
            if (!existing.font_family && asset.font_family)
                existing.font_family = asset.font_family;
            continue;
        }
        asset.asset_id = asset_id_for(dedupe_key);
        manifest.assets.push_back(std::move(asset));
    }

    std::sort(manifest.assets.begin(), manifest.assets.end(), [](const auto& a, const auto& b) {
        return a.asset_id < b.asset_id;
    });
    return manifest;
}

static std::optional<std::string> find_asset_id_for_value(
    const std::string& value,
    const std::unordered_map<std::string, std::string>& asset_id_by_uri) {
    if (auto found = asset_id_by_uri.find(value); found != asset_id_by_uri.end())
        return found->second;

    std::vector<std::string> tokens;
    collect_url_tokens(value, tokens);
    for (const auto& token : tokens) {
        if (auto found = asset_id_by_uri.find(token); found != asset_id_by_uri.end())
            return found->second;
    }
    return std::nullopt;
}

static std::string asset_id_attribute_for(std::string_view key) {
    if (key == "background-image") return "backgroundImageAssetId";
    return std::string(key) + "AssetId";
}

static void annotate_asset_ids(
    IRNode& node,
    const std::unordered_map<std::string, std::string>& asset_id_by_uri) {
    if (node.style.background_image) {
        if (auto asset_id = find_asset_id_for_value(*node.style.background_image, asset_id_by_uri))
            node.attributes["backgroundImageAssetId"] = *asset_id;
    }

    std::vector<std::pair<std::string, std::string>> to_add;
    for (const auto& [key, value] : node.attributes) {
        if (!is_asset_reference_key(key) && value.find("url(") == std::string::npos)
            continue;
        if (auto asset_id = find_asset_id_for_value(value, asset_id_by_uri)) {
            const auto asset_attr = asset_id_attribute_for(key);
            to_add.emplace_back(asset_attr, *asset_id);
        }
    }
    for (const auto& [key, value] : to_add)
        node.attributes[key] = value;

    for (auto& child : node.children)
        annotate_asset_ids(child, asset_id_by_uri);
}

static bool is_derived_asset_id_attribute(std::string_view key) {
    static constexpr std::string_view kSuffix = "AssetId";
    return key.size() >= kSuffix.size() && key.substr(key.size() - kSuffix.size()) == kSuffix;
}

static void clear_asset_id_annotations(IRNode& node) {
    for (auto it = node.attributes.begin(); it != node.attributes.end();) {
        if (is_derived_asset_id_attribute(it->first)) {
            it = node.attributes.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& child : node.children)
        clear_asset_id_annotations(child);
}

void refresh_design_ir_asset_manifest(DesignIR& ir,
                                      const DesignIrAssetOptions& options) {
    auto embedded_manifest = ir.asset_manifest;
    auto refreshed_manifest = collect_design_ir_assets(ir, options);
    const bool preserve_all_embedded_assets =
        ir.source == DesignSource::figma_plugin || ir.source_adapter == "figma-plugin";
    std::unordered_map<std::string, bool> explicitly_referenced_asset_ids;
    std::function<void(const IRNode&)> collect_explicit_asset_ids = [&](const IRNode& node) {
        if (auto it = node.attributes.find("asset_ref");
            it != node.attributes.end() && !it->second.empty()) {
            explicitly_referenced_asset_ids.emplace(it->second, true);
        }
        for (const auto& child : node.children)
            collect_explicit_asset_ids(child);
    };
    collect_explicit_asset_ids(ir.root);
    for (const auto& font : ir.font_family_assets) {
        if (!font.asset_id.empty())
            explicitly_referenced_asset_ids.emplace(font.asset_id, true);
    }

    if (!embedded_manifest.assets.empty()) {
        std::unordered_map<std::string, size_t> refreshed_by_id;
        for (size_t i = 0; i < refreshed_manifest.assets.size(); ++i) {
            if (!refreshed_manifest.assets[i].asset_id.empty())
                refreshed_by_id.emplace(refreshed_manifest.assets[i].asset_id, i);
        }
        for (auto& asset : embedded_manifest.assets) {
            if (!asset.asset_id.empty()
                && refreshed_by_id.find(asset.asset_id) != refreshed_by_id.end()) {
                auto& refreshed = refreshed_manifest.assets[refreshed_by_id[asset.asset_id]];
                if (refreshed.original_uri.empty()) refreshed.original_uri = asset.original_uri;
                for (const auto& alias : asset.original_uri_aliases)
                    append_unique_asset_alias(refreshed, alias);
                if (!refreshed.local_path && asset.local_path)
                    refreshed.local_path = asset.local_path;
                if (refreshed.content_hash.empty())
                    refreshed.content_hash = asset.content_hash;
                if (refreshed.mime.empty())
                    refreshed.mime = asset.mime;
                if (!refreshed.width && asset.width)
                    refreshed.width = asset.width;
                if (!refreshed.height && asset.height)
                    refreshed.height = asset.height;
                if (!refreshed.font_family && asset.font_family)
                    refreshed.font_family = asset.font_family;
                if (!refreshed.license && asset.license)
                    refreshed.license = asset.license;
                if (!refreshed.source_url && asset.source_url)
                    refreshed.source_url = asset.source_url;
                refreshed.diagnostics.insert(refreshed.diagnostics.end(),
                                             asset.diagnostics.begin(),
                                             asset.diagnostics.end());
            } else if (preserve_all_embedded_assets
                       || (!asset.asset_id.empty()
                           && explicitly_referenced_asset_ids.find(asset.asset_id)
                                != explicitly_referenced_asset_ids.end())) {
                refreshed_manifest.assets.push_back(std::move(asset));
            }
        }
        std::sort(refreshed_manifest.assets.begin(), refreshed_manifest.assets.end(),
                  [](const auto& a, const auto& b) {
                      return a.asset_id < b.asset_id;
                  });
    }

    ir.asset_manifest = std::move(refreshed_manifest);
    std::unordered_map<std::string, std::string> asset_id_by_uri;
    for (const auto& asset : ir.asset_manifest.assets) {
        if (!asset.original_uri.empty())
            asset_id_by_uri.emplace(asset.original_uri, asset.asset_id);
        for (const auto& alias : asset.original_uri_aliases)
            asset_id_by_uri.emplace(alias, asset.asset_id);
        if (asset.source_url)
            asset_id_by_uri.emplace(*asset.source_url, asset.asset_id);
        if (asset.local_path)
            asset_id_by_uri.emplace(*asset.local_path, asset.asset_id);
    }
    clear_asset_id_annotations(ir.root);
    annotate_asset_ids(ir.root, asset_id_by_uri);
}

// ── Source adapters ─────────────────────────────────────────────────────

DesignIR parse_figma_plugin_json(const std::string& json) {
    // Figma-plugin envelope shape:
    //   { format_version, parser_version, compat_schema_version,
    //     provenance: {adapter, version, source_uri, exported_at},
    //     library_manifest?: {...},
    //     tokens?: {colors,dimensions,strings},
    //     asset_manifest?: {version, assets[]},
    //     diagnostics?: [...],
    //     root: <IRNode-shaped> }
    DesignIR ir;
    ir.source = DesignSource::figma_plugin;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "figma-plugin";

    auto parsed = choc::json::parse(json);

    if (parsed.hasObjectMember("parser_version") && parsed["parser_version"].isString()) {
        ir.source_version = std::string(parsed["parser_version"].toString());
    } else {
        ir.source_version = "0.1.0";
    }

    if (parsed.hasObjectMember("root")) {
        ir.root = parse_ir_node(parsed["root"]);
    } else {
        // Defensive fallback — treat the whole document as the root.
        ir.root = parse_ir_node(parsed);
    }
    validate_blend_modes(ir.root, ir.diagnostics);

    if (parsed.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(parsed["tokens"]);
    if (parsed.hasObjectMember("token_source_identity"))
        parse_ir_token_source_identity(parsed["token_source_identity"], ir.tokens);

    if (parsed.hasObjectMember("asset_manifest"))
        ir.asset_manifest = parse_asset_manifest(parsed["asset_manifest"]);
    else if (parsed.hasObjectMember("assetManifest"))
        ir.asset_manifest = parse_asset_manifest(parsed["assetManifest"]);

    // font_family_assets[] — bundled fonts the importer registers
    // so setFontFamily resolves to the bundled face, not a system fallback.
    {
        auto read_str = [](const choc::value::ValueView& o, const char* k) -> std::string {
            return (o.hasObjectMember(k) && o[k].isString()) ? std::string(o[k].toString())
                                                             : std::string();
        };
        const char* fkey = parsed.hasObjectMember("font_family_assets") ? "font_family_assets"
                          : parsed.hasObjectMember("fontFamilyAssets")  ? "fontFamilyAssets"
                                                                        : nullptr;
        if (fkey && parsed[fkey].isArray()) {
            auto arr = parsed[fkey];
            for (uint32_t i = 0; i < arr.size(); ++i) {
                auto e = arr[static_cast<int>(i)];
                if (!e.isObject()) continue;
                IRFontAsset fa;
                fa.family = read_str(e, "family");
                fa.style = read_str(e, "style");
                if (e.hasObjectMember("weight"))
                    fa.weight = static_cast<int>(e["weight"].getWithDefault<int64_t>(400));
                fa.asset_id = read_str(e, "asset_id");
                if (fa.asset_id.empty()) fa.asset_id = read_str(e, "assetId");
                if (!fa.family.empty()) ir.font_family_assets.push_back(std::move(fa));
            }
        }
    }

    IRProvenance provenance{"figma-plugin", ir.source_version, {}};
    if (parsed.hasObjectMember("provenance") && parsed["provenance"].isObject()) {
        auto pr = parsed["provenance"];
        if (pr.hasObjectMember("adapter") && pr["adapter"].isString())
            provenance.adapter = std::string(pr["adapter"].toString());
        if (pr.hasObjectMember("version") && pr["version"].isString())
            provenance.version = std::string(pr["version"].toString());
        if (pr.hasObjectMember("source_uri") && pr["source_uri"].isString())
            provenance.source_uri = std::string(pr["source_uri"].toString());
        else if (pr.hasObjectMember("sourceUri") && pr["sourceUri"].isString())
            provenance.source_uri = std::string(pr["sourceUri"].toString());
    }
    ir.root.provenance = std::move(provenance);
    ir.root.confidence = IRConfidence::pass;

    // The envelope's diagnostics are the whole point of emitting them: they are
    // how a fidelity loss becomes a stated result instead of a silent one. This
    // parser documented `diagnostics?: [...]` in its own shape comment above and
    // then never read the field, so `ir.diagnostics` stayed empty and
    // `print_import_diagnostics` had nothing to print.
    //
    // Nothing about that was visible. The decoder emitted the warnings correctly
    // and wrote them to scene.pulp.json; the JS lane even printed "N warning(s)"
    // from its own array. But `--from fig` rewrites its source to figma-plugin
    // and lands HERE, so every gradient flattened to its mean, every dropped
    // blend mode, every missing icon font and every unresolvable asset was
    // announced to a file and to nobody. The skill promises the opposite
    // (import-design SKILL.md: "reported as named warnings ... rather than
    // silently dropped") — true of the JSON on disk, false of the CLI.
    //
    // A comment describing a field the code does not parse is the same failure
    // this whole lane keeps producing: a chain that runs end to end with one end
    // quietly unplugged, where nothing errors and the result is merely wrong.
    if (parsed.hasObjectMember("diagnostics") && parsed["diagnostics"].isArray()) {
        const auto diags = parsed["diagnostics"];
        // Local to this parser and matching its house idiom (hasObjectMember +
        // isString), rather than reaching for a helper this TU does not have.
        auto str = [](const choc::value::ValueView& o, const char* k) -> std::string {
            return (o.hasObjectMember(k) && o[k].isString())
                       ? std::string(o[k].toString()) : std::string{};
        };
        for (uint32_t i = 0; i < diags.size(); ++i) {
            const auto d = diags[i];
            if (!d.isObject()) continue;
            ImportDiagnostic out;
            out.code = str(d, "code");
            if (out.code.empty()) continue;  // a diagnostic with no code says nothing
            // `detail` is the decoder's field name; accept `message` too so a
            // different emitter of this envelope is not silently blanked.
            out.message = str(d, "detail");
            if (out.message.empty()) out.message = str(d, "message");
            // `path` is the plugin/REST emitters' field name; the .fig decoder
            // has none, so prefer its readable node name there and fall back to
            // the guid, which is at least resolvable.
            out.path = str(d, "path");
            if (out.path.empty()) out.path = str(d, "node_name");
            if (out.path.empty()) out.path = str(d, "node_id");
            const std::string sev = str(d, "severity");
            out.severity = sev == "error" ? ImportDiagnosticSeverity::error
                         : sev == "info"  ? ImportDiagnosticSeverity::info
                                          : ImportDiagnosticSeverity::warning;
            // The plugin/REST emitters carry the schema's structured `kind`
            // (figma-plugin-export-v1.json); the .fig decoder omits it, and an
            // unrecognized value stays `unknown` rather than guessing.
            const std::string kind = str(d, "kind");
            out.kind = kind == "unsupported_property" ? ImportDiagnosticKind::unsupported_property
                     : kind == "unsupported_node"     ? ImportDiagnosticKind::unsupported_node
                     : kind == "unresolved_asset"     ? ImportDiagnosticKind::unresolved_asset
                     : kind == "capture_partial"      ? ImportDiagnosticKind::capture_partial
                     : kind == "fallback_used"        ? ImportDiagnosticKind::fallback_used
                                                      : ImportDiagnosticKind::unknown;
            if (auto id = str(d, "node_id"); !id.empty()) out.anchor_id = id;
            ir.diagnostics.push_back(std::move(out));
        }
    }

    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::adapter, "figma-plugin");

    return ir;
}

DesignIR parse_figma_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "figma";
    ir.source_version = "1";

    auto root = choc::json::parse(json);

    // Figma export can be the IR format directly (from MCP or export tool)
    ir.root = parse_ir_node(root);
    validate_blend_modes(ir.root, ir.diagnostics);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);

    // Tag the root with adapter provenance + confidence, then assign
    // stable_anchor_id values via the adapter strategy. Figma exports carry
    // native layer UUIDs that parse_ir_node populates into source_node_id; the
    // adapter strategy prefixes those with "figma:". Nodes that don't have a
    // native ID (rare in Figma) silently fall through to the content-hash
    // branch inside compute_anchor_id.
    ir.root.provenance = IRProvenance{"figma", "1", /*source_uri=*/{}};
    ir.root.confidence = IRConfidence::pass;
    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::adapter, "figma");

    return ir;
}

DesignIR parse_stitch_html(const std::string& html) {
    DesignIR ir;
    ir.source = DesignSource::stitch;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "stitch-html";
    ir.source_version = "1";

    // Try parsing as JSON IR first (from Stitch MCP get_screen)
    try {
        auto root = choc::json::parse(html);
        ir.root = parse_ir_node(root);
        validate_blend_modes(ir.root, ir.diagnostics);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
        ir.root.provenance = IRProvenance{"stitch-html", "1", {}};
        ir.root.confidence = IRConfidence::pass;
        promote_interactive_frames(ir.root);
        assign_anchors(ir.root, AnchorStrategy::content_hash);
        return ir;
    } catch (...) {
        // Not JSON — minimal HTML extraction
    }

    // Basic HTML → IR: extract inline styles from HTML elements
    // For full HTML parsing, the AI pipeline handles translation
    ir.root.type = "frame";
    ir.root.name = "StitchImport";
    ir.root.layout.direction = LayoutDirection::column;

    // Extract text content between tags as a simple fallback
    std::regex tag_re("<([a-z][a-z0-9]*)[^>]*>([^<]*)</\\1>");
    auto begin = std::sregex_iterator(html.begin(), html.end(), tag_re);
    auto end = std::sregex_iterator();

    // Non-visible elements carry raw code / metadata, not rendered text: a
    // `<script>` body (e.g. a Stitch `tailwind.config = {...}` block or a Claude
    // bundler placeholder) or a `<style>` sheet is NOT UI text, and emitting it
    // as a label paints raw JavaScript/CSS into the imported design. The regex
    // matches any `<tag>...</tag>`, so filter these element kinds explicitly.
    static const std::unordered_set<std::string> kNonVisibleTags = {
        "script", "style", "noscript", "template",
        "head", "title", "meta", "link", "base"};

    for (auto it = begin; it != end; ++it) {
        std::string tag = (*it)[1].str();
        std::string content = (*it)[2].str();
        if (content.empty()) continue;
        if (kNonVisibleTags.count(tag)) continue;

        IRNode child;
        child.type = "text";
        child.name = tag;
        child.text_content = content;
        ir.root.children.push_back(std::move(child));
    }

    auto asset_uris = collect_html_asset_uris(html);
    for (size_t i = 0; i < asset_uris.size(); ++i) {
        auto index = std::to_string(i);
        if (asset_uris[i].font_family)
            ir.root.attributes["htmlFontFamily" + index] = *asset_uris[i].font_family;
        ir.root.attributes["htmlAsset" + index] = std::move(asset_uris[i].uri);
    }

    // Assign anchors to the regex-extracted tree.
    ir.root.provenance = IRProvenance{"stitch-html", "1", {}};
    ir.root.confidence = IRConfidence::diverge;  // regex fallback is lossy
    ir.fallback_reason = "input was not JSON; used regex HTML text extraction";
    ir.diagnostics.push_back(make_import_diagnostic(
        ImportDiagnosticSeverity::warning,
        "fallback-used",
        "<root>",
        ir.fallback_reason,
        ImportDiagnosticKind::fallback_used));
    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::content_hash);
    return ir;
}


DesignIR parse_pencil_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::pencil;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "pencil";
    ir.source_version = "1";

    auto root = choc::json::parse(json);

    // Pencil MCP batch_get returns a node tree similar to our IR
    ir.root = parse_ir_node(root);
    validate_blend_modes(ir.root, ir.diagnostics);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);
    if (root.hasObjectMember("variables"))
        ir.tokens = parse_ir_tokens(root["variables"]);

    // Pencil MCP nodes carry a stable `id` that parse_ir_node populates into
    // source_node_id; the adapter strategy uses it directly.
    ir.root.provenance = IRProvenance{"pencil", "1", {}};
    ir.root.confidence = IRConfidence::pass;
    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::adapter, "pencil");

    return ir;
}


namespace {

// Compact, locale-stable float formatter for synthesized SVG path data: fixed
// 3-decimal precision, trailing zeros (and a dangling '.') trimmed. Always
// emits '.' regardless of locale so the `d` string parses identically on every
// host.
std::string svg_num(float v) {
    if (v == 0.0f) return "0";  // also normalizes -0
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s(buf);
    const auto dot = s.find('.');
    if (dot != std::string::npos) {
        auto last = s.find_last_not_of('0');
        if (last == dot) last = dot - 1;  // drop the now-bare '.'
        s.erase(last + 1);
    }
    return s;
}

// The vector SHAPE PRIMITIVE kinds whose `d` we can synthesize from geometry
// alone. Deliberately excludes "vector"/"path"/"svg_path" (those need a real
// authored `d`) and "polyline" (an open run of explicit points we do not have
// without path data). Matched on IR type only — source-agnostic.
bool is_synthesizable_primitive(const std::string& t) {
    return t == "rect" || t == "rectangle" || t == "svg_rect" ||
           t == "line" || t == "svg_line" ||
           t == "ellipse" || t == "circle" ||
           t == "polygon" || t == "star";
}

// Read an integer-valued geometry attribute (e.g. polygon/star pointCount),
// tolerating the camelCase and snake_case spellings different sources emit.
std::optional<long> int_attr(const IRNode& n, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = n.attributes.find(k);
        if (it == n.attributes.end() || it->second.empty()) continue;
        char* end = nullptr;
        const long v = std::strtol(it->second.c_str(), &end, 10);
        if (end != it->second.c_str()) return v;
    }
    return std::nullopt;
}

std::optional<float> float_attr(const IRNode& n, std::initializer_list<const char*> keys) {
    for (const char* k : keys) {
        auto it = n.attributes.find(k);
        if (it == n.attributes.end() || it->second.empty()) continue;
        char* end = nullptr;
        const float v = std::strtof(it->second.c_str(), &end);
        if (end != it->second.c_str()) return v;
    }
    return std::nullopt;
}

std::string synth_rect_path(float w, float h, const IRStyle& s) {
    // Per-corner radius (CSS border-<corner>-radius wins over the uniform
    // border-radius shorthand), each clamped to half the shorter side so the
    // arcs never overrun the box.
    const float base = s.border_radius.value_or(0.0f);
    const float cap = std::min(w, h) * 0.5f;
    auto corner = [&](const std::optional<float>& v) {
        return std::clamp(v.value_or(base), 0.0f, cap);
    };
    const float tl = corner(s.border_top_left_radius);
    const float tr = corner(s.border_top_right_radius);
    const float br = corner(s.border_bottom_right_radius);
    const float bl = corner(s.border_bottom_left_radius);
    std::ostringstream d;
    if (tl <= 0.0f && tr <= 0.0f && br <= 0.0f && bl <= 0.0f) {
        d << "M0 0 H" << svg_num(w) << " V" << svg_num(h) << " H0 Z";
        return d.str();
    }
    // Clockwise from just past the top-left corner; arc sweep flag 1 = CW.
    // Zero-radius corners degenerate to lineto per the SVG arc spec.
    d << "M" << svg_num(tl) << " 0"
      << " H" << svg_num(w - tr)
      << " A" << svg_num(tr) << " " << svg_num(tr) << " 0 0 1 " << svg_num(w) << " " << svg_num(tr)
      << " V" << svg_num(h - br)
      << " A" << svg_num(br) << " " << svg_num(br) << " 0 0 1 " << svg_num(w - br) << " " << svg_num(h)
      << " H" << svg_num(bl)
      << " A" << svg_num(bl) << " " << svg_num(bl) << " 0 0 1 0 " << svg_num(h - bl)
      << " V" << svg_num(tl)
      << " A" << svg_num(tl) << " " << svg_num(tl) << " 0 0 1 " << svg_num(tl) << " 0"
      << " Z";
    return d.str();
}

std::string synth_ellipse_path(float w, float h) {
    const float rx = w * 0.5f, ry = h * 0.5f, cy = ry;
    std::ostringstream d;
    // Two half-arcs trace the full ellipse inside the (w, h) box.
    d << "M0 " << svg_num(cy)
      << " A" << svg_num(rx) << " " << svg_num(ry) << " 0 1 0 " << svg_num(w) << " " << svg_num(cy)
      << " A" << svg_num(rx) << " " << svg_num(ry) << " 0 1 0 0 " << svg_num(cy)
      << " Z";
    return d.str();
}

std::string synth_line_path(float w, float h) {
    // The line spans the bounding-box diagonal; a horizontal line has h≈0 and
    // collapses to M0 0 L w 0, which is what those sources intend.
    std::ostringstream d;
    d << "M0 0 L" << svg_num(w) << " " << svg_num(h);
    return d.str();
}

// Portable pi — MSVC does not define M_PI without _USE_MATH_DEFINES, so use an
// explicit constant rather than relying on a macro (or its include ordering).
constexpr float kSynthPi = 3.14159265358979323846f;

// Regular polygon / star vertices inscribed in the (w, h) box. `points` is the
// number of polygon corners (star spikes); `inner_ratio` < 1 alternates an
// inner radius to form a star (ignored for a plain polygon).
std::string synth_polygon_path(float w, float h, int points, float inner_ratio) {
    if (points < 3) points = 3;
    const float cx = w * 0.5f, cy = h * 0.5f, rx = w * 0.5f, ry = h * 0.5f;
    const bool is_star = inner_ratio > 0.0f && inner_ratio < 1.0f;
    const int verts = is_star ? points * 2 : points;
    const float step = 2.0f * kSynthPi / static_cast<float>(verts);
    const float start = -kSynthPi * 0.5f;  // first point at top
    std::ostringstream d;
    for (int i = 0; i < verts; ++i) {
        const float a = start + step * static_cast<float>(i);
        const float scale = (is_star && (i % 2 == 1)) ? inner_ratio : 1.0f;
        const float x = cx + rx * scale * std::cos(a);
        const float y = cy + ry * scale * std::sin(a);
        d << (i == 0 ? "M" : " L") << svg_num(x) << " " << svg_num(y);
    }
    d << " Z";
    return d.str();
}

bool node_has_visible_fill(const IRNode& n) {
    return (n.style.background_color && !n.style.background_color->empty()) ||
           (n.style.background_gradient && !n.style.background_gradient->empty()) ||
           (n.style.background_image && !n.style.background_image->empty() &&
            *n.style.background_image != "none");
}

void synthesize_node(IRNode& n) {
    if (!is_synthesizable_primitive(n.type)) return;
    // Only step in for the EXACT case codegen would otherwise drop: no authored
    // path, no children to paint, no fill, no rasterized asset, not a widget.
    // Anything already renderable is left exactly as-is (zero behavior change).
    if (n.attributes.count("path_data")) return;
    if (!n.children.empty()) return;
    if (n.audio_widget != AudioWidgetType::none) return;
    if (n.attributes.count("asset_path")) return;
    // "Already renderable" holds for a rect or a line: a frame paints its own
    // background box, so a filled rect needs no path. It does NOT hold for a
    // round or many-sided primitive — codegen has no painter for those, so a
    // filled ellipse emits nothing whatsoever and the shape vanishes. The
    // design's red record dot rendered as an empty cell for exactly this reason
    // while the play triangle beside it (a vector carrying a real path) was
    // fine. Their fill is only expressible as a path, so synthesize one and
    // carry the color across.
    //
    // A gradient rides the path too, via setSvgFillGradient. It used to bail
    // here because a gradient could not move onto the path, and emitting one
    // anyway painted a gradient SQUARE behind the circle — strictly worse than
    // a flat disc. The path can carry it now, so the only rule left is the one
    // that made the square: whatever moves onto the path must be cleared off
    // the node's own background.
    const bool fill_needs_path = n.type == "ellipse" || n.type == "circle" ||
                                 n.type == "polygon" || n.type == "star";
    const bool solid_filled =
        n.style.background_color && !n.style.background_color->empty();
    const bool gradient_filled =
        n.style.background_gradient && !n.style.background_gradient->empty();
    if (node_has_visible_fill(n) && !(fill_needs_path && (solid_filled || gradient_filled)))
        return;
    const float w = n.style.width.value_or(0.0f);
    const float h = n.style.height.value_or(0.0f);
    const bool is_line = (n.type == "line" || n.type == "svg_line");
    // A line spans a diagonal, so one extent may legitimately be 0 (a perfectly
    // horizontal/vertical rule). Every other primitive needs real area.
    if (is_line ? (w <= 0.0f && h <= 0.0f) : (w <= 0.0f || h <= 0.0f)) return;

    std::string d;
    if (n.type == "rect" || n.type == "rectangle" || n.type == "svg_rect")
        d = synth_rect_path(w, h, n.style);
    else if (n.type == "line" || n.type == "svg_line")
        d = synth_line_path(w, h);
    else if (n.type == "ellipse" || n.type == "circle")
        d = synth_ellipse_path(w, h);
    else if (n.type == "polygon")
        d = synth_polygon_path(w, h,
                               static_cast<int>(int_attr(n, {"pointCount", "point_count", "points"}).value_or(3)),
                               /*inner_ratio=*/0.0f);
    else if (n.type == "star")
        d = synth_polygon_path(w, h,
                               static_cast<int>(int_attr(n, {"pointCount", "point_count", "points"}).value_or(5)),
                               float_attr(n, {"innerRadius", "inner_radius", "innerRadiusRatio"}).value_or(0.5f));
    if (d.empty()) return;

    n.attributes["path_data"] = d;
    n.attributes["svg_viewbox"] = "0 0 " + svg_num(w) + " " + svg_num(h);
    // The synthesized path now IS this node's paint, so a solid fill moves onto
    // it and the background is cleared. Leaving the background set would paint a
    // square behind the circle — the stray box that shows up behind a knob.
    if (gradient_filled) {
        n.attributes["svg_fill_gradient"] = *n.style.background_gradient;
        n.style.background_gradient.reset();
        // The solid, when present, follows it onto the path as the widget's
        // parse-failure fallback. Either way the node's own background is
        // cleared: leaving it would paint the box behind the circle.
        n.attributes["svg_fill"] =
            solid_filled ? *n.style.background_color : std::string("none");
        n.style.background_color.reset();
    } else if (solid_filled) {
        n.attributes["svg_fill"] = *n.style.background_color;
        n.style.background_color.reset();
    } else {
        // No visible fill: force the SvgPathWidget's default opaque-black fill
        // off — otherwise a stroke-only or empty shape renders as a black box.
        n.attributes["svg_fill"] = "none";
    }
    if (n.style.border_color && !n.style.border_color->empty()) {
        n.attributes["svg_stroke"] = *n.style.border_color;
        n.attributes["svg_stroke_width"] = svg_num(n.style.border_width.value_or(1.0f));
    } else if (auto sc = n.attributes.find("stroke_color");
               sc != n.attributes.end() && !sc->second.empty()) {
        // Pencil-style shapes record their stroke as a `stroke_color` attribute
        // rather than style.border_color. Consume it (and its width) so a
        // stroked rect/ellipse is not synthesized as an invisible fill-none path.
        n.attributes["svg_stroke"] = sc->second;
        auto sw = n.attributes.find("stroke_width");
        if (sw == n.attributes.end()) sw = n.attributes.find("stroke-width");
        n.attributes["svg_stroke_width"] =
            (sw != n.attributes.end() && !sw->second.empty()) ? sw->second : "1";
    }
}

}  // namespace

void hoist_captured_art_knobs(DesignIR& ir) {
    // Pre-materialization IR normalization: co-located with the other importer
    // passes (enrich_imported_image_asset_metadata, synthesize_primitive_paths)
    // rather than in the materializer TU. See the header for the full contract.
    std::function<void(IRNode&)> visit = [&](IRNode& n) {
        if (n.audio_widget == AudioWidgetType::knob) {
            // Captured layers = asset-backed image children. Area is the layer's
            // own box; a captured disc/body is a substantial box while a pointer
            // hairline is ~0-area (a stroked vector the native notch replaces).
            std::vector<IRNode*> captured;
            IRNode* body = nullptr;
            float body_area = 0.0f;
            for (auto& c : n.children) {
                if (c.type == "image" && c.attributes.count("asset_ref")) {
                    captured.push_back(&c);
                    const float area = c.style.width.value_or(0.0f) *
                                       c.style.height.value_or(0.0f);
                    if (area > body_area) { body_area = area; body = &c; }
                }
            }
            if (body != nullptr) {
                // A secondary layer is SUBSTANTIAL (a real captured layer, not a
                // pointer the notch replaces) when its area is a meaningful
                // fraction of the body disc.
                int substantial = 0;
                for (auto* layer : captured) {
                    if (layer == body) continue;
                    const float area = layer->style.width.value_or(0.0f) *
                                       layer->style.height.value_or(0.0f);
                    if (body_area > 0.0f && area >= 0.4f * body_area) ++substantial;
                }
                if (substantial == 0) {
                    // One body disc (+ only pointer hairlines): hoist the disc so
                    // the materializer skins the knob and draws the design's own
                    // pointer over it. The knob stays interactive AND faithful.
                    n.attributes["asset_ref"] = body->attributes.at("asset_ref");
                    if (body->style.render_bounds && !n.style.render_bounds)
                        n.style.render_bounds = body->style.render_bounds;
                    if (!n.attributes.count("sprite_strip_frame_count"))
                        n.attributes["sprite_strip_frame_count"] = "1";

                    // Capture the design's OWN pointer geometry (the hairline
                    // vector — Figma "Vector 7") before erasing it, so the
                    // renderer reproduces it pivoting at the disc center on the
                    // value arc instead of drawing a guessed synthetic notch.
                    // The hairline = the smallest-area non-body captured layer.
                    // Geometry is recorded as fractions of the disc's half-extent
                    // so it survives the importer's core-fit rescale.
                    const float bx = body->style.left.value_or(0.0f);
                    const float by = body->style.top.value_or(0.0f);
                    const float bw = body->style.width.value_or(0.0f);
                    const float bh = body->style.height.value_or(0.0f);
                    const float dcx = bx + bw * 0.5f;
                    const float dcy = by + bh * 0.5f;
                    const float half = std::min(bw, bh) * 0.5f;
                    // The pointer (Figma "Vector 7") is the thinnest non-body,
                    // non-text child — a hairline. It may still be an asset image,
                    // OR a 1px frame the stroke→fill demotion pass produced (which
                    // is why scanning only asset images missed it and the knob fell
                    // back to the synthetic notch). Capture its geometry + color,
                    // then erase it (we draw our own rotating pointer).
                    IRNode* pointer = nullptr;
                    float pointer_len = std::numeric_limits<float>::max();
                    for (auto& c : n.children) {
                        if (&c == body || c.type == "text") continue;
                        const float w = c.style.width.value_or(-1.0f);
                        const float h = c.style.height.value_or(-1.0f);
                        if (w < 0.0f || h < 0.0f) continue;
                        const float thin = std::min(w, h);  // raw hairline ≈ 0; demoted = 1px
                        const float lng = std::max(w, h);
                        if (thin > 2.5f || lng <= 0.0f) continue;  // need tiny × real
                        if (lng < pointer_len) { pointer_len = lng; pointer = &c; }
                    }
                    if (pointer != nullptr && half > 0.0f) {
                        pointer->attributes["__knob_pointer"] = "1";
                        const float px = pointer->style.left.value_or(0.0f);
                        const float py = pointer->style.top.value_or(0.0f);
                        const float pw = pointer->style.width.value_or(0.0f);
                        const float pyh = pointer->style.height.value_or(0.0f);
                        // The hairline runs along its long axis; its two ends are
                        // the extremes of its box. Measure each end's radius from
                        // the disc center, keep the near/far pair.
                        const float pcx = px + pw * 0.5f;
                        auto rad = [&](float ex, float ey) {
                            const float dx = ex - dcx, dy = ey - dcy;
                            return std::sqrt(dx * dx + dy * dy);
                        };
                        const float r_a = rad(pcx, py);
                        const float r_b = rad(pcx, py + pyh);
                        const float r_out = std::max(r_a, r_b) / half;
                        const float r_in = std::min(r_a, r_b) / half;
                        const float w_frac =
                            pointer->style.border_width.value_or(1.5f) / half;
                        if (r_out > r_in) {
                            n.attributes["knob_ind_r_in"] = std::to_string(r_in);
                            n.attributes["knob_ind_r_out"] = std::to_string(r_out);
                            n.attributes["knob_ind_w"] = std::to_string(w_frac);
                            // The demotion pass moves the stroke to background_color;
                            // un-demoted strokes keep it on border_color.
                            if (pointer->style.border_color)
                                n.attributes["knob_ind_color"] = *pointer->style.border_color;
                            else if (pointer->style.background_color)
                                n.attributes["knob_ind_color"] = *pointer->style.background_color;
                            else if (pointer->style.color)
                                n.attributes["knob_ind_color"] = *pointer->style.color;
                        }
                    }
                    n.children.erase(
                        std::remove_if(n.children.begin(), n.children.end(),
                            [](const IRNode& c) {
                                return (c.type == "image" &&
                                        c.attributes.count("asset_ref") != 0) ||
                                       c.attributes.count("__knob_pointer") != 0;
                            }),
                        n.children.end());
                } else {
                    // Multiple substantial captured layers (body + highlight +
                    // logo …): a single-frame sprite skin can hold only one, so
                    // demote to a plain container — every layer renders as an
                    // image (faithful but not turnable). No silent layer loss.
                    n.audio_widget = AudioWidgetType::none;
                }
            }
        }
        for (auto& c : n.children) visit(c);
    };
    visit(ir.root);
}

namespace {

// Split a CSS `border` shorthand into its parts. Tokenized with paren-depth
// tracking rather than a plain space split, because `rgba(255, 0, 0, 0.5)` is
// ONE value containing spaces and commas — a naive split hands back "rgba(255,"
// as the color and the border silently disappears again, one layer deeper.
//
// The color is carried across verbatim rather than parsed: border_color is a
// string, and every downstream paint site already accepts hex / rgb() / rgba()
// / hsl(). Parsing here would only add a second place to disagree about color.
struct BorderShorthand {
    std::optional<std::string> color;
    std::optional<float> width;
    std::optional<std::string> style;
};

BorderShorthand parse_border_shorthand(const std::string& value) {
    BorderShorthand out;
    static const std::unordered_set<std::string> kStyles = {
        "none", "hidden", "solid", "dashed", "dotted",
        "double", "groove", "ridge", "inset", "outset",
    };

    std::vector<std::string> tokens;
    std::string cur;
    int depth = 0;
    for (char c : value) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (std::isspace(static_cast<unsigned char>(c)) && depth == 0) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) tokens.push_back(cur);

    for (const auto& t : tokens) {
        const std::string lower = choc::text::toLowerCase(t);
        if (!out.style && kStyles.count(lower)) { out.style = lower; continue; }
        // A width is a bare number with an optional unit. Only px is honoured as
        // a real length; other units are left for the caller's default rather
        // than converted with a guessed root font size.
        if (!out.width && !t.empty() &&
            (std::isdigit(static_cast<unsigned char>(t[0])) || t[0] == '.')) {
            char* end = nullptr;
            const float v = std::strtof(t.c_str(), &end);
            if (end != t.c_str()) {
                const std::string unit = choc::text::toLowerCase(std::string(end));
                if (unit.empty() || unit == "px") { out.width = v; continue; }
            }
        }
        if (!out.color) out.color = t;
    }
    return out;
}

}  // namespace

void normalize_border_shorthand(IRNode& root) {
    auto visit = [](auto&& self, IRNode& n) -> void {
        if (n.style.border && !n.style.border->empty()) {
            const auto parts = parse_border_shorthand(*n.style.border);
            // `border: none` / a zero width is a positive statement that there
            // is no edge. Recording color-less parts would leave border_width
            // set with no color, which reads downstream as "no border" anyway —
            // but say it explicitly so the intent survives a later refactor.
            const bool suppressed =
                (parts.style && *parts.style == "none") ||
                (parts.style && *parts.style == "hidden") ||
                (parts.width && *parts.width <= 0.0f);
            if (!suppressed) {
                // Fill gaps only: a producer that set the discrete fields said
                // what it meant more precisely than the shorthand can.
                if (!n.style.border_color && parts.color)
                    n.style.border_color = *parts.color;
                if (!n.style.border_width)
                    n.style.border_width = parts.width.value_or(1.0f);
                if (!n.style.border_style && parts.style)
                    n.style.border_style = *parts.style;
            }
        }
        for (auto& c : n.children) self(self, c);
    };
    visit(visit, root);
}

namespace {

// The resolved box of a slider child in its parent's coordinate space. A child
// only qualifies once it carries all four absolute fields — a flow-positioned
// child has no fixed geometry to reason about, so it never joins the triplet.
struct SliderBox {
    IRNode* node = nullptr;
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool ok = false;
};

SliderBox slider_box(IRNode& c) {
    SliderBox b;
    b.node = &c;
    if (c.style.left && c.style.top && c.style.width && c.style.height) {
        b.x = *c.style.left;
        b.y = *c.style.top;
        b.w = *c.style.width;
        b.h = *c.style.height;
        b.ok = b.w > 0.0f && b.h > 0.0f;
    }
    return b;
}

// A slider thumb reads as a circle. The .fig decoder emits it as an ellipse;
// other producers may emit a square frame rounded to a pill by its radius.
bool slider_thumb_is_round(const IRNode& n, float w, float h) {
    if (n.type == "ellipse" || n.type == "circle") return true;
    const float side = std::min(w, h);
    const float r = n.style.border_top_left_radius.value_or(
        n.style.border_radius.value_or(0.0f));
    return std::abs(w - h) <= 1.0f && r >= 0.4f * side;
}

}  // namespace

void reconnect_slider_fill(IRNode& root) {
    auto visit = [](auto&& self, IRNode& n) -> void {
        // A horizontal slider is a short, wide container: a near-full-width
        // track, a shorter colored progress fill on the same band, and a round
        // thumb whose height fills the bar. Detect that triplet structurally —
        // never by layer name — so only a real slider is ever touched.
        if (n.style.width && n.style.height && *n.style.height > 0.0f &&
            n.children.size() >= 2 && n.children.size() <= 4) {
            const float W = *n.style.width;
            const float H = *n.style.height;
            if (W >= 3.0f * H) {
                SliderBox thumb, track, fill;
                for (auto& c : n.children) {
                    const SliderBox b = slider_box(c);
                    if (!b.ok) continue;
                    const bool thin = b.h <= 0.5f * H;
                    if (!thumb.ok && slider_thumb_is_round(c, b.w, b.h) &&
                        std::abs(b.w - b.h) <= std::max(3.0f, 0.3f * b.w) &&
                        b.h >= 0.7f * H) {
                        thumb = b;
                    } else if (!track.ok && thin && b.w >= 0.85f * W) {
                        track = b;
                    } else if (!fill.ok && thin && b.w < 0.85f * W &&
                               c.style.background_color) {
                        fill = b;
                    }
                }
                // Require a distinct fill: its color must differ from the track,
                // so a plain progress bar's own track is never mistaken for a
                // fill and pulled around.
                const bool distinct_fill =
                    fill.ok &&
                    (!track.node || !track.node->style.background_color ||
                     *fill.node->style.background_color !=
                         *track.node->style.background_color);
                if (thumb.ok && track.ok && distinct_fill) {
                    const float thumb_left = thumb.x;
                    const float thumb_right = thumb.x + thumb.w;
                    const float thumb_center = thumb.x + thumb.w * 0.5f;
                    const float fill_left = fill.x;
                    const float fill_right = fill.x + fill.w;
                    // Only act when the fill floats in a gap away from the thumb.
                    // A fill already touching or overlapping its thumb is stored
                    // faithfully and left exactly as-is.
                    if (fill_left > thumb_right) {
                        // Fill sits to the right of the thumb: pull its left edge
                        // back to the thumb so the bar meets the handle.
                        fill.node->style.left = thumb_center;
                        fill.node->style.width = fill_right - thumb_center;
                    } else if (fill_right < thumb_left) {
                        // Fill sits to the left: push its right edge to the thumb.
                        fill.node->style.width = thumb_center - fill_left;
                    }
                }
            }
        }
        for (auto& c : n.children) self(self, c);
    };
    visit(visit, root);
}

void synthesize_primitive_paths(IRNode& root) {
    synthesize_node(root);
    for (auto& child : root.children)
        synthesize_primitive_paths(child);
}

int apply_param_binding_manifest(
    IRNode& root,
    const std::unordered_map<std::string, std::string>& node_id_to_param_key) {
    if (node_id_to_param_key.empty())
        return 0;
    int applied = 0;
    for (auto& el : root.interactive_elements) {
        if (!el.param_key.empty())         // explicit producer binding (sigil) wins
            continue;
        if (!el.source_node_id)            // needs provenance to key on
            continue;
        auto it = node_id_to_param_key.find(*el.source_node_id);
        if (it != node_id_to_param_key.end() && !it->second.empty()) {
            el.param_key = it->second;
            ++applied;
        }
    }
    for (auto& child : root.children)
        applied += apply_param_binding_manifest(child, node_id_to_param_key);
    return applied;
}

std::optional<std::unordered_map<std::string, std::string>>
parse_param_binding_manifest_json(const std::string& json, std::string* error) {
    choc::value::Value parsed;
    try {
        parsed = choc::json::parse(json);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
    if (!parsed.isObject()) {
        if (error)
            *error = "expected a JSON object mapping figma node id -> param key";
        return std::nullopt;
    }
    std::unordered_map<std::string, std::string> out;
    for (uint32_t i = 0; i < parsed.size(); ++i) {
        auto m = parsed.getObjectMemberAt(i);
        std::string key(m.name);
        if (key.empty() || !m.value.isString())
            continue;  // lenient: skip a blank key or a non-string binding value
        std::string val(m.value.toString());
        if (!val.empty())
            out[std::move(key)] = std::move(val);
    }
    return out;
}

} // namespace pulp::view
