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
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
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
        case DesignSource::jsx:      return "JSX instrument";
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

/// Phase 9: motion provenance vendor key. Matches the `source` token
/// the import CLI accepts, lowercased + slash-friendly so the resulting
/// `source_id` (e.g. "figma:LevelMeter/Panel") is easy to grep through
/// fixtures. Stable across releases — fixtures depend on these strings.
const char* design_source_vendor_key(DesignSource source) {
    switch (source) {
        case DesignSource::figma:    return "figma";
        case DesignSource::stitch:   return "stitch";
        case DesignSource::v0:       return "v0";
        case DesignSource::pencil:   return "pencil";
        case DesignSource::claude:   return "claude";
        case DesignSource::designmd: return "designmd";
        case DesignSource::jsx:      return "jsx";
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
    // source. Per pulp #468 (manual-export framing), users hand the
    // exported HTML over directly — no Anthropic API integration.
    auto ir = parse_stitch_html(html);
    ir.source = DesignSource::claude;
    ir.source_adapter = "claude-design-html";
    // Phase 0a: re-tag provenance after the Stitch parser stamped it.
    // Anchors were already assigned by parse_stitch_html with the
    // content-hash strategy — same strategy claude-design-html uses
    // (see DEFAULT_ANCHOR_STRATEGY in anchors.ts), so we don't reassign.
    // parse_stitch_html always populates provenance on both its JSON and
    // regex-fallback paths, so the optional is always set here.
    if (ir.root.provenance) ir.root.provenance->adapter = "claude-design-html";
    return ir;
}

// ── Claude Design classname extraction (pulp #1035) ─────────────────────
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

// Trim ASCII whitespace from both ends of a string view in place.
std::string trim_ascii_ws(std::string_view sv) {
    size_t i = 0, j = sv.size();
    while (i < j && std::isspace(static_cast<unsigned char>(sv[i]))) ++i;
    while (j > i && std::isspace(static_cast<unsigned char>(sv[j - 1]))) --j;
    return std::string(sv.substr(i, j - i));
}

// Strip CSS `/* ... */` comments from a block. Multi-line safe. We
// don't need to be a full CSS tokenizer — Claude Design exports use
// vanilla rules, not CSS-in-JS or nested at-rules.
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
        auto prop = trim_ascii_ws(std::string_view(decl).substr(0, colon));
        auto value = trim_ascii_ws(std::string_view(decl).substr(colon + 1));
        if (prop.empty() || value.empty()) continue;
        out[css_prop_to_camel_case(prop)] = value;
    }
    return out;
}

// Walk a CSS source string (the inside of one `<style>` block) and
// merge every `.classname { ... }` rule into `into`. Skips at-rules
// (anything that begins with `@`), `:root` blocks, descendant /
// pseudo-class selectors (anything with whitespace, `:`, `>` or `,`
// between the dot and the `{`), and `.scheme-*` selectors (those are
// already handled as theme-mode token overrides upstream).
//
// We hand-walk character-by-character rather than regex because:
//   1. CSS values can contain `{` (e.g. `linear-gradient(...)`) — but
//      not at the top level of a declaration block.
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

        // Find the next `{` that opens a rule body. Anything between
        // `i` and that brace is the selector list.
        auto open = css.find('{', i);
        if (open == std::string::npos) break;
        std::string selector_list = css.substr(i, open - i);

        // Find the matching `}`. Top-level only — declaration values
        // never embed `{...}` braces in well-formed CSS.
        auto close = css.find('}', open + 1);
        if (close == std::string::npos) break;
        std::string body = css.substr(open + 1, close - (open + 1));
        i = close + 1;

        // Skip at-rules (`@media`, `@font-face`, `@keyframes`, etc.).
        // The first non-whitespace char of the selector tells us.
        auto first_non_ws = selector_list.find_first_not_of(" \t\r\n");
        if (first_non_ws == std::string::npos) continue;
        if (selector_list[first_non_ws] == '@') continue;

        // Split selector_list on top-level commas.
        std::vector<std::string> selectors;
        size_t s_start = 0;
        for (size_t k = 0; k <= selector_list.size(); ++k) {
            if (k == selector_list.size() || selector_list[k] == ',') {
                selectors.push_back(trim_ascii_ws(
                    std::string_view(selector_list).substr(s_start, k - s_start)));
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
// modifier_strings_to_mask) moved to design_import_shortcuts.cpp
// in the 2026-05 A3 refactor first cut. See planning/
// 2026-05-16-refactor-ops-high-leverage.md.



// ── Audio widget detection ──────────────────────────────────────────────

static std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return result;
}

AudioWidgetType detect_audio_widget(const std::string& name) {
    auto lower = to_lower(name);
    if (lower.find("knob") != std::string::npos)      return AudioWidgetType::knob;
    if (lower.find("dial") != std::string::npos)       return AudioWidgetType::knob;
    if (lower.find("fader") != std::string::npos)      return AudioWidgetType::fader;
    if (lower.find("slider") != std::string::npos)     return AudioWidgetType::fader;
    if (lower.find("meter") != std::string::npos)      return AudioWidgetType::meter;
    if (lower.find("level") != std::string::npos)      return AudioWidgetType::meter;
    if (lower.find("vu") != std::string::npos)         return AudioWidgetType::meter;
    if (lower.find("xypad") != std::string::npos)      return AudioWidgetType::xy_pad;
    if (lower.find("xy_pad") != std::string::npos)     return AudioWidgetType::xy_pad;
    if (lower.find("xy pad") != std::string::npos)     return AudioWidgetType::xy_pad;
    if (lower.find("waveform") != std::string::npos)   return AudioWidgetType::waveform;
    if (lower.find("oscilloscope") != std::string::npos) return AudioWidgetType::waveform;
    if (lower.find("spectrum") != std::string::npos)   return AudioWidgetType::spectrum;
    if (lower.find("analyzer") != std::string::npos)   return AudioWidgetType::spectrum;
    if (lower.find("analyser") != std::string::npos)   return AudioWidgetType::spectrum;
    return AudioWidgetType::none;
}

AudioWidgetType audio_widget_from_id(const std::string& id) {
    const auto lower = to_lower(id);
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
    value = trim_ascii_ws(value);
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
    ir.asset_manifest = collect_design_ir_assets(ir, options);
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

DesignIR parse_figma_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::figma;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "figma";
    ir.source_version = "1";

    auto root = choc::json::parse(json);

    // Figma export can be the IR format directly (from MCP or export tool)
    ir.root = parse_ir_node(root);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);

    // Phase 0a: tag the root with adapter provenance + confidence, then
    // assign stable_anchor_id values via the adapter strategy. Figma
    // exports carry native layer UUIDs that parse_ir_node populates into
    // source_node_id; the adapter strategy prefixes those with "figma:".
    // Nodes that don't have a native ID (rare in Figma) silently fall
    // through to the content-hash branch inside compute_anchor_id.
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

    for (auto it = begin; it != end; ++it) {
        std::string tag = (*it)[1].str();
        std::string content = (*it)[2].str();
        if (content.empty()) continue;

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

    // Phase 0a: assign anchors to the regex-extracted tree.
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

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);
    if (root.hasObjectMember("variables"))
        ir.tokens = parse_ir_tokens(root["variables"]);

    // Phase 0a: Pencil MCP nodes carry a stable `id` that parse_ir_node
    // populates into source_node_id; the adapter strategy uses it directly.
    ir.root.provenance = IRProvenance{"pencil", "1", {}};
    ir.root.confidence = IRConfidence::pass;
    promote_interactive_frames(ir.root);
    assign_anchors(ir.root, AnchorStrategy::adapter, "pencil");

    return ir;
}


} // namespace pulp::view
