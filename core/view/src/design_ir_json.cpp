// design_ir_json.cpp — DesignIR JSON serialize / deserialize band.
//
// Extracted verbatim from design_import.cpp in the 2026-05-29 frontend-IR
// refactor (PR-1, planning/2026-05-29-frontend-ir-refactor-plan.md). This
// is a relocation-only split: the JSON helpers, serialize_design_ir, and
// parse_design_ir_json moved here unchanged. Four helpers
// (parse_ir_node, parse_ir_tokens, make_import_diagnostic,
// is_asset_reference_key) are promoted from static to external linkage
// because the asset pipeline / source parsers that remain in
// design_import.cpp call them; their declarations live in
// design_import_internal.hpp. promote_interactive_frames stays defined in
// design_import.cpp and is declared in that same header.

#include <pulp/view/design_import.hpp>

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
        float p = get_float(obj, "padding");
        l.padding_top = l.padding_right = l.padding_bottom = l.padding_left = p;
    }
    if (obj.hasObjectMember("paddingTop"))    l.padding_top = get_float(obj, "paddingTop");
    if (obj.hasObjectMember("paddingRight"))  l.padding_right = get_float(obj, "paddingRight");
    if (obj.hasObjectMember("paddingBottom")) l.padding_bottom = get_float(obj, "paddingBottom");
    if (obj.hasObjectMember("paddingLeft"))   l.padding_left = get_float(obj, "paddingLeft");
    if (obj.hasObjectMember("marginTop"))     l.margin_top = get_float(obj, "marginTop");
    if (obj.hasObjectMember("marginRight"))   l.margin_right = get_float(obj, "marginRight");
    if (obj.hasObjectMember("marginBottom"))  l.margin_bottom = get_float(obj, "marginBottom");
    if (obj.hasObjectMember("marginLeft"))    l.margin_left = get_float(obj, "marginLeft");

    auto parse_align = [](const std::string& s) -> LayoutAlign {
        if (s == "center")        return LayoutAlign::center;
        if (s == "flex-end" || s == "end") return LayoutAlign::flex_end;
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

IRNode parse_ir_node(const choc::value::ValueView& obj) {
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
        auto detected = explicit_audio_widget ? AudioWidgetType::none : detect_audio_widget(node.name);
        if (detected == AudioWidgetType::none && !node.type.empty() && !explicit_audio_widget)
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

}  // namespace pulp::view
