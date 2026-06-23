// design_ir_json.cpp — DesignIR JSON serialize / deserialize band.
//
// JSON helpers, serialize_design_ir, and parse_design_ir_json live here. Four
// helpers (parse_ir_node, parse_ir_tokens, make_import_diagnostic,
// is_asset_reference_key) have external linkage because the asset pipeline /
// source parsers in design_import.cpp call them; their declarations live in
// design_import_internal.hpp. promote_interactive_frames stays defined in
// design_import.cpp and is declared in that same header.

#include <pulp/view/design_import.hpp>

#include <pulp/runtime/log.hpp>

#include "design_binding_metadata.hpp"
#include "design_import_internal.hpp"

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pulp::view {

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

// ── Faithful-vector import enum<->id ────────────────────────────────────
static NodeRenderMode render_mode_from_id(const std::string& s) {
    return s == "faithful_svg" ? NodeRenderMode::faithful_svg : NodeRenderMode::normal;
}
static const char* render_mode_id(NodeRenderMode m) {
    return m == NodeRenderMode::faithful_svg ? "faithful_svg" : "normal";
}
// Maps a wire `kind` string to an InteractiveElementKind. Unknown ids fall back
// to `knob` for forward-compat, but set `*recognized = false` so the caller can
// diagnose a genuinely-unknown kind instead of silently shipping a wrong knob.
// `recognized` may be null.
static InteractiveElementKind interactive_kind_from_id(const std::string& s,
                                                       bool* recognized = nullptr) {
    if (recognized) *recognized = true;
    if (s == "knob")        return InteractiveElementKind::knob;
    if (s == "fader")       return InteractiveElementKind::fader;
    if (s == "toggle")      return InteractiveElementKind::toggle;
    if (s == "dropdown")    return InteractiveElementKind::dropdown;
    if (s == "text_field")  return InteractiveElementKind::text_field;
    if (s == "tab_group")   return InteractiveElementKind::tab_group;
    if (s == "stepper")     return InteractiveElementKind::stepper;
    if (s == "swap")        return InteractiveElementKind::swap;
    if (s == "action")      return InteractiveElementKind::action;
    if (s == "xy_pad")      return InteractiveElementKind::xy_pad;
    if (s == "value_label") return InteractiveElementKind::value_label;
    if (s == "custom")      return InteractiveElementKind::custom;
    if (recognized) *recognized = false;
    return InteractiveElementKind::knob;
}
static const char* interactive_kind_id(InteractiveElementKind k) {
    switch (k) {
        case InteractiveElementKind::fader:       return "fader";
        case InteractiveElementKind::toggle:      return "toggle";
        case InteractiveElementKind::dropdown:    return "dropdown";
        case InteractiveElementKind::text_field:  return "text_field";
        case InteractiveElementKind::tab_group:   return "tab_group";
        case InteractiveElementKind::stepper:     return "stepper";
        case InteractiveElementKind::swap:        return "swap";
        case InteractiveElementKind::action:      return "action";
        case InteractiveElementKind::xy_pad:      return "xy_pad";
        case InteractiveElementKind::value_label: return "value_label";
        case InteractiveElementKind::custom:      return "custom";
        case InteractiveElementKind::knob:        break;
    }
    return "knob";
}

// ── box-shadow parse / serialize ────────────────────────────────────────
//
// CSS `box-shadow` is a comma-separated list of layers; each layer is
// `[inset] <ox> <oy> [<blur> [<spread>]] <color>` with lengths in arbitrary
// order relative to the color but always offsets-first. The IR used to keep
// the whole declaration as one opaque string, so any layer past the first
// (and the structured offset/blur/spread/color/inset of every layer) was
// effectively lost. These parse/serialize helpers give every consumer the
// structured layers while preserving the original text for lossless
// round-trips.

namespace {

std::string bxsh_trim(std::string s) {
    auto sp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && sp((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && sp((unsigned char)s.back())) s.pop_back();
    return s;
}

// Split a CSS value on top-level commas only — commas inside parentheses
// (rgb()/rgba()/hsl()/color-mix()) are part of a single token.
std::vector<std::string> bxsh_split_top_level(const std::string& css) {
    std::vector<std::string> parts;
    int depth = 0;
    std::string cur;
    for (char c : css) {
        if (c == '(') { ++depth; cur.push_back(c); }
        else if (c == ')') { if (depth > 0) --depth; cur.push_back(c); }
        else if (c == ',' && depth == 0) { parts.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

// Tokenize one layer on top-level whitespace (parens kept intact).
std::vector<std::string> bxsh_tokenize(const std::string& layer) {
    std::vector<std::string> toks;
    int depth = 0;
    std::string t;
    for (char c : layer) {
        if (c == '(') { ++depth; t.push_back(c); }
        else if (c == ')') { if (depth > 0) --depth; t.push_back(c); }
        else if (std::isspace((unsigned char)c) && depth == 0) {
            if (!t.empty()) { toks.push_back(t); t.clear(); }
        } else t.push_back(c);
    }
    if (!t.empty()) toks.push_back(t);
    return toks;
}

// Parse a CSS length token ("12", "12px", "-4px") fully; returns false if the
// token is not a pure number (optionally px-suffixed).
bool bxsh_parse_length(const std::string& tok, float& out) {
    std::string n = tok;
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "px") == 0) n.resize(n.size() - 2);
    if (n.empty()) return false;
    try {
        size_t pos = 0;
        float v = std::stof(n, &pos);
        if (pos != n.size()) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

std::vector<IRBoxShadow> parse_css_box_shadow(const std::string& css) {
    std::vector<IRBoxShadow> out;
    std::string all = bxsh_trim(css);
    if (all.empty()) return out;
    {
        std::string lower = all;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower == "none") return out;
    }
    for (const auto& rawLayer : bxsh_split_top_level(all)) {
        std::string layer = bxsh_trim(rawLayer);
        if (layer.empty()) continue;
        IRBoxShadow sh;
        sh.raw = layer;
        std::vector<float> lengths;
        for (const auto& tok : bxsh_tokenize(layer)) {
            std::string low = tok;
            for (auto& c : low) c = (char)std::tolower((unsigned char)c);
            if (low == "inset") { sh.inset = true; continue; }
            float len = 0.0f;
            if (bxsh_parse_length(tok, len)) { lengths.push_back(len); continue; }
            // First non-length, non-inset token is the color (rgba()/hex/named).
            if (sh.color.empty()) sh.color = tok;
        }
        if (lengths.size() >= 1) sh.offset_x = lengths[0];
        if (lengths.size() >= 2) sh.offset_y = lengths[1];
        if (lengths.size() >= 3) sh.blur = lengths[2];
        if (lengths.size() >= 4) sh.spread = lengths[3];
        out.push_back(std::move(sh));
    }
    return out;
}

std::string box_shadow_to_css(const std::vector<IRBoxShadow>& shadows) {
    std::string out;
    for (size_t i = 0; i < shadows.size(); ++i) {
        if (i) out += ", ";
        const auto& s = shadows[i];
        if (!s.raw.empty()) { out += s.raw; continue; }
        std::ostringstream ss;
        if (s.inset) ss << "inset ";
        ss << s.offset_x << "px " << s.offset_y << "px " << s.blur << "px";
        if (s.spread != 0.0f) ss << ' ' << s.spread << "px";
        if (!s.color.empty()) ss << ' ' << s.color;
        out += ss.str();
    }
    return out;
}

// ── IR from JSON ────────────────────────────────────────────────────────

// Normalize a blend-mode keyword to its CSS lowercase-hyphen spelling. CSS
// sources (v0 / Stitch / Pencil) already emit "multiply"; Figma's API uses
// UPPER_SNAKE ("MULTIPLY", "COLOR_DODGE", "PASS_THROUGH"). The no-op modes
// (normal / pass-through) return nullopt so codegen emits nothing for them.
static std::optional<std::string> normalize_blend_mode(std::string raw) {
    for (auto& c : raw) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (c == '_') c = '-';
    }
    if (raw.empty() || raw == "normal" || raw == "pass-through" ||
        raw == "passthrough")
        return std::nullopt;
    return raw;
}

static IRStyle parse_ir_style(const choc::value::ValueView& obj) {
    IRStyle s;
    if (!obj.isObject()) return s;

    // Style keys arrive in two spellings: figma/pencil/v0 emit camelCase
    // (`borderRadius`, `backgroundColor`), the figma-plugin export emits
    // snake_case (`border_radius`, `background_color`). Single-word keys
    // (`width`, `color`) are identical, but every COMPOUND key would be
    // silently dropped for one source or the other. Resolve both spellings so
    // a frame's corner radius, borders, and background survive regardless of
    // which adapter produced the JSON.
    auto snake_of = [](const char* camel) {
        std::string out;
        for (const char* p = camel; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') { out.push_back('_'); out.push_back(static_cast<char>(*p - 'A' + 'a')); }
            else out.push_back(*p);
        }
        return out;
    };
    auto resolve_key = [&](const char* key) -> std::optional<std::string> {
        if (obj.hasObjectMember(key)) return std::string(key);
        std::string snake = snake_of(key);
        if (snake != key && obj.hasObjectMember(snake.c_str())) return snake;
        return std::nullopt;
    };
    auto set_opt_str = [&](const char* key, std::optional<std::string>& field) {
        if (auto k = resolve_key(key)) field = std::string(obj[k->c_str()].toString());
    };
    auto set_opt_float = [&](const char* key, std::optional<float>& field) {
        auto k = resolve_key(key);
        if (!k) return;
        const auto& v = obj[k->c_str()];
        // v0 / Stitch / Pencil emit CSS string dimensions ("100px", "12") where
        // the figma-plugin lane emits a bare number. getWithDefault<double> on a
        // string returns 0, which silently degenerates the dimension (a "1px"
        // border or width collapses to 0). Coerce a px-suffixed / numeric string
        // through the shared length parser; a non-length string (e.g. "auto",
        // "50%") leaves the field unset for the sizing-mode path to handle.
        if (v.isString()) {
            float parsed = 0.0f;
            if (bxsh_parse_length(std::string(v.toString()), parsed)) field = parsed;
        } else {
            field = static_cast<float>(v.getWithDefault<double>(0));
        }
    };
    auto set_opt_int = [&](const char* key, std::optional<int>& field) {
        if (auto k = resolve_key(key)) field = static_cast<int>(obj[k->c_str()].getWithDefault<int64_t>(0));
    };

    set_opt_str("backgroundColor", s.background_color);
    set_opt_str("backgroundGradient", s.background_gradient);
    set_opt_str("backgroundImage", s.background_image);
    set_opt_str("backgroundRepeat", s.background_repeat);
    set_opt_str("color", s.color);
    set_opt_float("opacity", s.opacity);
    set_opt_str("mixBlendMode", s.mix_blend_mode);
    if (s.mix_blend_mode) s.mix_blend_mode = normalize_blend_mode(*s.mix_blend_mode);
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
    if (auto k = resolve_key("boxShadow"))
        s.box_shadow = parse_css_box_shadow(std::string(obj[k->c_str()].toString()));
    set_opt_str("filter", s.filter);
    set_opt_str("backdropFilter", s.backdrop_filter);
    set_opt_str("clipPath", s.clip_path);
    set_opt_str("mask", s.mask);
    set_opt_str("maskImage", s.mask_image);
    set_opt_str("maskSize", s.mask_size);
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

    // render_bounds {w,h,dx,dy} — the asset's true visual extent when it bleeds
    // past the layout box (figma-plugin). Without this the silver-knob graphic
    // (210px wide) gets squashed into its 62px layout box.
    if (auto k = resolve_key("renderBounds")) {
        auto rb = obj[k->c_str()];
        if (rb.isObject()) {
            IRStyle::RenderBounds out{};
            auto rbf = [&](const char* m, float& f) {
                if (rb.hasObjectMember(m)) f = static_cast<float>(rb[m].getWithDefault<double>(0));
            };
            rbf("w", out.w); rbf("h", out.h); rbf("dx", out.dx); rbf("dy", out.dy);
            if (out.w > 0 && out.h > 0) s.render_bounds = out;
        }
    }

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

    // Padding — support a uniform float, a nested {top,right,bottom,left}
    // object (the figma-plugin export shape), or camelCase per-side keys.
    // All three forms are accepted and may be combined (later forms override).
    if (obj.hasObjectMember("padding")) {
        const auto& pad = obj["padding"];
        if (pad.isObject()) {
            // Nested object: {top,right,bottom,left}. Missing edges stay 0.
            if (pad.hasObjectMember("top"))    l.padding_top    = static_cast<float>(pad["top"].getWithDefault<double>(0));
            if (pad.hasObjectMember("right"))  l.padding_right  = static_cast<float>(pad["right"].getWithDefault<double>(0));
            if (pad.hasObjectMember("bottom")) l.padding_bottom = static_cast<float>(pad["bottom"].getWithDefault<double>(0));
            if (pad.hasObjectMember("left"))   l.padding_left   = static_cast<float>(pad["left"].getWithDefault<double>(0));
        } else {
            float p = get_float(obj, "padding");
            l.padding_top = l.padding_right = l.padding_bottom = l.padding_left = p;
        }
    }
    if (obj.hasObjectMember("paddingTop"))    l.padding_top = get_float(obj, "paddingTop");
    if (obj.hasObjectMember("paddingRight"))  l.padding_right = get_float(obj, "paddingRight");
    if (obj.hasObjectMember("paddingBottom")) l.padding_bottom = get_float(obj, "paddingBottom");
    if (obj.hasObjectMember("paddingLeft"))   l.padding_left = get_float(obj, "paddingLeft");
    if (obj.hasObjectMember("marginTop"))     l.margin_top = get_float(obj, "marginTop");
    if (obj.hasObjectMember("marginRight"))   l.margin_right = get_float(obj, "marginRight");
    if (obj.hasObjectMember("marginBottom"))  l.margin_bottom = get_float(obj, "marginBottom");
    if (obj.hasObjectMember("marginLeft"))    l.margin_left = get_float(obj, "marginLeft");

    auto parse_align = [](std::string s) -> LayoutAlign {
        // The figma-plugin export uses snake_case (flex_end, space_between);
        // earlier callers used kebab-case (flex-end). Normalize '_'→'-' so both
        // spell the same — otherwise snake_case justify/align silently fell
        // through to flex_start, leaving column `justify:flex_end` titles pinned
        // to the top (overlapping their sublabels) and dropping every
        // `space_between` row's distribution.
        for (auto& c : s) if (c == '_') c = '-';
        if (s == "center")        return LayoutAlign::center;
        if (s == "flex-end" || s == "end") return LayoutAlign::flex_end;
        if (s == "flex-start" || s == "start") return LayoutAlign::flex_start;
        if (s == "stretch")       return LayoutAlign::stretch;
        if (s == "space-between") return LayoutAlign::space_between;
        if (s == "space-around")  return LayoutAlign::space_around;
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

    // CSS Grid (camelCase + snake_case). Stored as raw CSS strings; the codegen
    // lowers them to createGrid/setGrid. Source-agnostic.
    auto first_str = [&](std::initializer_list<const char*> keys) -> std::optional<std::string> {
        for (const char* k : keys)
            if (obj.hasObjectMember(k)) {
                auto s = get_string(obj, k);
                if (!s.empty()) return s;
            }
        return std::nullopt;
    };
    l.grid_template_columns = first_str({"gridTemplateColumns", "grid_template_columns"});
    l.grid_template_rows    = first_str({"gridTemplateRows", "grid_template_rows"});
    l.grid_auto_flow        = first_str({"gridAutoFlow", "grid_auto_flow"});
    l.grid_column           = first_str({"gridColumn", "grid_column"});
    l.grid_row              = first_str({"gridRow", "grid_row"});

    return l;
}

static std::optional<IRConfidence> parse_confidence(const std::string& value);

bool is_asset_reference_key(std::string_view key) {
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

// ── figma-plugin binding normalization ──────────────────────────────────────
//
// The figma-plugin extractor (tools/figma-plugin/src/extract.ts) emits a Pulp
// Library control as a recognized audio widget plus a single free-form
// `attributes["binding"]` string (e.g. "filter.cutoff_hz"). Historically that
// string reached the IR but was dropped: the native materializer
// (design_import_native_common.cpp) and the binding-manifest codegen
// (design_cpp_codegen.cpp) only consume the `pulp*`-prefixed binding contract,
// never the raw `binding` attribute.
//
// This normalizes the figma-plugin `binding` into the SAME internal binding
// representation that the JSX / Claude path already feeds —
// NativeBindingMetadata — so the native materializer and the codegen manifest
// both pick it up via their existing logic, with no duplicated downstream
// consumer. It runs at the IR-ingest boundary (end of parse_ir_node), gated on
// the node being a semantically-recognized audio widget. An unrecognized /
// generic node (audio_widget == none) never gets a synthesized binding.
//
// The raw `attributes["binding"]` is preserved (the evidence is not deleted),
// and NativeBindingMetadata::serialize() uses no-overwrite / skip-empty
// semantics so a node that already carries `pulp*` binding attributes (e.g. a
// JSX/Claude import that happens to also carry a `binding` key) is left exactly
// as-is — no regression to the existing pulp* path.
void normalize_figma_plugin_binding(IRNode& node) {
    // Gate 1: only semantically-recognized widgets get a synthesized binding.
    if (node.audio_widget == AudioWidgetType::none)
        return;

    // Gate 2: must carry a non-empty figma-plugin `binding` string.
    auto binding_it = node.attributes.find("binding");
    if (binding_it == node.attributes.end() || binding_it->second.empty())
        return;

    // Gate 3: if the node already carries any of the canonical single-param /
    // meter binding-contract attributes, leave it untouched. This keeps the
    // existing pulp* path byte-identical and avoids double-synthesis on a node
    // that was already lowered by the JSX/Claude writer.
    for (const char* existing : {"pulpParamKey", "pulpBindingModule",
                                 "pulpBindingParam", "pulpMeterSource",
                                 "pulpMeterChannel"}) {
        auto it = node.attributes.find(existing);
        if (it != node.attributes.end() && !it->second.empty())
            return;
    }

    const std::string& binding = binding_it->second;

    // Split on the first '.': "<module>.<param>". A binding with no '.' has an
    // empty module and the whole string as the param/channel.
    std::string module_part;
    std::string param_part = binding;
    if (auto dot = binding.find('.'); dot != std::string::npos) {
        module_part = binding.substr(0, dot);
        param_part = binding.substr(dot + 1);
    }

    NativeBindingMetadata md;
    // route_id is required for the codegen helper gate to emit a live bind_*()
    // call; make it deterministic from the binding so re-imports are stable.
    md.route_id = "figma-plugin:" + binding;
    md.route_type = "native_cpp";

    switch (node.audio_widget) {
        case AudioWidgetType::meter:
        case AudioWidgetType::spectrum:
            // Meters read from a metering source/channel, not a writable param.
            md.source_family = "meter";
            md.meter_source = module_part.empty() ? binding : module_part;
            md.meter_channel = param_part;
            break;
        case AudioWidgetType::knob:
        case AudioWidgetType::fader:
        case AudioWidgetType::xy_pad:
        case AudioWidgetType::waveform:
        default:
            // Scalar parameter controls. param_key drives both the manifest
            // entry and the codegen helper gate (has_single_param).
            md.source_family = "param";
            md.param_key = binding;
            if (!module_part.empty())
                md.binding_module = module_part;
            md.binding_param = param_part;
            break;
    }

    // serialize() writes only present, non-empty values and never overwrites an
    // existing non-empty attribute — so `attributes["binding"]` and any other
    // pre-existing data survive untouched.
    md.serialize(node);
}

// ── parse_ir_node post-passes ────────────────────────────────────────────
// Named post-parse rules. The shadow snap reads parsed IRBoxShadow layers
// instead of re-parsing the raw CSS string each time.

// Shadow-driven sibling snap. When a frame has a downward drop shadow and an
// absolutely-positioned sibling sits just below it with a small gap, the gap
// exposes the grandparent's canvas color through the shadow zone — a thin
// lighter band between the panel and whatever sits below. Figma's designer
// places these tightly because Figma's shadow extends visually ONTO the
// sibling below (shadow is drawn on top); the intent is visual continuity.
// Pulp paints the lower sibling ABOVE the shadow (later in z-order) so closing
// the geometric gap gives us the same continuity. Rule: for each absolute
// child F with a downward drop shadow, look at the next absolute sibling S
// beneath it; if 0 < gap < (oy + blur/2), snap S up — but leave the shadow's
// y-offset worth of room so Pulp's same-z-layer shadow still has somewhere to
// render (otherwise S overpaints it).
static void snap_absolute_siblings_under_shadow(IRNode& node) {
    // First non-inset downward drop-shadow layer of a node, if any.
    auto down_shadow = [](const IRStyle& st) -> const IRBoxShadow* {
        for (const auto& sh : st.box_shadow)
            if (!sh.inset && sh.offset_y > 0.0f) return &sh;
        return nullptr;
    };
    struct SibRect { size_t idx; float top, bottom; bool has_down_shadow; float shadow_reach; };
    std::vector<SibRect> abs_siblings;
    for (size_t i = 0; i < node.children.size(); ++i) {
        auto& c = node.children[i];
        bool is_abs = c.style.position && *c.style.position == "absolute";
        if (!is_abs) continue;
        float top = c.style.top.value_or(0.0f);
        float h   = c.style.height.value_or(0.0f);
        if (h <= 0.0f) continue;
        const IRBoxShadow* sh = down_shadow(c.style);
        float oy = sh ? sh->offset_y : 0.0f;
        float blur = sh ? sh->blur : 0.0f;
        abs_siblings.push_back({i, top, top + h, sh != nullptr, oy + blur * 0.5f});
    }
    std::sort(abs_siblings.begin(), abs_siblings.end(),
              [](const SibRect& a, const SibRect& b){ return a.top < b.top; });
    for (size_t k = 0; k + 1 < abs_siblings.size(); ++k) {
        const auto& F = abs_siblings[k];
        auto& S       = abs_siblings[k + 1];
        if (!F.has_down_shadow) continue;
        float gap = S.top - F.bottom;
        if (gap <= 0.0f) continue;
        if (gap >= F.shadow_reach) continue;
        const IRBoxShadow* fsh = down_shadow(node.children[F.idx].style);
        float preserve = std::max(0.0f, fsh ? fsh->offset_y : 0.0f);
        float close = std::max(0.0f, gap - preserve);
        if (close <= 0.0f) continue;
        float new_top = S.top - close;
        node.children[S.idx].style.top = new_top;
        S.bottom -= (S.top - new_top);
        S.top = new_top;
    }
}

// Audio-widget detection (deferred until after children are parsed). A node is
// an audio widget ONLY if its name/type matches an audio-widget pattern AND it
// has no child frames/groups — a node with only shape children (ellipse /
// rectangle) + text is a widget; a node with child frames (e.g. "KnobRow"
// containing four knob frames) is a container. Skipped when the source already
// set an explicit audio_widget.
static void detect_node_audio_widget(IRNode& node, bool explicit_audio_widget) {
    auto detected = explicit_audio_widget ? AudioWidgetType::none
                                          : detect_audio_widget(node.name);
    if (detected == AudioWidgetType::none && !node.type.empty() && !explicit_audio_widget)
        detected = detect_audio_widget(node.type);
    if (detected == AudioWidgetType::none) return;

    // A child that is itself a container — a populated frame/group, or any
    // nested-frame/group structure — marks this node as a composite layout
    // that must NOT be collapsed into a single audio widget (a widget-named
    // row of sub-widgets stays a row). The ONE exception is a stroke-demoted
    // decoration: a ~0-width stroked leaf (e.g. a knob's pointer hairline)
    // that the pre-pass above retypes image→frame. Without this exception that
    // lone decorative box blocked recognition, so a knob shipping a pointer
    // line fell through to a raw container of images instead of a native knob.
    // The decoration is tagged at demotion time (__stroke_demoted).
    bool has_child_containers = false;
    for (auto& child : node.children) {
        const bool decorative_stroke =
            child.attributes.count("__stroke_demoted") != 0;
        const bool is_container =
            !child.children.empty() ||
            ((child.type == "frame" || child.type == "group") && !decorative_stroke);
        if (is_container) {
            has_child_containers = true;
            break;
        }
    }
    if (!has_child_containers)
        node.audio_widget = detected;
}

// Parse a shape's stroke color (Pencil puts stroke on ellipse/rectangle nodes)
// into the `stroke_color` attribute.
static void parse_shape_stroke_color(IRNode& node, const choc::value::ValueView& obj) {
    if (!obj.hasObjectMember("stroke")) return;
    auto stroke = obj["stroke"];
    if (!stroke.isObject() || !stroke.hasObjectMember("fill")) return;
    auto fill = stroke["fill"];
    if (!fill.isString()) return;
    auto fill_str = std::string(fill.toString());
    if (!fill_str.empty() && fill_str[0] == '#')
        node.attributes["stroke_color"] = fill_str;
}

// For audio widgets: copy the first child shape's dimensions into
// shape_width/shape_height attributes. The frame's own width/height belong to
// the column container; the child shape's belong to the widget itself.
static void extract_widget_shape_dims(IRNode& node) {
    if (node.audio_widget == AudioWidgetType::none || node.children.empty()) return;
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

// Figma resize-constraint normalization. Figma's API spells these MIN / MAX /
// CENTER / STRETCH / SCALE; we also accept already-normalized tokens and the
// CSS-ish "left_right"/"top_bottom" spellings other tools emit. Returns nullopt
// for an unrecognized value so it is simply left unconstrained.
static std::optional<std::string> normalize_h_constraint(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "min" || s == "left")  return std::string("left");
    if (s == "max" || s == "right") return std::string("right");
    if (s == "center")              return std::string("center");
    if (s == "scale")               return std::string("scale");
    if (s == "stretch" || s == "left_right" || s == "leftright")
        return std::string("stretch");
    return std::nullopt;
}
static std::optional<std::string> normalize_v_constraint(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (s == "min" || s == "top")    return std::string("top");
    if (s == "max" || s == "bottom") return std::string("bottom");
    if (s == "center")               return std::string("center");
    if (s == "scale")                return std::string("scale");
    if (s == "stretch" || s == "top_bottom" || s == "topbottom")
        return std::string("stretch");
    return std::nullopt;
}

IRNode parse_ir_node(const choc::value::ValueView& obj) {
    IRNode node;
    node.type = get_string(obj, "type", "frame");
    node.name = get_string(obj, "name");
    node.text_content = get_string(obj, "content");
    // Per-range text style runs (mixed bold/colored/sized text). Accept `runs`
    // or `textRuns`: an array of {start,end, fontSize?, fontWeight?, italic? |
    // fontStyle?, color?, letterSpacing?, textDecoration?}. Source-agnostic —
    // any source expressing styled ranges feeds the same IR runs.
    for (const char* runs_key : {"runs", "textRuns"}) {
        if (!obj.hasObjectMember(runs_key) || !obj[runs_key].isArray()) continue;
        const auto runs = obj[runs_key];
        for (uint32_t i = 0; i < runs.size(); ++i) {
            const auto r = runs[i];
            if (!r.isObject()) continue;
            IRTextRun run;
            run.start = static_cast<int>(get_float(r, "start"));
            run.end   = static_cast<int>(get_float(r, "end"));
            if (run.end <= run.start) continue;
            if (r.hasObjectMember("fontSize"))   run.font_size = get_float(r, "fontSize");
            if (r.hasObjectMember("fontWeight"))
                run.font_weight = static_cast<int>(get_float(r, "fontWeight"));
            if (r.hasObjectMember("fontStyle"))  run.font_style = get_string(r, "fontStyle");
            else if (get_bool(r, "italic"))      run.font_style = "italic";
            if (r.hasObjectMember("color"))      run.color = get_string(r, "color");
            if (r.hasObjectMember("letterSpacing")) run.letter_spacing = get_float(r, "letterSpacing");
            if (r.hasObjectMember("textDecoration")) run.text_decoration = get_string(r, "textDecoration");
            node.text_runs.push_back(std::move(run));
        }
        if (!node.text_runs.empty()) break;
    }

    // Capture the source-native ID so the `adapter` anchor strategy can use it
    // as its anchor. Figma + Pencil + Mitosis-style exports all carry an ID
    // under one of these field names; first non-empty wins. Sources without
    // native IDs (Stitch HTML, v0 TSX, Claude HTML) leave this empty and fall
    // through to the content-hash strategy.
    for (const char* k : {"id", "nodeId", "node_id", "source_node_id", "sourceNodeId"}) {
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

    // ── Faithful-vector import: render mode + SVG asset + overlays ──
    for (const char* k : {"render_mode", "renderMode"}) {
        if (obj.hasObjectMember(k) && obj[k].isString()) {
            node.render_mode = render_mode_from_id(get_string(obj, k));
            break;
        }
    }
    for (const char* k : {"svg_asset_id", "svgAssetId"}) {
        if (obj.hasObjectMember(k) && obj[k].isString()) {
            node.svg_asset_id = get_string(obj, k);
            break;
        }
    }
    for (const char* arr_key : {"interactive_elements", "interactiveElements"}) {
        if (!obj.hasObjectMember(arr_key) || !obj[arr_key].isArray()) continue;
        const auto arr = obj[arr_key];
        for (uint32_t i = 0; i < arr.size(); ++i) {
            const auto e = arr[static_cast<int>(i)];
            if (!e.isObject()) continue;
            IRInteractiveElement el;
            const std::string kind_str = get_string(e, "kind", "knob");
            bool kind_recognized = true;
            el.kind = interactive_kind_from_id(kind_str, &kind_recognized);
            if (!kind_recognized) {
                // Don't silently materialize an unknown control as a working
                // knob — surface it so the import isn't quietly wrong.
                pulp::runtime::log_warn(
                    "design-import: unknown interactive_element kind '{}' "
                    "(node {}); falling back to knob render",
                    kind_str,
                    e.hasObjectMember("source_node_id") &&
                            e["source_node_id"].isString()
                        ? std::string(e["source_node_id"].toString())
                        : std::string("?"));
            }
            el.cx = get_float(e, "cx");
            el.cy = get_float(e, "cy");
            el.hit_radius = get_float(e, "hit_radius");
            el.svg_patch_d = get_string(e, "svg_patch_d");
            el.default_value = get_float(e, "default_value", 0.5f);
            el.flash = get_bool(e, "flash");  // toggle: press-flash vs sticky
            // Overlay-control fields (dropdown / text_field / tab_group).
            el.x = get_float(e, "x");
            el.y = get_float(e, "y");
            el.w = get_float(e, "w");
            el.h = get_float(e, "h");
            el.selected_index = static_cast<int>(get_float(e, "selected_index"));
            el.placeholder = get_string(e, "placeholder");
            el.bg_color = get_string(e, "bg_color");
            el.label = get_string(e, "label");
            // swap / action / xy_pad / value_label fields.
            el.target_frame = static_cast<int>(get_float(e, "target_frame", -1.0f));
            el.action = get_string(e, "action");
            el.text = get_string(e, "text");
            el.value_left_align = get_bool(e, "value_left_align");
            el.default_value_y = get_float(e, "default_value_y", 0.5f);
            // Import report (resolution provenance).
            el.resolution_rung = static_cast<int>(get_float(e, "resolution_rung", 0.0f));
            el.confidence_score = get_float(e, "confidence_score", 1.0f);
            el.verification_pass = get_bool(e, "verification_pass", true);
            // custom — registered native control.
            el.factory_id = get_string(e, "factory_id");
            el.custom_props = get_string(e, "custom_props");
            if (e.hasObjectMember("conflict_signals") && e["conflict_signals"].isArray()) {
                const auto cs = e["conflict_signals"];
                for (uint32_t j = 0; j < cs.size(); ++j)
                    if (cs[static_cast<int>(j)].isString())
                        el.conflict_signals.push_back(
                            std::string(cs[static_cast<int>(j)].toString()));
            }
            if (e.hasObjectMember("options") && e["options"].isArray()) {
                const auto opts = e["options"];
                for (uint32_t j = 0; j < opts.size(); ++j)
                    if (opts[static_cast<int>(j)].isString())
                        el.options.push_back(std::string(opts[static_cast<int>(j)].toString()));
            }
            if (e.hasObjectMember("source_node_id") && e["source_node_id"].isString())
                el.source_node_id = get_string(e, "source_node_id");
            node.interactive_elements.push_back(std::move(el));
        }
        if (!node.interactive_elements.empty()) break;
    }

    if (obj.hasObjectMember("style"))
        node.style = parse_ir_style(obj["style"]);
    // The figma-plugin export carries the blend mode in the `figma` block
    // (e.g. {"figma": {"blend_mode": "MULTIPLY"}}), not in `style`. Promote it
    // into the normalized IRStyle when `style.mixBlendMode` didn't already set
    // one, so a Figma layer's blend mode survives the same as a CSS source's.
    if (!node.style.mix_blend_mode && obj.hasObjectMember("figma") &&
        obj["figma"].isObject() && obj["figma"].hasObjectMember("blend_mode") &&
        obj["figma"]["blend_mode"].isString()) {
        node.style.mix_blend_mode =
            normalize_blend_mode(std::string(obj["figma"]["blend_mode"].toString()));
    }
    if (obj.hasObjectMember("layout"))
        node.layout = parse_ir_layout(obj["layout"]);
    // Resize constraints. Figma carries them at node level as
    // `constraints: {horizontal, vertical}`; the figma-plugin export nests the
    // same under a `figma` block; CSS-ish sources may put them inside `layout`.
    // Read all three, first non-empty wins. Source-agnostic: keyed on the
    // normalized token, never a layer name.
    auto read_constraints = [&](const choc::value::ValueView& c) {
        if (!c.isObject()) return;
        if (!node.layout.h_constraint && c.hasObjectMember("horizontal") &&
            c["horizontal"].isString())
            node.layout.h_constraint =
                normalize_h_constraint(std::string(c["horizontal"].toString()));
        if (!node.layout.v_constraint && c.hasObjectMember("vertical") &&
            c["vertical"].isString())
            node.layout.v_constraint =
                normalize_v_constraint(std::string(c["vertical"].toString()));
    };
    if (obj.hasObjectMember("constraints")) read_constraints(obj["constraints"]);
    if (obj.hasObjectMember("figma") && obj["figma"].isObject() &&
        obj["figma"].hasObjectMember("constraints"))
        read_constraints(obj["figma"]["constraints"]);
    if (obj.hasObjectMember("layout") && obj["layout"].isObject() &&
        obj["layout"].hasObjectMember("constraints"))
        read_constraints(obj["layout"]["constraints"]);
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

    // Top-level `asset_ref` (figma-plugin lane stamps it directly on the node,
    // not under `attributes`). Promote it into node.attributes so the import
    // CLI's asset-resolution pass can resolve it to a file path — this feeds
    // both the knob sprite-strip skin and fader/meter skin sampling.
    // Don't overwrite an attributes-nested asset_ref if one was already set.
    if (obj.hasObjectMember("asset_ref") && obj["asset_ref"].isString() &&
        node.attributes.find("asset_ref") == node.attributes.end()) {
        auto ar = std::string(obj["asset_ref"].toString());
        if (!ar.empty()) node.attributes["asset_ref"] = std::move(ar);
    }

    // SVG path geometry — preserve the path-data string under a canonical key
    // so codegen can lower vector/path/svg_path nodes to a native SvgPathWidget
    // instead of silently dropping them.
    // Multi-spelling so Pencil / Stitch / v0 / Claude / RN SVG exports all
    // survive (the figma lane rasterizes vectors to PNG, so this serves the
    // path-carrying sources). First non-empty wins; an attributes-nested
    // path_data copied above is not overwritten. svg_fill / svg_stroke /
    // svg_stroke_width capture the path's own paint for the SvgPath skin.
    if (node.attributes.find("path_data") == node.attributes.end()) {
        for (const char* k : {"path_data", "pathData", "path_d", "d"}) {
            if (obj.hasObjectMember(k) && obj[k].isString()) {
                auto v = std::string(obj[k].toString());
                if (!v.empty()) { node.attributes["path_data"] = std::move(v); break; }
            }
        }
    }
    if (node.attributes.count("path_data")) {
        if (!node.attributes.count("svg_viewbox") &&
            obj.hasObjectMember("viewBox") && obj["viewBox"].isString()) {
            auto v = std::string(obj["viewBox"].toString());
            if (!v.empty()) node.attributes["svg_viewbox"] = std::move(v);
        }
        auto capture_color = [&](const char* src, const char* dst) {
            if (node.attributes.count(dst)) return;
            if (obj.hasObjectMember(src) && obj[src].isString()) {
                auto v = std::string(obj[src].toString());
                if (!v.empty()) node.attributes[dst] = std::move(v);
            }
        };
        capture_color("fill", "svg_fill");
        capture_color("stroke", "svg_stroke");
        if (!node.attributes.count("svg_stroke_width")) {
            for (const char* k : {"strokeWidth", "stroke_width"}) {
                if (obj.hasObjectMember(k)) {
                    float sw = get_float(obj, k, 0.0f);
                    if (sw > 0.0f) { node.attributes["svg_stroke_width"] = std::to_string(sw); break; }
                }
            }
        }
    }

    // Exact layout dimensions from snapshot_layout (injected by import skill)
    if (obj.hasObjectMember("_layoutHeight"))
        node.attributes["_layoutHeight"] = std::to_string(static_cast<int>(get_float(obj, "_layoutHeight", 0)));
    if (obj.hasObjectMember("_layoutWidth"))
        node.attributes["_layoutWidth"] = std::to_string(static_cast<int>(get_float(obj, "_layoutWidth", 0)));

    // Audio widget detection is deferred until after children are parsed
    // (see below) — containers with child frames shouldn't be widgets

    bool explicit_audio_widget = false;
    if (obj.hasObjectMember("audioWidget") && obj["audioWidget"].isString()) {
        node.audio_widget = audio_widget_from_id(get_string(obj, "audioWidget"));
        explicit_audio_widget = node.audio_widget != AudioWidgetType::none;
    } else if (obj.hasObjectMember("audio_widget") && obj["audio_widget"].isString()) {
        node.audio_widget = audio_widget_from_id(get_string(obj, "audio_widget"));
        explicit_audio_widget = node.audio_widget != AudioWidgetType::none;
    }
    if (obj.hasObjectMember("label"))
        node.audio_label = get_string(obj, "label");
    if (obj.hasObjectMember("min"))
        node.audio_min = get_float(obj, "min", 0.0f);
    if (obj.hasObjectMember("max"))
        node.audio_max = get_float(obj, "max", 1.0f);
    if (obj.hasObjectMember("default"))
        node.audio_default = get_float(obj, "default", 0.5f);
    if (obj.hasObjectMember("min") && obj.hasObjectMember("max"))
        node.has_audio_range = true;

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

    // ── Separator promotion ─────────────────────────────────────────────
    // Figma stores 1-pixel vertical lines (column separators, etc.) as a
    // VECTOR node with effectively-zero width or height plus a 1px stroke.
    // The figma-plugin extractor captures the bounding-box dims faithfully,
    // which means we end up with width ≈ 5e-06 — invisible in any renderer.
    // Promote the stroke weight to the visible dimension and turn the
    // node into a colored rect (drop the empty PNG fill) so it actually
    // shows up. Trigger: width < 0.5 or height < 0.5 AND border_width >= 0.5.
    {
        constexpr float kDegenerateAxis = 0.5f;
        constexpr float kMinStrokeWeight = 0.5f;  // Figma "1px" strokes
                                                   // often come through at
                                                   // 0.97 due to fractional
                                                   // raster alignment.
        float bw = node.style.border_width.value_or(0.0f);
        // Only fire when BOTH dimensions are explicitly set AND at least
        // one is degenerate. A nullopt (auto-sized) dim must NOT be
        // treated as 0 — that would misfire on round-trip parses of
        // legitimately auto-sized stroked frames.
        bool has_w = node.style.width.has_value();
        bool has_h = node.style.height.has_value();
        float w = node.style.width.value_or(0.0f);
        float h = node.style.height.value_or(0.0f);
        bool degenerate_w = has_w && w < kDegenerateAxis;
        bool degenerate_h = has_h && h < kDegenerateAxis;
        if (bw >= kMinStrokeWeight && has_w && has_h && (degenerate_w || degenerate_h)) {
            if (degenerate_w) node.style.width  = std::max(bw, 1.0f);
            if (degenerate_h) node.style.height = std::max(bw, 1.0f);
            // Use the stroke color as the rect fill; the captured PNG is
            // a zero-area image and would render nothing anyway.
            if (!node.style.background_color && node.style.border_color)
                node.style.background_color = node.style.border_color;
            // The fill now IS the hairline. Drop the stroke so codegen does
            // not ALSO emit a border — a 1.5px line + a 1.5px border draws on
            // both edges and renders ~3× too wide (e.g. a knob pointer line).
            // The width was already set to the stroke weight above, so the
            // filled rect alone reproduces the line at its true thickness.
            node.style.border.reset();
            node.style.border_color.reset();
            node.style.border_width.reset();
            node.style.border_style.reset();
            // Strip the asset_ref so codegen's image branch doesn't try to
            // emit setImageSource on a degenerate PNG.
            node.attributes.erase("asset_ref");
            // Demote from "image" to "frame" so codegen emits a styled
            // container instead of an <img>-style image element.
            if (node.type == "image") node.type = "frame";
            // Tag it as a decorative stroke so a parent widget's recognition
            // gate treats it as ornamentation, not as a disqualifying nested
            // container (a knob keeps its pointer hairline AND its widget-ness).
            node.attributes["__stroke_demoted"] = "1";
        }
    }

    // Children
    if (obj.hasObjectMember("children") && obj["children"].isArray()) {
        auto children = obj["children"];
        for (uint32_t i = 0; i < children.size(); ++i)
            node.children.push_back(parse_ir_node(children[static_cast<int>(i)]));
    }

    // ── Inherit rounded corners from rounded parent ─────────────────────
    // Figma stores a corner radius on the CONTAINER frame and relies on
    // overflow:clip to round the children that fill it. Pulp's renderer
    // doesn't clip children to a parent's border-radius, so a gradient
    // rect that exactly fills a rounded parent ends up with hard corners.
    // Propagate the parent's radius to any child that has position:abs
    // at (0,0) and matches the parent's size (or any axis matches and
    // the other is close), so the gradient/fill child also paints with
    // rounded corners.
    if (node.style.border_radius && *node.style.border_radius > 0.0f) {
        float pr = *node.style.border_radius;
        float pw = node.style.width.value_or(0.0f);
        float ph = node.style.height.value_or(0.0f);
        for (auto& c : node.children) {
            if (c.style.border_radius && *c.style.border_radius > 0.0f)
                continue;  // child already has its own radius — respect it
            float cl = c.style.left.value_or(-1.0f);
            float ct = c.style.top.value_or(-1.0f);
            float cw = c.style.width.value_or(0.0f);
            float ch = c.style.height.value_or(0.0f);
            constexpr float kFillTol = 1.0f;
            bool fills_origin = (std::abs(cl) < kFillTol) && (std::abs(ct) < kFillTol);
            bool fills_size = (pw > 0.0f && std::abs(cw - pw) < kFillTol) &&
                              (ph > 0.0f && std::abs(ch - ph) < kFillTol);
            if (fills_origin && fills_size) {
                c.style.border_radius = pr;
            }
        }
    }

    // Shadow-driven sibling snap.
    snap_absolute_siblings_under_shadow(node);

    // ── Connector-line spanning rule ────────────────────────────────────
    // Pattern: a flex row whose FIRST child is a horizontal hairline (height
    // ≤ a couple of px, width > 1) and whose SUBSEQUENT children are
    // boxes/widgets/buttons that visually sit ON TOP of the line. Figma
    // designers use this to communicate a connected pipeline — the line
    // threads BEHIND the items so the visible bits are the gaps between
    // boxes. Without a fix, our flex layout puts the line in the
    // first-item slot (compressed to its 106-ish px width on the left),
    // breaking the connection visual. Convert the line to absolute,
    // span the full row width, and centre it vertically. Because it
    // stays first in z-order, subsequent children draw on top — the
    // visible segments emerge as gaps. Generalises a connector-rail
    // pattern (e.g. an FX-rack row: a hairline + chained dropdowns + "+")
    // to any flex row with a connector hairline + ≥ 2 sibling widgets.
    if (node.layout.direction == LayoutDirection::row && node.children.size() >= 3) {
        auto& first = node.children.front();
        float fw = first.style.width.value_or(0.0f);
        float fh = first.style.height.value_or(0.0f);
        size_t non_line_followers = 0;
        std::vector<float> follower_widths;
        float max_follower_h = 0.0f;
        for (size_t i = 1; i < node.children.size(); ++i) {
            const auto& c = node.children[i];
            float cw = c.style.width.value_or(0.0f);
            float ch = c.style.height.value_or(0.0f);
            if (cw >= 8.0f && ch >= 8.0f) {
                ++non_line_followers;
                follower_widths.push_back(cw);
                max_follower_h = std::max(max_follower_h, ch);
            }
        }
        float row_w = node.style.width.value_or(0.0f);
        float row_h = node.style.height.value_or(0.0f);
        // Trigger conditions for the connector promotion. Each gate exists
        // to NOT misfire on legitimate "thin first child" patterns:
        //   - is_horizontal_hairline: 0 < height ≤ 2px, width > 4px
        //   - fits_below_half_row: a real CONNECTOR has the line drawn
        //     much shorter than the row width (Figma stores the
        //     SEGMENT length, not the spanning extent). A flex row
        //     whose first child has width >= 50% of the row is almost
        //     certainly a content element (progress bar, slider track,
        //     divider) that should participate in flex sizing.
        //   - row_much_taller: a 2px first child inside a 4-6px tall
        //     row is geometry; a 2px first child inside a ≥ 6× tall
        //     row is a connector because there's vertical headroom
        //     for the dropdowns/buttons to sit ON TOP of it.
        bool is_horizontal_hairline = (fh > 0.0f && fh <= 2.0f && fw > 4.0f);
        bool fits_below_half_row = (row_w > 0.0f && fw <= row_w * 0.5f);
        bool row_much_taller = (max_follower_h > 0.0f && row_h >= max_follower_h * 1.5f)
                             || (row_h >= fh * 6.0f);
        if (is_horizontal_hairline && fits_below_half_row && row_much_taller &&
            non_line_followers >= 2 && row_w > 0.0f && row_h > 0.0f) {
            // Compute the span. Default: full row width. Refinement: if the
            // LAST follower is significantly smaller than the others (≤ 60%
            // of the median width), it's most likely a trailing "add" /
            // "more" affordance ("+", "settings cog", etc.) — NOT part of
            // the connected pipeline. Pull the line's right edge back to
            // just-past the penultimate widget so the connection visual
            // ends at the last real item.
            float span_w = row_w;
            if (follower_widths.size() >= 3) {
                std::vector<float> sorted = follower_widths;
                std::sort(sorted.begin(), sorted.end());
                float median = sorted[sorted.size() / 2];
                float last = follower_widths.back();
                if (median > 0.0f && (last / median) < 0.6f) {
                    // Trailing widget is most likely an "add" / "more"
                    // affordance, not part of the connected pipeline.
                    // Pull the line's right edge back by that widget's
                    // width + the gap before it, so the connection
                    // visual ends at the last real item.
                    float gap = node.layout.gap;
                    span_w = std::max(median, row_w - last - gap);
                }
            }
            first.style.position = "absolute";
            first.style.left = 0.0f;
            first.style.top  = (row_h - fh) * 0.5f;
            first.style.width  = span_w;
            first.style.height = fh;             // keep stroke weight
        }
    }

    // Tail post-passes, extracted into named functions.
    detect_node_audio_widget(node, explicit_audio_widget);
    parse_shape_stroke_color(node, obj);
    extract_widget_shape_dims(node);

    // Normalize a recognized figma-plugin widget's free-form `binding` string
    // into the canonical pulp* binding contract. Runs after audio_widget is
    // finalized (covers both explicit `audio_widget` and name-detected widgets).
    normalize_figma_plugin_binding(node);

    return node;
}

IRTokens parse_ir_tokens(const choc::value::ValueView& obj) {
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
        case DesignSource::figma_plugin: return "figma-plugin";
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

ImportDiagnostic make_import_diagnostic(ImportDiagnosticSeverity severity,
                                               std::string code,
                                               std::string path,
                                               std::string message,
                                               ImportDiagnosticKind kind) {
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
    write_string_member(out, first, "mixBlendMode", s.mix_blend_mode);
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
    if (!s.box_shadow.empty())
        write_string_member(out, first, "boxShadow", box_shadow_to_css(s.box_shadow));
    write_string_member(out, first, "filter", s.filter);
    write_string_member(out, first, "backdropFilter", s.backdrop_filter);
    write_string_member(out, first, "clipPath", s.clip_path);
    write_string_member(out, first, "mask", s.mask);
    write_string_member(out, first, "maskImage", s.mask_image);
    write_string_member(out, first, "maskSize", s.mask_size);
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
    // CSS grid + Figma resize constraints — round-trip the fields parse_ir_layout
    // / parse_ir_node read back, so a frozen/serialized DesignIR does not lose
    // them on the second parse+codegen pass.
    write_string_member(out, first, "gridTemplateColumns", l.grid_template_columns);
    write_string_member(out, first, "gridTemplateRows", l.grid_template_rows);
    write_string_member(out, first, "gridAutoFlow", l.grid_auto_flow);
    write_string_member(out, first, "gridColumn", l.grid_column);
    write_string_member(out, first, "gridRow", l.grid_row);
    if (l.h_constraint || l.v_constraint) {
        write_key(out, first, "constraints");
        out << '{';
        bool cf = true;
        write_string_member(out, cf, "horizontal", l.h_constraint);
        write_string_member(out, cf, "vertical", l.v_constraint);
        out << '}';
    }
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

    // Per-range text style runs — round-trip so mixed-style text survives a
    // serialize -> re-parse pass (e.g. `--emit ir-json` then re-import).
    if (!node.text_runs.empty()) {
        write_key(out, first, "runs");
        out << '[';
        for (size_t i = 0; i < node.text_runs.size(); ++i) {
            if (i) out << ',';
            const auto& r = node.text_runs[i];
            out << '{';
            bool rf = true;
            write_int_member(out, rf, "start", r.start);
            write_int_member(out, rf, "end", r.end);
            write_float_member(out, rf, "fontSize", r.font_size);
            write_int_member(out, rf, "fontWeight", r.font_weight);
            write_string_member(out, rf, "fontStyle", r.font_style);
            write_string_member(out, rf, "color", r.color);
            write_float_member(out, rf, "letterSpacing", r.letter_spacing);
            write_string_member(out, rf, "textDecoration", r.text_decoration);
            out << '}';
        }
        out << ']';
    }

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

    // ── Faithful-vector import ───────────────────────────────────────────
    // Structural, not source-metadata: a faithful_svg node must round-trip
    // its render mode + asset + overlays regardless of metadata stripping.
    if (node.render_mode != NodeRenderMode::normal)
        write_string_member(out, first, "render_mode", render_mode_id(node.render_mode));
    write_string_member(out, first, "svg_asset_id", node.svg_asset_id);
    if (!node.interactive_elements.empty()) {
        write_key(out, first, "interactive_elements");
        out << '[';
        for (size_t i = 0; i < node.interactive_elements.size(); ++i) {
            if (i) out << ',';
            const auto& el = node.interactive_elements[i];
            out << '{';
            bool ef = true;
            write_string_member(out, ef, "kind", interactive_kind_id(el.kind));
            write_float_member(out, ef, "cx", el.cx);
            write_float_member(out, ef, "cy", el.cy);
            write_float_member(out, ef, "hit_radius", el.hit_radius);
            if (!el.svg_patch_d.empty()) write_string_member(out, ef, "svg_patch_d", el.svg_patch_d);
            write_float_member(out, ef, "default_value", el.default_value);
            if (el.flash) { write_key(out, ef, "flash"); out << "true"; }
            // Overlay-control fields — emitted only when set (knobs stay lean).
            if (el.x != 0.0f) write_float_member(out, ef, "x", el.x);
            if (el.y != 0.0f) write_float_member(out, ef, "y", el.y);
            if (el.w != 0.0f) write_float_member(out, ef, "w", el.w);
            if (el.h != 0.0f) write_float_member(out, ef, "h", el.h);
            if (el.selected_index != 0)
                write_int_member(out, ef, "selected_index", el.selected_index);
            if (!el.placeholder.empty())
                write_string_member(out, ef, "placeholder", el.placeholder);
            if (!el.bg_color.empty()) write_string_member(out, ef, "bg_color", el.bg_color);
            if (!el.label.empty()) write_string_member(out, ef, "label", el.label);
            if (!el.options.empty()) {
                write_key(out, ef, "options");
                out << '[';
                for (size_t j = 0; j < el.options.size(); ++j) {
                    if (j) out << ',';
                    out << '"' << json_escape(el.options[j]) << '"';
                }
                out << ']';
            }
            // swap / action / xy_pad / value_label fields — emitted only when set.
            if (el.target_frame != -1)
                write_int_member(out, ef, "target_frame", el.target_frame);
            if (!el.action.empty()) write_string_member(out, ef, "action", el.action);
            if (!el.text.empty()) write_string_member(out, ef, "text", el.text);
            if (el.value_left_align) { write_key(out, ef, "value_left_align"); out << "true"; }
            if (el.kind == InteractiveElementKind::xy_pad)
                write_float_member(out, ef, "default_value_y", el.default_value_y);
            // Import report fields — emitted only when set (lean otherwise).
            if (el.resolution_rung != 0)
                write_int_member(out, ef, "resolution_rung", el.resolution_rung);
            if (el.confidence_score != 1.0f)
                write_float_member(out, ef, "confidence_score", el.confidence_score);
            if (!el.verification_pass) { write_key(out, ef, "verification_pass"); out << "false"; }
            if (!el.factory_id.empty()) write_string_member(out, ef, "factory_id", el.factory_id);
            if (!el.custom_props.empty()) write_string_member(out, ef, "custom_props", el.custom_props);
            if (!el.conflict_signals.empty()) {
                write_key(out, ef, "conflict_signals");
                out << '[';
                for (size_t j = 0; j < el.conflict_signals.size(); ++j) {
                    if (j) out << ',';
                    out << '"' << json_escape(el.conflict_signals[j]) << '"';
                }
                out << ']';
            }
            write_string_member(out, ef, "source_node_id", el.source_node_id);
            out << '}';
        }
        out << ']';
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

IRAssetManifest parse_asset_manifest(const choc::value::ValueView& obj) {
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
    if (!ir.font_family_assets.empty()) {
        write_key(out, first, "fontFamilyAssets");
        out << "[";
        bool ffirst = true;
        for (const auto& fa : ir.font_family_assets) {
            if (!ffirst) out << ",";
            ffirst = false;
            out << "{";
            bool mf = true;
            write_string_member(out, mf, "family", fa.family);
            if (!fa.style.empty()) write_string_member(out, mf, "style", fa.style);
            write_key(out, mf, "weight"); out << fa.weight;
            if (!fa.asset_id.empty()) write_string_member(out, mf, "asset_id", fa.asset_id);
            if (!fa.resolved_path.empty()) write_string_member(out, mf, "resolvedPath", fa.resolved_path);
            out << "}";
        }
        out << "]";
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
        for (const char* fk : {"fontFamilyAssets", "font_family_assets"}) {
            if (parsed.hasObjectMember(fk) && parsed[fk].isArray()) {
                auto arr = parsed[fk];
                for (uint32_t i = 0; i < arr.size(); ++i) {
                    auto e = arr[static_cast<int>(i)];
                    if (!e.isObject()) continue;
                    IRFontAsset fa;
                    fa.family = get_string(e, "family");
                    fa.style = get_string(e, "style");
                    if (e.hasObjectMember("weight"))
                        fa.weight = static_cast<int>(e["weight"].getWithDefault<int64_t>(400));
                    fa.asset_id = get_string(e, "asset_id");
                    if (fa.asset_id.empty()) fa.asset_id = get_string(e, "assetId");
                    fa.resolved_path = get_string(e, "resolvedPath");
                    if (fa.resolved_path.empty()) fa.resolved_path = get_string(e, "resolved_path");
                    if (!fa.family.empty()) ir.font_family_assets.push_back(std::move(fa));
                }
                break;
            }
        }
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

// ── import report ────────────────────────────────────────────────────────────
static void collect_report_visit(const IRNode& node, ImportReport& report,
                                 float low_confidence_threshold) {
    for (const auto& e : node.interactive_elements) {
        ImportReportEntry entry;
        entry.source_node_id = e.source_node_id.value_or("");
        entry.kind = interactive_kind_id(e.kind);
        entry.resolution_rung = e.resolution_rung;
        entry.confidence_score = e.confidence_score;
        entry.conflict_signals = e.conflict_signals;
        entry.verification_pass = e.verification_pass;
        if (!entry.conflict_signals.empty()) report.conflicted++;
        if (entry.confidence_score < low_confidence_threshold) report.low_confidence++;
        if (entry.resolution_rung == 5) report.unresolved++;  // inert (warn) rung
        report.controls.push_back(std::move(entry));
    }
    for (const auto& c : node.children)
        collect_report_visit(c, report, low_confidence_threshold);
}

ImportReport collect_import_report(const IRNode& root, float low_confidence_threshold) {
    ImportReport report;
    collect_report_visit(root, report, low_confidence_threshold);
    return report;
}

// Render-placement verification (structural half of the render-golden gate).
static int verify_placement_visit(IRNode& node, float fw, float fh) {
    int flagged = 0;
    for (auto& e : node.interactive_elements) {
        // A knob/fader/xy_pad carries its hit circle (hit_radius); the overlays
        // carry a box (w,h). A control with neither can't render anywhere.
        const bool has_box = e.w > 0.0f && e.h > 0.0f;
        const bool has_radius = e.hit_radius > 0.0f;
        std::string issue;
        if (!has_box && !has_radius) {
            issue = "control has no renderable extent (zero hit-radius and zero-area box)";
        } else if (fw > 0.0f && fh > 0.0f) {
            // Does the control's region fall ENTIRELY outside the frame [0,0,fw,fh]?
            float x0, y0, x1, y1;
            if (has_box) {
                x0 = e.x; y0 = e.y; x1 = e.x + e.w; y1 = e.y + e.h;
            } else {
                x0 = e.cx - e.hit_radius; y0 = e.cy - e.hit_radius;
                x1 = e.cx + e.hit_radius; y1 = e.cy + e.hit_radius;
            }
            if (x1 <= 0.0f || y1 <= 0.0f || x0 >= fw || y0 >= fh)
                issue = "control falls entirely outside the frame render region";
        }
        if (!issue.empty()) {
            e.verification_pass = false;
            e.conflict_signals.push_back(issue);
            ++flagged;
        }
    }
    // Overlays are checked against the ROOT faithful frame's region. The importer
    // emits interactive_elements only on the single top-level faithful_svg node
    // (children carry none), so the root frame is the correct coordinate space.
    // If nested faithful_svg nodes ever carry their own overlays, pass each such
    // node's own render dimensions here instead of inheriting the root's.
    for (auto& c : node.children) flagged += verify_placement_visit(c, fw, fh);
    return flagged;
}

int apply_placement_verification(IRNode& root, float frame_w, float frame_h) {
    return verify_placement_visit(root, frame_w, frame_h);
}

std::string import_report_to_json(const ImportReport& r) {
    std::ostringstream out;
    out << "{\"summary\":{\"total\":" << r.controls.size()
        << ",\"conflicted\":" << r.conflicted
        << ",\"low_confidence\":" << r.low_confidence
        << ",\"unresolved\":" << r.unresolved
        << ",\"ok\":" << (r.ok() ? "true" : "false") << "},\"controls\":[";
    for (size_t i = 0; i < r.controls.size(); ++i) {
        const auto& c = r.controls[i];
        if (i) out << ',';
        out << "{\"source_node_id\":\"" << json_escape(c.source_node_id) << "\""
            << ",\"kind\":\"" << c.kind << "\""
            << ",\"resolution_rung\":" << c.resolution_rung
            << ",\"confidence_score\":" << c.confidence_score
            << ",\"verification_pass\":" << (c.verification_pass ? "true" : "false")
            << ",\"conflict_signals\":[";
        for (size_t j = 0; j < c.conflict_signals.size(); ++j) {
            if (j) out << ',';
            out << '"' << json_escape(c.conflict_signals[j]) << '"';
        }
        out << "]}";
    }
    out << "]}";
    return out.str();
}

std::string import_report_to_text(const ImportReport& r) {
    std::ostringstream out;
    out << "import report: " << r.controls.size() << " control(s), "
        << r.conflicted << " conflicted, " << r.low_confidence << " low-confidence, "
        << r.unresolved << " unresolved (inert)\n";
    for (const auto& c : r.controls) {
        out << "  - " << (c.source_node_id.empty() ? "?" : c.source_node_id)
            << "  kind=" << c.kind << " rung=" << c.resolution_rung
            << " confidence=" << c.confidence_score
            << (c.verification_pass ? "" : " [verify-FAIL]") << '\n';
        for (const auto& cf : c.conflict_signals)
            out << "      conflict: " << cf << '\n';
    }
    return out.str();
}

}  // namespace pulp::view
