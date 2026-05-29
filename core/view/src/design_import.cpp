#include <pulp/view/design_import.hpp>
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
    if (name == "figma-plugin" || name == "figma_plugin") return DesignSource::figma_plugin;
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

// ── JSON parsing helpers ────────────────────────────────────────────────

static std::string get_string(const choc::value::ValueView& obj, const char* key, const char* def = "") {
    if (obj.hasObjectMember(key))
        return std::string(obj[key].toString());
    return def;
}

static float get_float(const choc::value::ValueView& obj, const char* key, float def = 0.0f) {
    if (obj.hasObjectMember(key))
        return static_cast<float>(obj[key].getWithDefault<double>(def));
    return def;
}

static bool get_bool(const choc::value::ValueView& obj, const char* key, bool def = false) {
    if (obj.hasObjectMember(key))
        return obj[key].getWithDefault<bool>(def);
    return def;
}

// ── IR from JSON ────────────────────────────────────────────────────────

static IRStyle parse_ir_style(const choc::value::ValueView& obj) {
    IRStyle s;
    if (!obj.isObject()) return s;

    auto set_opt_str = [&](const char* key, std::optional<std::string>& field) {
        if (obj.hasObjectMember(key)) field = std::string(obj[key].toString());
    };
    auto set_opt_float = [&](const char* key, std::optional<float>& field) {
        if (obj.hasObjectMember(key)) field = static_cast<float>(obj[key].getWithDefault<double>(0));
    };
    auto set_opt_int = [&](const char* key, std::optional<int>& field) {
        if (obj.hasObjectMember(key)) field = static_cast<int>(obj[key].getWithDefault<int64_t>(0));
    };

    // Snake_case aliases (lib emitted by the Figma plugin lane). Camel-case
    // reads above win when both are present; these only fire when the field
    // is still empty.
    auto alias_str = [&](const char* snake, std::optional<std::string>& field) {
        if (!field && obj.hasObjectMember(snake)) field = std::string(obj[snake].toString());
    };
    auto alias_float = [&](const char* snake, std::optional<float>& field) {
        if (!field && obj.hasObjectMember(snake))
            field = static_cast<float>(obj[snake].getWithDefault<double>(0));
    };
    auto alias_int = [&](const char* snake, std::optional<int>& field) {
        if (!field && obj.hasObjectMember(snake))
            field = static_cast<int>(obj[snake].getWithDefault<int64_t>(0));
    };

    set_opt_str("backgroundColor", s.background_color);
    set_opt_str("backgroundGradient", s.background_gradient);
    set_opt_str("backgroundImage", s.background_image);
    set_opt_str("backgroundRepeat", s.background_repeat);
    set_opt_str("color", s.color);
    set_opt_float("opacity", s.opacity);
    set_opt_float("borderRadius", s.border_radius);
    set_opt_str("border", s.border);
    set_opt_str("borderColor", s.border_color);
    set_opt_float("borderWidth", s.border_width);
    set_opt_str("borderStyle", s.border_style);
    set_opt_str("borderTopColor", s.border_top_color);
    set_opt_str("borderRightColor", s.border_right_color);
    set_opt_str("borderBottomColor", s.border_bottom_color);
    set_opt_str("borderLeftColor", s.border_left_color);
    set_opt_float("borderTopWidth", s.border_top_width);
    set_opt_float("borderRightWidth", s.border_right_width);
    set_opt_float("borderBottomWidth", s.border_bottom_width);
    set_opt_float("borderLeftWidth", s.border_left_width);
    set_opt_float("borderTopLeftRadius", s.border_top_left_radius);
    set_opt_float("borderTopRightRadius", s.border_top_right_radius);
    set_opt_float("borderBottomRightRadius", s.border_bottom_right_radius);
    set_opt_float("borderBottomLeftRadius", s.border_bottom_left_radius);
    set_opt_str("boxShadow", s.box_shadow);
    set_opt_str("filter", s.filter);
    set_opt_str("backdropFilter", s.backdrop_filter);
    set_opt_str("fontFamily", s.font_family);
    set_opt_float("fontSize", s.font_size);
    set_opt_int("fontWeight", s.font_weight);
    set_opt_str("fontStyle", s.font_style);
    set_opt_str("textAlign", s.text_align);
    set_opt_float("letterSpacing", s.letter_spacing);
    set_opt_float("lineHeight", s.line_height);
    set_opt_str("textTransform", s.text_transform);
    set_opt_str("textDecoration", s.text_decoration);
    set_opt_str("whiteSpace", s.white_space);
    set_opt_str("textOverflow", s.text_overflow);
    set_opt_str("overflow", s.overflow);
    set_opt_str("cursor", s.cursor);
    set_opt_str("position", s.position);
    set_opt_float("top", s.top);
    set_opt_float("left", s.left);
    set_opt_float("right", s.right);
    set_opt_float("bottom", s.bottom);
    set_opt_int("zIndex", s.z_index);
    set_opt_str("transform", s.transform);
    set_opt_float("width", s.width);
    set_opt_float("height", s.height);
    set_opt_float("minWidth", s.min_width);
    set_opt_float("minHeight", s.min_height);
    set_opt_float("maxWidth", s.max_width);
    set_opt_float("maxHeight", s.max_height);

    // Snake_case aliases (figma-plugin lane).
    alias_str("background_color", s.background_color);
    alias_str("background_gradient", s.background_gradient);
    alias_str("background_image", s.background_image);
    alias_str("background_repeat", s.background_repeat);
    alias_float("border_radius", s.border_radius);
    alias_str("border_color", s.border_color);
    alias_float("border_width", s.border_width);
    alias_str("border_style", s.border_style);
    alias_str("border_top_color", s.border_top_color);
    alias_str("border_right_color", s.border_right_color);
    alias_str("border_bottom_color", s.border_bottom_color);
    alias_str("border_left_color", s.border_left_color);
    alias_float("border_top_width", s.border_top_width);
    alias_float("border_right_width", s.border_right_width);
    alias_float("border_bottom_width", s.border_bottom_width);
    alias_float("border_left_width", s.border_left_width);
    alias_float("border_top_left_radius", s.border_top_left_radius);
    alias_float("border_top_right_radius", s.border_top_right_radius);
    alias_float("border_bottom_right_radius", s.border_bottom_right_radius);
    alias_float("border_bottom_left_radius", s.border_bottom_left_radius);
    alias_str("box_shadow", s.box_shadow);
    alias_str("backdrop_filter", s.backdrop_filter);
    alias_str("font_family", s.font_family);
    alias_float("font_size", s.font_size);
    alias_int("font_weight", s.font_weight);
    alias_str("font_style", s.font_style);
    alias_str("text_align", s.text_align);
    alias_float("letter_spacing", s.letter_spacing);
    alias_float("line_height", s.line_height);
    alias_str("text_transform", s.text_transform);
    alias_str("text_decoration", s.text_decoration);
    alias_str("white_space", s.white_space);
    alias_str("text_overflow", s.text_overflow);
    alias_int("z_index", s.z_index);
    alias_float("min_width", s.min_width);
    alias_float("min_height", s.min_height);
    alias_float("max_width", s.max_width);
    alias_float("max_height", s.max_height);

    return s;
}

static IRLayout parse_ir_layout(const choc::value::ValueView& obj) {
    IRLayout l;
    if (!obj.isObject()) return l;

    auto dir = get_string(obj, "direction", "column");
    l.direction = (dir == "row") ? LayoutDirection::row : LayoutDirection::column;
    if (obj.hasObjectMember("display")) l.display = get_string(obj, "display");
    l.gap = get_float(obj, "gap");
    if (obj.hasObjectMember("rowGap")) l.row_gap = get_float(obj, "rowGap");
    if (obj.hasObjectMember("columnGap")) l.column_gap = get_float(obj, "columnGap");
    l.wrap = get_bool(obj, "wrap");

    // Padding — support uniform or per-side
    if (obj.hasObjectMember("padding")) {
        auto pad = obj["padding"];
        if (pad.isObject()) {
            // figma-plugin lane: { top, right, bottom, left }
            if (pad.hasObjectMember("top"))    l.padding_top    = static_cast<float>(pad["top"].getWithDefault<double>(0));
            if (pad.hasObjectMember("right"))  l.padding_right  = static_cast<float>(pad["right"].getWithDefault<double>(0));
            if (pad.hasObjectMember("bottom")) l.padding_bottom = static_cast<float>(pad["bottom"].getWithDefault<double>(0));
            if (pad.hasObjectMember("left"))   l.padding_left   = static_cast<float>(pad["left"].getWithDefault<double>(0));
        } else {
            // Legacy uniform-float form (Pencil / older sources).
            float p = get_float(obj, "padding");
            l.padding_top = l.padding_right = l.padding_bottom = l.padding_left = p;
        }
    }
    if (obj.hasObjectMember("paddingTop"))    l.padding_top = get_float(obj, "paddingTop");
    if (obj.hasObjectMember("paddingRight"))  l.padding_right = get_float(obj, "paddingRight");
    if (obj.hasObjectMember("paddingBottom")) l.padding_bottom = get_float(obj, "paddingBottom");
    if (obj.hasObjectMember("paddingLeft"))   l.padding_left = get_float(obj, "paddingLeft");
    if (obj.hasObjectMember("padding_top"))    l.padding_top = get_float(obj, "padding_top");
    if (obj.hasObjectMember("padding_right"))  l.padding_right = get_float(obj, "padding_right");
    if (obj.hasObjectMember("padding_bottom")) l.padding_bottom = get_float(obj, "padding_bottom");
    if (obj.hasObjectMember("padding_left"))   l.padding_left = get_float(obj, "padding_left");
    if (obj.hasObjectMember("marginTop"))     l.margin_top = get_float(obj, "marginTop");
    if (obj.hasObjectMember("marginRight"))   l.margin_right = get_float(obj, "marginRight");
    if (obj.hasObjectMember("marginBottom"))  l.margin_bottom = get_float(obj, "marginBottom");
    if (obj.hasObjectMember("marginLeft"))    l.margin_left = get_float(obj, "marginLeft");
    if (obj.hasObjectMember("margin_top"))     l.margin_top = get_float(obj, "margin_top");
    if (obj.hasObjectMember("margin_right"))   l.margin_right = get_float(obj, "margin_right");
    if (obj.hasObjectMember("margin_bottom"))  l.margin_bottom = get_float(obj, "margin_bottom");
    if (obj.hasObjectMember("margin_left"))    l.margin_left = get_float(obj, "margin_left");

    auto parse_align = [](const std::string& s) -> LayoutAlign {
        if (s == "center")        return LayoutAlign::center;
        if (s == "flex-end" || s == "flex_end" || s == "end")   return LayoutAlign::flex_end;
        if (s == "flex-start" || s == "flex_start" || s == "start") return LayoutAlign::flex_start;
        if (s == "stretch")       return LayoutAlign::stretch;
        if (s == "space-between" || s == "space_between") return LayoutAlign::space_between;
        if (s == "space-around" || s == "space_around")  return LayoutAlign::space_around;
        return LayoutAlign::flex_start;
    };

    if (obj.hasObjectMember("justify"))
        l.justify = parse_align(get_string(obj, "justify"));
    if (obj.hasObjectMember("align"))
        l.align = parse_align(get_string(obj, "align"));
    if (obj.hasObjectMember("alignSelf")) l.align_self = get_string(obj, "alignSelf");
    if (obj.hasObjectMember("alignContent")) l.align_content = get_string(obj, "alignContent");
    if (obj.hasObjectMember("flexGrow")) l.flex_grow = get_float(obj, "flexGrow");
    if (obj.hasObjectMember("flexShrink")) l.flex_shrink = get_float(obj, "flexShrink");
    if (obj.hasObjectMember("flexBasis")) l.flex_basis = get_string(obj, "flexBasis");
    if (obj.hasObjectMember("order"))
        l.order = static_cast<int>(obj["order"].getWithDefault<int64_t>(0));
    if (obj.hasObjectMember("aspectRatio")) l.aspect_ratio = get_float(obj, "aspectRatio");
    if (obj.hasObjectMember("overflowX")) l.overflow_x = get_string(obj, "overflowX");
    if (obj.hasObjectMember("overflowY")) l.overflow_y = get_string(obj, "overflowY");

    auto parse_sizing = [](const std::string& s) -> SizingMode {
        if (s == "hug" || s == "auto") return SizingMode::hug;
        if (s == "fill")               return SizingMode::fill;
        return SizingMode::fixed;
    };

    if (obj.hasObjectMember("widthMode"))
        l.width_mode = parse_sizing(get_string(obj, "widthMode"));
    if (obj.hasObjectMember("heightMode"))
        l.height_mode = parse_sizing(get_string(obj, "heightMode"));

    return l;
}

static std::optional<IRConfidence> parse_confidence(const std::string& value);

static bool is_asset_reference_key(std::string_view key) {
    static constexpr const char* kAssetKeys[] = {
        "src", "href", "image", "imageSrc", "source", "backgroundImage",
        "background-image", "fontUrl", "fontURL", "asset", "url"
    };
    for (const char* asset_key : kAssetKeys) {
        if (key == asset_key) return true;
    }
    if (key.rfind("htmlAsset", 0) == 0) return true;
    return false;
}

static IRNode parse_ir_node(const choc::value::ValueView& obj) {
    IRNode node;
    node.type = get_string(obj, "type", "frame");
    node.name = get_string(obj, "name");
    node.text_content = get_string(obj, "content");

    // Phase 0a (planning/2026-05-18-inspector-direct-manipulation-roadmap.md):
    // capture the source-native ID so the `adapter` anchor strategy can use
    // it as its anchor. Figma + Pencil + Mitosis-style exports all carry an
    // ID under one of these field names; first non-empty wins. Sources
    // without native IDs (Stitch HTML, v0 TSX, Claude HTML) leave this
    // empty and fall through to the content-hash strategy.
    // Stash the figma-plugin envelope's per-node asset_ref into attributes so
    // the codegen can resolve it against asset_manifest at emit time.
    if (obj.hasObjectMember("asset_ref") && obj["asset_ref"].isString()) {
        node.attributes["asset_ref"] = std::string(obj["asset_ref"].toString());
    }

    for (const char* k : {"id", "nodeId", "node_id", "source_node_id", "sourceNodeId", "figma_node_id", "figmaNodeId"}) {
        if (!obj.hasObjectMember(k)) continue;
        auto v = obj[k];
        if (!v.isString()) continue;
        auto s = std::string(v.toString());
        if (s.empty()) continue;
        node.source_node_id = std::move(s);
        break;
    }
    if (obj.hasObjectMember("stable_anchor_id") && obj["stable_anchor_id"].isString()) {
        node.stable_anchor_id = std::string(obj["stable_anchor_id"].toString());
    } else if (obj.hasObjectMember("stableAnchorId") && obj["stableAnchorId"].isString()) {
        node.stable_anchor_id = std::string(obj["stableAnchorId"].toString());
    }
    if (obj.hasObjectMember("anchor_strategy") && obj["anchor_strategy"].isString()) {
        node.anchor_strategy = std::string(obj["anchor_strategy"].toString());
    } else if (obj.hasObjectMember("anchorStrategy") && obj["anchorStrategy"].isString()) {
        node.anchor_strategy = std::string(obj["anchorStrategy"].toString());
    }
    if (obj.hasObjectMember("source_adapter") && obj["source_adapter"].isString()) {
        node.source_adapter = std::string(obj["source_adapter"].toString());
    } else if (obj.hasObjectMember("sourceAdapter") && obj["sourceAdapter"].isString()) {
        node.source_adapter = std::string(obj["sourceAdapter"].toString());
    }
    if (obj.hasObjectMember("source_version") && obj["source_version"].isString()) {
        node.source_version = std::string(obj["source_version"].toString());
    } else if (obj.hasObjectMember("sourceVersion") && obj["sourceVersion"].isString()) {
        node.source_version = std::string(obj["sourceVersion"].toString());
    }
    if (obj.hasObjectMember("provenance") && obj["provenance"].isObject()) {
        auto p = obj["provenance"];
        IRProvenance provenance;
        provenance.adapter = get_string(p, "adapter");
        provenance.version = get_string(p, "version");
        provenance.source_uri = get_string(p, "sourceUri");
        if (provenance.source_uri.empty())
            provenance.source_uri = get_string(p, "source_uri");
        node.provenance = std::move(provenance);
    }
    if (!node.provenance && (node.source_adapter || node.source_version)) {
        node.provenance = IRProvenance{
            node.source_adapter.value_or(""),
            node.source_version.value_or(""),
            {}
        };
    }
    if (obj.hasObjectMember("confidence") && obj["confidence"].isString()) {
        node.confidence = parse_confidence(std::string(obj["confidence"].toString()));
    }
    if (obj.hasObjectMember("raw_source") && obj["raw_source"].isString()) {
        node.raw_source = std::string(obj["raw_source"].toString());
    } else if (obj.hasObjectMember("rawSource") && obj["rawSource"].isString()) {
        node.raw_source = std::string(obj["rawSource"].toString());
    }

    if (obj.hasObjectMember("style"))
        node.style = parse_ir_style(obj["style"]);
    if (obj.hasObjectMember("layout"))
        node.layout = parse_ir_layout(obj["layout"]);
    if (obj.hasObjectMember("attributes") && obj["attributes"].isObject()) {
        auto attrs = obj["attributes"];
        for (uint32_t i = 0; i < attrs.size(); ++i) {
            auto m = attrs.getObjectMemberAt(i);
            node.attributes[std::string(m.name)] = std::string(m.value.toString());
        }
    }
    for (const char* key : {
             "src", "href", "image", "imageSrc", "source", "backgroundImage",
             "background-image", "fontUrl", "fontURL", "asset", "url"
         }) {
        if (!obj.hasObjectMember(key) || !obj[key].isString()) continue;
        auto value = std::string(obj[key].toString());
        if (!value.empty() && node.attributes.find(key) == node.attributes.end())
            node.attributes[key] = std::move(value);
    }

    // Exact layout dimensions from snapshot_layout (injected by import skill)
    if (obj.hasObjectMember("_layoutHeight"))
        node.attributes["_layoutHeight"] = std::to_string(static_cast<int>(get_float(obj, "_layoutHeight", 0)));
    if (obj.hasObjectMember("_layoutWidth"))
        node.attributes["_layoutWidth"] = std::to_string(static_cast<int>(get_float(obj, "_layoutWidth", 0)));

    // Audio widget detection is deferred until after children are parsed
    // (see below) — containers with child frames shouldn't be widgets

    if (obj.hasObjectMember("label"))
        node.audio_label = get_string(obj, "label");
    if (obj.hasObjectMember("min"))
        node.audio_min = get_float(obj, "min", 0.0f);
    if (obj.hasObjectMember("max"))
        node.audio_max = get_float(obj, "max", 1.0f);
    if (obj.hasObjectMember("default"))
        node.audio_default = get_float(obj, "default", 0.5f);

    // Top-level properties (Pencil/Figma format puts these at node level, not in "style")
    // Override style values if they weren't set from the "style" sub-object
    // Handle Pencil sizing modes: fill_container → flex_grow, fit_content → auto
    if (obj.hasObjectMember("width")) {
        auto w_str = get_string(obj, "width", "");
        if (w_str == "fill_container") {
            node.layout.width_mode = SizingMode::fill;
        } else if (w_str == "fit_content" || w_str.find("fit_content") == 0) {
            node.layout.width_mode = SizingMode::hug;
        } else if (!node.style.width) {
            float w = get_float(obj, "width", 0);
            if (w > 0) node.style.width = w;
        }
    }
    if (obj.hasObjectMember("height")) {
        auto h_str = get_string(obj, "height", "");
        if (h_str == "fill_container") {
            node.layout.height_mode = SizingMode::fill;
        } else if (h_str == "fit_content" || h_str.find("fit_content") == 0) {
            node.layout.height_mode = SizingMode::hug;
        } else if (!node.style.height) {
            float h = get_float(obj, "height", 0);
            if (h > 0) node.style.height = h;
        }
    }
    // Top-level fill → backgroundColor
    if (!node.style.background_color && obj.hasObjectMember("fill")) {
        auto fill_val = obj["fill"];
        if (fill_val.isString()) {
            auto fill_str = std::string(fill_val.toString());
            if (!fill_str.empty() && fill_str[0] == '#')
                node.style.background_color = fill_str;
        }
    }
    // Top-level cornerRadius
    if (!node.style.border_radius && obj.hasObjectMember("cornerRadius")) {
        node.style.border_radius = get_float(obj, "cornerRadius", 0);
    }
    // Top-level fontSize, fontWeight, fontFamily for text nodes
    if (node.type == "text" || node.type == "label") {
        if (!node.style.font_size && obj.hasObjectMember("fontSize"))
            node.style.font_size = get_float(obj, "fontSize", 14);
        if (!node.style.font_weight && obj.hasObjectMember("fontWeight")) {
            auto fw = get_string(obj, "fontWeight", "");
            if (!fw.empty()) {
                try { node.style.font_weight = std::stoi(fw); }
                catch (...) { if (fw == "bold") node.style.font_weight = 700; }
            }
        }
        if (!node.style.color && obj.hasObjectMember("fill")) {
            auto fill_val = obj["fill"];
            if (fill_val.isString()) node.style.color = std::string(fill_val.toString());
        }
        if (!node.style.font_family && obj.hasObjectMember("fontFamily"))
            node.style.font_family = get_string(obj, "fontFamily", "");
        // Top-level content → text_content
        if (node.text_content.empty() && obj.hasObjectMember("content"))
            node.text_content = get_string(obj, "content", "");
    }
    // Top-level layout properties (Pencil uses "layout": "vertical"/"horizontal")
    // Pencil frames default to horizontal when no layout is specified
    if (obj.hasObjectMember("layout")) {
        auto layout_val = obj["layout"];
        if (layout_val.isString()) {
            auto dir = std::string(layout_val.toString());
            if (dir == "horizontal") node.layout.direction = LayoutDirection::row;
            else if (dir == "vertical") node.layout.direction = LayoutDirection::column;
            else if (dir == "none") {} // absolute positioning, keep default
        } else if (layout_val.isObject()) {
            // IR format: layout is an object with "direction" key
            // Already parsed above via parse_ir_layout
        }
    } else if (node.type == "frame" &&
               obj.hasObjectMember("children") && obj["children"].isArray() && obj["children"].size() > 0) {
        // Pencil frames default to horizontal (row) when layout is not specified
        node.layout.direction = LayoutDirection::row;
    }
    if (obj.hasObjectMember("gap"))
        node.layout.gap = get_float(obj, "gap", 0);
    if (obj.hasObjectMember("justifyContent")) {
        auto j = get_string(obj, "justifyContent", "");
        if (j == "center") node.layout.justify = LayoutAlign::center;
        else if (j == "space_between" || j == "space-between") node.layout.justify = LayoutAlign::space_between;
        else if (j == "space_around" || j == "space-around") node.layout.justify = LayoutAlign::space_around;
        else if (j == "end" || j == "flex-end") node.layout.justify = LayoutAlign::flex_end;
    }
    if (obj.hasObjectMember("alignItems")) {
        auto a = get_string(obj, "alignItems", "");
        if (a == "center") node.layout.align = LayoutAlign::center;
        else if (a == "end" || a == "flex-end") node.layout.align = LayoutAlign::flex_end;
    }
    // Top-level padding (Pencil uses array: [top, right] or [top, right, bottom, left])
    if (obj.hasObjectMember("padding")) {
        auto pad = obj["padding"];
        if (pad.isArray()) {
            if (pad.size() == 2) {
                float tb = static_cast<float>(pad[0].getWithDefault<double>(0));
                float lr = static_cast<float>(pad[1].getWithDefault<double>(0));
                node.layout.padding_top = node.layout.padding_bottom = tb;
                node.layout.padding_left = node.layout.padding_right = lr;
            } else if (pad.size() == 4) {
                node.layout.padding_top = static_cast<float>(pad[0].getWithDefault<double>(0));
                node.layout.padding_right = static_cast<float>(pad[1].getWithDefault<double>(0));
                node.layout.padding_bottom = static_cast<float>(pad[2].getWithDefault<double>(0));
                node.layout.padding_left = static_cast<float>(pad[3].getWithDefault<double>(0));
            }
        } else {
            float p = get_float(obj, "padding", 0);
            node.layout.padding_top = node.layout.padding_right =
                node.layout.padding_bottom = node.layout.padding_left = p;
        }
    }

    // Children
    if (obj.hasObjectMember("children") && obj["children"].isArray()) {
        auto children = obj["children"];
        for (uint32_t i = 0; i < children.size(); ++i)
            node.children.push_back(parse_ir_node(children[static_cast<int>(i)]));
    }

    // Audio widget detection (deferred until after children are parsed)
    // Rule: a node is an audio widget ONLY if:
    //   1. Its name matches an audio widget pattern (knob, fader, meter, etc.)
    //   2. AND it doesn't have child frames/groups (containers aren't widgets)
    //   A node with only shape children (ellipse/rectangle) + text is a widget.
    //   A node with child frames (like "KnobRow" containing 4 knob frames) is a container.
    {
        auto detected = detect_audio_widget(node.name);
        if (detected == AudioWidgetType::none && !node.type.empty())
            detected = detect_audio_widget(node.type);

        if (detected != AudioWidgetType::none) {
            // Check if this node has child frames — if so, it's a container, not a widget
            bool has_child_containers = false;
            for (auto& child : node.children) {
                if (child.type == "frame" || child.type == "group" ||
                    !child.children.empty()) {
                    has_child_containers = true;
                    break;
                }
            }
            // Only assign widget type if it's a leaf or has only shapes+text
            if (!has_child_containers)
                node.audio_widget = detected;
        }
    }

    // Parse stroke color from shapes (Pencil puts stroke on ellipse/rectangle nodes)
    if (obj.hasObjectMember("stroke")) {
        auto stroke = obj["stroke"];
        if (stroke.isObject() && stroke.hasObjectMember("fill")) {
            auto fill = stroke["fill"];
            if (fill.isString()) {
                auto fill_str = std::string(fill.toString());
                if (!fill_str.empty() && fill_str[0] == '#')
                    node.attributes["stroke_color"] = fill_str;
            }
        }
    }

    // For audio widgets: extract child shape dimensions into attributes
    // The frame's own width/height is for the column container.
    // The child shape's width/height is for the widget itself.
    if (node.audio_widget != AudioWidgetType::none && !node.children.empty()) {
        for (auto& child : node.children) {
            if (child.type == "ellipse" || child.type == "rectangle") {
                if (child.style.width)
                    node.attributes["shape_width"] = std::to_string(static_cast<int>(*child.style.width));
                if (child.style.height)
                    node.attributes["shape_height"] = std::to_string(static_cast<int>(*child.style.height));
                break;
            }
        }
    }

    return node;
}

static IRTokens parse_ir_tokens(const choc::value::ValueView& obj) {
    IRTokens tokens;
    if (!obj.isObject()) return tokens;

    if (obj.hasObjectMember("colors")) {
        auto c = obj["colors"];
        for (uint32_t i = 0; i < c.size(); ++i) {
            auto m = c.getObjectMemberAt(i);
            tokens.colors[std::string(m.name)] = std::string(m.value.toString());
        }
    }
    if (obj.hasObjectMember("dimensions")) {
        auto d = obj["dimensions"];
        for (uint32_t i = 0; i < d.size(); ++i) {
            auto m = d.getObjectMemberAt(i);
            tokens.dimensions[std::string(m.name)] = static_cast<float>(m.value.getWithDefault<double>(0));
        }
    }
    if (obj.hasObjectMember("strings")) {
        auto s = obj["strings"];
        for (uint32_t i = 0; i < s.size(); ++i) {
            auto m = s.getObjectMemberAt(i);
            tokens.strings[std::string(m.name)] = std::string(m.value.toString());
        }
    }
    if (obj.hasObjectMember("sourceIdentity") && obj["sourceIdentity"].isObject()) {
        auto ids = obj["sourceIdentity"];
        for (uint32_t i = 0; i < ids.size(); ++i) {
            auto m = ids.getObjectMemberAt(i);
            if (!m.value.isObject()) continue;
            IRTokenIdentity identity;
            identity.source_id = get_string(m.value, "sourceId");
            identity.source_collection = get_string(m.value, "sourceCollection");
            identity.source_mode = get_string(m.value, "sourceMode");
            identity.source_adapter = get_string(m.value, "sourceAdapter");
            tokens.source_identity[std::string(m.name)] = std::move(identity);
        }
    }
    return tokens;
}

static const char* design_source_id(DesignSource source) {
    switch (source) {
        case DesignSource::figma:    return "figma";
        case DesignSource::stitch:   return "stitch";
        case DesignSource::v0:       return "v0";
        case DesignSource::pencil:   return "pencil";
        case DesignSource::claude:   return "claude";
        case DesignSource::designmd: return "designmd";
        case DesignSource::jsx:      return "jsx";
    }
    return "figma";
}

static const char* confidence_id(IRConfidence confidence) {
    switch (confidence) {
        case IRConfidence::pass:     return "pass";
        case IRConfidence::diverge:  return "diverge";
        case IRConfidence::not_impl: return "not_impl";
    }
    return "pass";
}

static std::optional<IRConfidence> parse_confidence(const std::string& value) {
    if (value == "pass") return IRConfidence::pass;
    if (value == "diverge") return IRConfidence::diverge;
    if (value == "not_impl") return IRConfidence::not_impl;
    return std::nullopt;
}

static const char* diagnostic_severity_id(ImportDiagnosticSeverity severity) {
    switch (severity) {
        case ImportDiagnosticSeverity::info:    return "info";
        case ImportDiagnosticSeverity::warning: return "warning";
        case ImportDiagnosticSeverity::error:   return "error";
    }
    return "warning";
}

static ImportDiagnosticSeverity parse_diagnostic_severity(const std::string& value) {
    if (value == "info") return ImportDiagnosticSeverity::info;
    if (value == "error") return ImportDiagnosticSeverity::error;
    return ImportDiagnosticSeverity::warning;
}

static const char* diagnostic_kind_id(ImportDiagnosticKind kind) {
    switch (kind) {
        case ImportDiagnosticKind::unknown:                    return "unknown";
        case ImportDiagnosticKind::unsupported_property:       return "unsupported_property";
        case ImportDiagnosticKind::unresolved_asset:           return "unresolved_asset";
        case ImportDiagnosticKind::snapshot_semantics_warning: return "snapshot_semantics_warning";
        case ImportDiagnosticKind::legacy_field_shortcut:      return "legacy_field_shortcut";
        case ImportDiagnosticKind::capture_partial:            return "capture_partial";
        case ImportDiagnosticKind::fallback_used:              return "fallback_used";
    }
    return "unknown";
}

static ImportDiagnosticKind parse_diagnostic_kind(const std::string& value) {
    if (value == "unsupported_property") return ImportDiagnosticKind::unsupported_property;
    if (value == "unresolved_asset") return ImportDiagnosticKind::unresolved_asset;
    if (value == "snapshot_semantics_warning") return ImportDiagnosticKind::snapshot_semantics_warning;
    if (value == "legacy_field_shortcut") return ImportDiagnosticKind::legacy_field_shortcut;
    if (value == "capture_partial") return ImportDiagnosticKind::capture_partial;
    if (value == "fallback_used") return ImportDiagnosticKind::fallback_used;
    return ImportDiagnosticKind::unknown;
}

static ImportDiagnosticKind diagnostic_kind_from_code(const std::string& code) {
    if (code == "asset-unresolved"
        || code == "asset-network-fetch-disabled"
        || code == "asset-fetcher-missing"
        || code == "asset-fetch-failed"
        || code == "asset-fetch-timeout"
        || code == "asset-empty"
        || code == "asset-data-uri-invalid"
        || code == "asset-hash-mismatch") {
        return ImportDiagnosticKind::unresolved_asset;
    }
    if (code == "snapshot-dynamic-api") {
        return ImportDiagnosticKind::snapshot_semantics_warning;
    }
    if (code == "legacy-ir") {
        return ImportDiagnosticKind::legacy_field_shortcut;
    }
    if (code == "fallback-used" || code == "runtime-fallback") {
        return ImportDiagnosticKind::fallback_used;
    }
    return ImportDiagnosticKind::unknown;
}

static ImportDiagnostic make_import_diagnostic(ImportDiagnosticSeverity severity,
                                               std::string code,
                                               std::string path,
                                               std::string message,
                                               ImportDiagnosticKind kind = ImportDiagnosticKind::unknown) {
    ImportDiagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.code = std::move(code);
    diagnostic.path = std::move(path);
    diagnostic.message = std::move(message);
    diagnostic.kind = kind == ImportDiagnosticKind::unknown
        ? diagnostic_kind_from_code(diagnostic.code)
        : kind;
    return diagnostic;
}

static std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c) << std::dec;
                } else {
                    out << static_cast<char>(c);
                }
                break;
        }
    }
    return out.str();
}

static void write_key(std::ostringstream& out, bool& first, const char* key) {
    if (!first) out << ',';
    first = false;
    out << '"' << key << "\":";
}

static void write_string_member(std::ostringstream& out, bool& first,
                                const char* key, std::string_view value) {
    write_key(out, first, key);
    out << '"' << json_escape(value) << '"';
}

static void write_string_member(std::ostringstream& out, bool& first,
                                const char* key, const char* value) {
    write_string_member(out, first, key, std::string_view(value ? value : ""));
}

static void write_string_member(std::ostringstream& out, bool& first,
                                const char* key, const std::string& value) {
    write_string_member(out, first, key, std::string_view(value));
}

static void write_string_member(std::ostringstream& out, bool& first,
                                const char* key,
                                const std::optional<std::string>& value) {
    if (value) write_string_member(out, first, key, *value);
}

static void write_float_member(std::ostringstream& out, bool& first,
                               const char* key, float value) {
    write_key(out, first, key);
    out << value;
}

static void write_float_member(std::ostringstream& out, bool& first,
                               const char* key, const std::optional<float>& value) {
    if (value) write_float_member(out, first, key, *value);
}

static void write_int_member(std::ostringstream& out, bool& first,
                             const char* key, int value) {
    write_key(out, first, key);
    out << value;
}

static void write_int_member(std::ostringstream& out, bool& first,
                             const char* key, const std::optional<int>& value) {
    if (value) write_int_member(out, first, key, *value);
}

template <typename Map, typename Fn>
static void for_each_sorted_map_entry(const Map& map, Fn&& fn) {
    std::vector<typename Map::key_type> keys;
    keys.reserve(map.size());
    for (const auto& [key, _] : map) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    for (const auto& key : keys) fn(key, map.at(key));
}

static const char* layout_direction_id(LayoutDirection direction) {
    return direction == LayoutDirection::row ? "row" : "column";
}

static const char* layout_align_id(LayoutAlign align) {
    switch (align) {
        case LayoutAlign::flex_start:    return "flex-start";
        case LayoutAlign::flex_end:      return "flex-end";
        case LayoutAlign::center:        return "center";
        case LayoutAlign::stretch:       return "stretch";
        case LayoutAlign::space_between: return "space-between";
        case LayoutAlign::space_around:  return "space-around";
    }
    return "flex-start";
}

static const char* sizing_mode_id(SizingMode mode) {
    switch (mode) {
        case SizingMode::fixed: return "fixed";
        case SizingMode::hug:   return "hug";
        case SizingMode::fill:  return "fill";
    }
    return "fixed";
}

static const char* audio_widget_id(AudioWidgetType type) {
    switch (type) {
        case AudioWidgetType::none:     return "none";
        case AudioWidgetType::knob:     return "knob";
        case AudioWidgetType::fader:    return "fader";
        case AudioWidgetType::meter:    return "meter";
        case AudioWidgetType::xy_pad:   return "xy_pad";
        case AudioWidgetType::waveform: return "waveform";
        case AudioWidgetType::spectrum: return "spectrum";
    }
    return "none";
}

static void write_ir_style_json(std::ostringstream& out, const IRStyle& s) {
    out << '{';
    bool first = true;
    write_string_member(out, first, "backgroundColor", s.background_color);
    write_string_member(out, first, "backgroundGradient", s.background_gradient);
    write_string_member(out, first, "backgroundImage", s.background_image);
    write_string_member(out, first, "backgroundRepeat", s.background_repeat);
    write_string_member(out, first, "color", s.color);
    write_float_member(out, first, "opacity", s.opacity);
    write_float_member(out, first, "borderRadius", s.border_radius);
    write_string_member(out, first, "border", s.border);
    write_string_member(out, first, "borderColor", s.border_color);
    write_float_member(out, first, "borderWidth", s.border_width);
    write_string_member(out, first, "borderStyle", s.border_style);
    write_string_member(out, first, "borderTopColor", s.border_top_color);
    write_string_member(out, first, "borderRightColor", s.border_right_color);
    write_string_member(out, first, "borderBottomColor", s.border_bottom_color);
    write_string_member(out, first, "borderLeftColor", s.border_left_color);
    write_float_member(out, first, "borderTopWidth", s.border_top_width);
    write_float_member(out, first, "borderRightWidth", s.border_right_width);
    write_float_member(out, first, "borderBottomWidth", s.border_bottom_width);
    write_float_member(out, first, "borderLeftWidth", s.border_left_width);
    write_float_member(out, first, "borderTopLeftRadius", s.border_top_left_radius);
    write_float_member(out, first, "borderTopRightRadius", s.border_top_right_radius);
    write_float_member(out, first, "borderBottomRightRadius", s.border_bottom_right_radius);
    write_float_member(out, first, "borderBottomLeftRadius", s.border_bottom_left_radius);
    write_string_member(out, first, "boxShadow", s.box_shadow);
    write_string_member(out, first, "filter", s.filter);
    write_string_member(out, first, "backdropFilter", s.backdrop_filter);
    write_string_member(out, first, "fontFamily", s.font_family);
    write_float_member(out, first, "fontSize", s.font_size);
    write_int_member(out, first, "fontWeight", s.font_weight);
    write_string_member(out, first, "fontStyle", s.font_style);
    write_string_member(out, first, "textAlign", s.text_align);
    write_float_member(out, first, "letterSpacing", s.letter_spacing);
    write_float_member(out, first, "lineHeight", s.line_height);
    write_string_member(out, first, "textTransform", s.text_transform);
    write_string_member(out, first, "textDecoration", s.text_decoration);
    write_string_member(out, first, "whiteSpace", s.white_space);
    write_string_member(out, first, "textOverflow", s.text_overflow);
    write_string_member(out, first, "overflow", s.overflow);
    write_string_member(out, first, "cursor", s.cursor);
    write_string_member(out, first, "position", s.position);
    write_float_member(out, first, "top", s.top);
    write_float_member(out, first, "left", s.left);
    write_float_member(out, first, "right", s.right);
    write_float_member(out, first, "bottom", s.bottom);
    write_int_member(out, first, "zIndex", s.z_index);
    write_string_member(out, first, "transform", s.transform);
    write_float_member(out, first, "width", s.width);
    write_float_member(out, first, "height", s.height);
    write_float_member(out, first, "minWidth", s.min_width);
    write_float_member(out, first, "minHeight", s.min_height);
    write_float_member(out, first, "maxWidth", s.max_width);
    write_float_member(out, first, "maxHeight", s.max_height);
    out << '}';
}

static void write_ir_layout_json(std::ostringstream& out, const IRLayout& l) {
    out << '{';
    bool first = true;
    write_string_member(out, first, "display", l.display);
    write_string_member(out, first, "direction", layout_direction_id(l.direction));
    write_float_member(out, first, "gap", l.gap);
    write_float_member(out, first, "rowGap", l.row_gap);
    write_float_member(out, first, "columnGap", l.column_gap);
    write_float_member(out, first, "paddingTop", l.padding_top);
    write_float_member(out, first, "paddingRight", l.padding_right);
    write_float_member(out, first, "paddingBottom", l.padding_bottom);
    write_float_member(out, first, "paddingLeft", l.padding_left);
    write_float_member(out, first, "marginTop", l.margin_top);
    write_float_member(out, first, "marginRight", l.margin_right);
    write_float_member(out, first, "marginBottom", l.margin_bottom);
    write_float_member(out, first, "marginLeft", l.margin_left);
    write_string_member(out, first, "justify", layout_align_id(l.justify));
    write_string_member(out, first, "align", layout_align_id(l.align));
    write_string_member(out, first, "alignSelf", l.align_self);
    write_string_member(out, first, "alignContent", l.align_content);
    write_key(out, first, "wrap");
    out << (l.wrap ? "true" : "false");
    write_float_member(out, first, "flexGrow", l.flex_grow);
    write_float_member(out, first, "flexShrink", l.flex_shrink);
    write_string_member(out, first, "flexBasis", l.flex_basis);
    write_int_member(out, first, "order", l.order);
    write_float_member(out, first, "aspectRatio", l.aspect_ratio);
    write_string_member(out, first, "overflowX", l.overflow_x);
    write_string_member(out, first, "overflowY", l.overflow_y);
    write_string_member(out, first, "widthMode", sizing_mode_id(l.width_mode));
    write_string_member(out, first, "heightMode", sizing_mode_id(l.height_mode));
    out << '}';
}

static void write_ir_node_json(std::ostringstream& out, const IRNode& node,
                               bool include_source_metadata) {
    out << '{';
    bool first = true;
    write_string_member(out, first, "type", node.type);
    write_string_member(out, first, "name", node.name);
    if (!node.text_content.empty())
        write_string_member(out, first, "content", node.text_content);

    write_key(out, first, "style");
    write_ir_style_json(out, node.style);
    write_key(out, first, "layout");
    write_ir_layout_json(out, node.layout);

    if (node.audio_widget != AudioWidgetType::none)
        write_string_member(out, first, "audioWidget", audio_widget_id(node.audio_widget));
    if (!node.audio_label.empty())
        write_string_member(out, first, "label", node.audio_label);
    if (node.audio_widget != AudioWidgetType::none) {
        write_float_member(out, first, "min", node.audio_min);
        write_float_member(out, first, "max", node.audio_max);
        write_float_member(out, first, "default", node.audio_default);
    }

    if (!node.attributes.empty()) {
        write_key(out, first, "attributes");
        out << '{';
        bool attr_first = true;
        for_each_sorted_map_entry(node.attributes, [&](const auto& key, const auto& value) {
            write_string_member(out, attr_first, key.c_str(), value);
        });
        out << '}';
    }

    if (include_source_metadata) {
        write_string_member(out, first, "stable_anchor_id", node.stable_anchor_id);
        write_string_member(out, first, "anchor_strategy", node.anchor_strategy);
        write_string_member(out, first, "source_node_id", node.source_node_id);
        if (node.source_adapter) write_string_member(out, first, "source_adapter", *node.source_adapter);
        else if (node.provenance) write_string_member(out, first, "source_adapter", node.provenance->adapter);
        if (node.source_version) write_string_member(out, first, "source_version", *node.source_version);
        else if (node.provenance) write_string_member(out, first, "source_version", node.provenance->version);
        if (node.confidence)
            write_string_member(out, first, "confidence", confidence_id(*node.confidence));
        write_string_member(out, first, "raw_source", node.raw_source);
    }

    write_key(out, first, "children");
    out << '[';
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (i) out << ',';
        write_ir_node_json(out, node.children[i], include_source_metadata);
    }
    out << ']';
    out << '}';
}

static void write_tokens_json(std::ostringstream& out, const IRTokens& tokens) {
    out << '{';
    bool first = true;
    write_key(out, first, "colors");
    out << '{';
    bool colors_first = true;
    for_each_sorted_map_entry(tokens.colors, [&](const auto& key, const auto& value) {
        write_string_member(out, colors_first, key.c_str(), value);
    });
    out << '}';
    write_key(out, first, "dimensions");
    out << '{';
    bool dims_first = true;
    for_each_sorted_map_entry(tokens.dimensions, [&](const auto& key, const auto& value) {
        write_float_member(out, dims_first, key.c_str(), value);
    });
    out << '}';
    write_key(out, first, "strings");
    out << '{';
    bool strings_first = true;
    for_each_sorted_map_entry(tokens.strings, [&](const auto& key, const auto& value) {
        write_string_member(out, strings_first, key.c_str(), value);
    });
    out << '}';
    if (!tokens.source_identity.empty()) {
        write_key(out, first, "sourceIdentity");
        out << '{';
        bool ids_first = true;
        for_each_sorted_map_entry(tokens.source_identity, [&](const auto& key, const auto& identity) {
            write_key(out, ids_first, key.c_str());
            out << '{';
            bool id_first = true;
            write_string_member(out, id_first, "sourceId", identity.source_id);
            write_string_member(out, id_first, "sourceCollection", identity.source_collection);
            write_string_member(out, id_first, "sourceMode", identity.source_mode);
            write_string_member(out, id_first, "sourceAdapter", identity.source_adapter);
            out << '}';
        });
        out << '}';
    }
    out << '}';
}

static void write_diagnostic_json(std::ostringstream& out, const ImportDiagnostic& diagnostic) {
    out << '{';
    bool first = true;
    write_string_member(out, first, "severity", diagnostic_severity_id(diagnostic.severity));
    const auto kind = diagnostic.kind == ImportDiagnosticKind::unknown
        ? diagnostic_kind_from_code(diagnostic.code)
        : diagnostic.kind;
    write_string_member(out, first, "kind", diagnostic_kind_id(kind));
    write_string_member(out, first, "code", diagnostic.code);
    write_string_member(out, first, "path", diagnostic.path);
    write_string_member(out, first, "message", diagnostic.message);
    write_string_member(out, first, "anchor_id", diagnostic.anchor_id);
    write_string_member(out, first, "property", diagnostic.property);
    out << '}';
}

static void write_diagnostics_array_json(std::ostringstream& out,
                                         const std::vector<ImportDiagnostic>& diagnostics) {
    out << '[';
    for (size_t i = 0; i < diagnostics.size(); ++i) {
        if (i) out << ',';
        write_diagnostic_json(out, diagnostics[i]);
    }
    out << ']';
}

static void write_asset_manifest_json(std::ostringstream& out, const IRAssetManifest& manifest) {
    out << '{';
    bool first = true;
    write_int_member(out, first, "version", manifest.version);
    write_key(out, first, "assets");
    out << '[';
    for (size_t i = 0; i < manifest.assets.size(); ++i) {
        if (i) out << ',';
        const auto& asset = manifest.assets[i];
        out << '{';
        bool afirst = true;
        write_string_member(out, afirst, "asset_id", asset.asset_id);
        write_string_member(out, afirst, "original_uri", asset.original_uri);
        if (!asset.original_uri_aliases.empty()) {
            write_key(out, afirst, "original_uri_aliases");
            out << '[';
            for (size_t j = 0; j < asset.original_uri_aliases.size(); ++j) {
                if (j) out << ',';
                out << '"' << json_escape(asset.original_uri_aliases[j]) << '"';
            }
            out << ']';
        }
        write_string_member(out, afirst, "local_path", asset.local_path);
        write_string_member(out, afirst, "content_hash", asset.content_hash);
        write_string_member(out, afirst, "mime", asset.mime);
        write_int_member(out, afirst, "width", asset.width);
        write_int_member(out, afirst, "height", asset.height);
        write_string_member(out, afirst, "font_family", asset.font_family);
        write_string_member(out, afirst, "license", asset.license);
        write_string_member(out, afirst, "source_url", asset.source_url);
        write_key(out, afirst, "diagnostics");
        write_diagnostics_array_json(out, asset.diagnostics);
        out << '}';
    }
    out << ']';
    out << '}';
}

static ImportDiagnostic parse_import_diagnostic(const choc::value::ValueView& obj) {
    ImportDiagnostic diagnostic;
    if (!obj.isObject()) return diagnostic;
    diagnostic.severity = parse_diagnostic_severity(get_string(obj, "severity", "warning"));
    diagnostic.code = get_string(obj, "code");
    diagnostic.path = get_string(obj, "path");
    diagnostic.message = get_string(obj, "message");
    diagnostic.kind = parse_diagnostic_kind(get_string(obj, "kind"));
    if (diagnostic.kind == ImportDiagnosticKind::unknown)
        diagnostic.kind = diagnostic_kind_from_code(diagnostic.code);
    auto anchor = get_string(obj, "anchor_id");
    if (anchor.empty()) anchor = get_string(obj, "anchorId");
    if (!anchor.empty()) diagnostic.anchor_id = anchor;
    auto property = get_string(obj, "property");
    if (!property.empty()) diagnostic.property = property;
    return diagnostic;
}

static std::vector<ImportDiagnostic> parse_import_diagnostics(const choc::value::ValueView& obj) {
    std::vector<ImportDiagnostic> diagnostics;
    if (!obj.isArray()) return diagnostics;
    for (uint32_t i = 0; i < obj.size(); ++i) {
        diagnostics.push_back(parse_import_diagnostic(obj[static_cast<int>(i)]));
    }
    return diagnostics;
}

static IRAssetManifest parse_asset_manifest(const choc::value::ValueView& obj) {
    IRAssetManifest manifest;
    if (!obj.isObject()) return manifest;
    if (obj.hasObjectMember("version"))
        manifest.version = static_cast<int>(obj["version"].getWithDefault<int64_t>(1));
    if (!obj.hasObjectMember("assets") || !obj["assets"].isArray()) return manifest;
    auto assets = obj["assets"];
    for (uint32_t i = 0; i < assets.size(); ++i) {
        auto entry = assets[static_cast<int>(i)];
        if (!entry.isObject()) continue;
        IRAssetRef asset;
        asset.asset_id = get_string(entry, "asset_id");
        if (asset.asset_id.empty()) asset.asset_id = get_string(entry, "assetId");
        asset.original_uri = get_string(entry, "original_uri");
        if (asset.original_uri.empty()) asset.original_uri = get_string(entry, "originalUri");
        auto parse_uri_aliases = [&](const char* key) {
            if (!entry.hasObjectMember(key) || !entry[key].isArray()) return;
            auto aliases = entry[key];
            for (uint32_t j = 0; j < aliases.size(); ++j) {
                auto alias = aliases[static_cast<int>(j)];
                if (!alias.isString()) continue;
                auto text = std::string(alias.toString());
                if (text.empty() || text == asset.original_uri) continue;
                if (std::find(asset.original_uri_aliases.begin(),
                              asset.original_uri_aliases.end(),
                              text) == asset.original_uri_aliases.end()) {
                    asset.original_uri_aliases.push_back(std::move(text));
                }
            }
        };
        parse_uri_aliases("original_uri_aliases");
        parse_uri_aliases("originalUriAliases");
        auto local = get_string(entry, "local_path");
        if (local.empty()) local = get_string(entry, "localPath");
        if (!local.empty()) asset.local_path = local;
        asset.content_hash = get_string(entry, "content_hash");
        if (asset.content_hash.empty()) asset.content_hash = get_string(entry, "contentHash");
        asset.mime = get_string(entry, "mime");
        if (entry.hasObjectMember("width"))
            asset.width = static_cast<int>(entry["width"].getWithDefault<int64_t>(0));
        if (entry.hasObjectMember("height"))
            asset.height = static_cast<int>(entry["height"].getWithDefault<int64_t>(0));
        auto family = get_string(entry, "font_family");
        if (family.empty()) family = get_string(entry, "fontFamily");
        if (!family.empty()) asset.font_family = family;
        auto license = get_string(entry, "license");
        if (!license.empty()) asset.license = license;
        auto source_url = get_string(entry, "source_url");
        if (source_url.empty()) source_url = get_string(entry, "sourceUrl");
        if (!source_url.empty()) asset.source_url = source_url;
        if (entry.hasObjectMember("diagnostics") && entry["diagnostics"].isArray()) {
            auto diags = entry["diagnostics"];
            for (uint32_t j = 0; j < diags.size(); ++j)
                asset.diagnostics.push_back(parse_import_diagnostic(diags[static_cast<int>(j)]));
        }
        manifest.assets.push_back(std::move(asset));
    }
    return manifest;
}

std::string serialize_design_ir(const DesignIR& ir,
                                const DesignIrJsonOptions& options) {
    std::ostringstream out;
    out << '{';
    bool first = true;
    write_int_member(out, first, "version", options.version > 0 ? options.version : ir.version);
    write_string_member(out, first, "source", design_source_id(ir.source));
    if (!ir.source_file.empty()) write_string_member(out, first, "sourceFile", ir.source_file);
    write_string_member(out, first, "capture_method", ir.capture_method);
    if (ir.settle_rounds > 0) write_int_member(out, first, "settle_rounds", ir.settle_rounds);
    write_string_member(out, first, "fallback_reason", ir.fallback_reason);
    write_string_member(out, first, "source_adapter", ir.source_adapter);
    write_string_member(out, first, "source_version", ir.source_version);
    write_string_member(out, first, "imported_at", ir.imported_at);
    write_key(out, first, "root");
    write_ir_node_json(out, ir.root, options.include_source_metadata);
    if (options.include_tokens) {
        write_key(out, first, "tokens");
        write_tokens_json(out, ir.tokens);
    }
    if (options.include_asset_manifest) {
        write_key(out, first, "assetManifest");
        write_asset_manifest_json(out, ir.asset_manifest);
    }
    if (!ir.diagnostics.empty()) {
        write_key(out, first, "diagnostics");
        write_diagnostics_array_json(out, ir.diagnostics);
    }
    out << '}';
    return out.str();
}

DesignIR parse_design_ir_json(const std::string& json) {
    auto parsed = choc::json::parse(json);
    DesignIR ir;

    if (parsed.isObject() && parsed.hasObjectMember("version") && parsed.hasObjectMember("root")) {
        ir.version = static_cast<int>(parsed["version"].getWithDefault<int64_t>(1));
        if (parsed.hasObjectMember("source") && parsed["source"].isString()) {
            if (auto source = parse_design_source(std::string(parsed["source"].toString()))) {
                ir.source = *source;
            }
        }
        ir.source_file = get_string(parsed, "sourceFile");
        ir.capture_method = get_string(parsed, "capture_method");
        if (ir.capture_method.empty()) ir.capture_method = get_string(parsed, "captureMethod");
        if (parsed.hasObjectMember("settle_rounds"))
            ir.settle_rounds = static_cast<int>(parsed["settle_rounds"].getWithDefault<int64_t>(0));
        else if (parsed.hasObjectMember("settleRounds"))
            ir.settle_rounds = static_cast<int>(parsed["settleRounds"].getWithDefault<int64_t>(0));
        ir.fallback_reason = get_string(parsed, "fallback_reason");
        if (ir.fallback_reason.empty()) ir.fallback_reason = get_string(parsed, "fallbackReason");
        ir.source_adapter = get_string(parsed, "source_adapter");
        if (ir.source_adapter.empty()) ir.source_adapter = get_string(parsed, "sourceAdapter");
        ir.source_version = get_string(parsed, "source_version");
        if (ir.source_version.empty()) ir.source_version = get_string(parsed, "sourceVersion");
        ir.imported_at = get_string(parsed, "imported_at");
        if (ir.imported_at.empty()) ir.imported_at = get_string(parsed, "importedAt");
        ir.root = parse_ir_node(parsed["root"]);
        if (parsed.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(parsed["tokens"]);
        if (parsed.hasObjectMember("assetManifest"))
            ir.asset_manifest = parse_asset_manifest(parsed["assetManifest"]);
        if (parsed.hasObjectMember("diagnostics"))
            ir.diagnostics = parse_import_diagnostics(parsed["diagnostics"]);
        promote_interactive_frames(ir.root);
        return ir;
    }

    ir.root = parse_ir_node(parsed);
    if (parsed.isObject() && parsed.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(parsed["tokens"]);
    ir.capture_method = "adapter_parse";
    ir.diagnostics.push_back(make_import_diagnostic(
        ImportDiagnosticSeverity::info,
        "legacy-ir",
        "<root>",
        "parsed legacy bare-node DesignIR JSON",
        ImportDiagnosticKind::legacy_field_shortcut));
    promote_interactive_frames(ir.root);
    return ir;
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

DesignIR parse_figma_plugin_json(const std::string& json) {
    // Envelope shape (planning/2026-05-28-pulp-figma-plugin-strategy.md §7.2):
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

    if (parsed.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(parsed["tokens"]);

    if (parsed.hasObjectMember("asset_manifest"))
        ir.asset_manifest = parse_asset_manifest(parsed["asset_manifest"]);
    else if (parsed.hasObjectMember("assetManifest"))
        ir.asset_manifest = parse_asset_manifest(parsed["assetManifest"]);

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

DesignIR parse_v0_tsx(const std::string& tsx) {
    DesignIR ir;
    ir.source = DesignSource::v0;
    ir.capture_method = "adapter_parse";
    ir.source_adapter = "v0-tsx";
    ir.source_version = "1";

    // Try parsing as JSON IR first (pre-processed by AI pipeline)
    try {
        auto root = choc::json::parse(tsx);
        ir.root = parse_ir_node(root);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
        ir.root.provenance = IRProvenance{"v0-tsx", "1", {}};
        ir.root.confidence = IRConfidence::pass;
        promote_interactive_frames(ir.root);
        assign_anchors(ir.root, AnchorStrategy::content_hash);
        return ir;
    } catch (...) {
        // Not JSON — TSX source
    }

    // TSX → IR: extract JSX elements and Tailwind classes
    // Full translation requires AI; this provides structural extraction
    ir.root.type = "frame";
    ir.root.name = "V0Import";
    ir.root.layout.direction = LayoutDirection::column;

    // Extract className strings for Tailwind analysis
    std::regex class_re("className=\"([^\"]+)\"");
    auto begin = std::sregex_iterator(tsx.begin(), tsx.end(), class_re);
    auto end = std::sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        std::string classes = (*it)[1].str();
        IRNode child;
        child.type = "frame";
        child.name = classes;  // Store Tailwind classes as name for AI processing
        // Parse common Tailwind patterns
        if (classes.find("flex-row") != std::string::npos)
            child.layout.direction = LayoutDirection::row;
        if (classes.find("flex-col") != std::string::npos)
            child.layout.direction = LayoutDirection::column;
        ir.root.children.push_back(std::move(child));
    }

    // Phase 0a: anchor the regex-extracted Tailwind tree.
    ir.root.provenance = IRProvenance{"v0-tsx", "1", {}};
    ir.root.confidence = IRConfidence::diverge;  // regex extraction is lossy
    ir.fallback_reason = "input was not JSON; used regex TSX class extraction";
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
