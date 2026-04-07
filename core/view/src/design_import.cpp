#include <pulp/view/design_import.hpp>
#include <choc/text/choc_JSON.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <cmath>
#include <map>
#include <unordered_set>

namespace pulp::view {

// ── Design source helpers ───────────────────────────────────────────────

std::optional<DesignSource> parse_design_source(const std::string& name) {
    if (name == "figma")  return DesignSource::figma;
    if (name == "stitch") return DesignSource::stitch;
    if (name == "v0")     return DesignSource::v0;
    if (name == "pencil") return DesignSource::pencil;
    return std::nullopt;
}

const char* design_source_name(DesignSource source) {
    switch (source) {
        case DesignSource::figma:  return "Figma";
        case DesignSource::stitch: return "Stitch";
        case DesignSource::v0:     return "v0";
        case DesignSource::pencil: return "Pencil";
    }
    return "unknown";
}

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

static int get_int(const choc::value::ValueView& obj, const char* key, int def = 0) {
    if (obj.hasObjectMember(key))
        return static_cast<int>(obj[key].getWithDefault<int64_t>(def));
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
    set_opt_str("color", s.color);
    set_opt_float("opacity", s.opacity);
    set_opt_float("borderRadius", s.border_radius);
    set_opt_str("border", s.border);
    set_opt_str("boxShadow", s.box_shadow);
    set_opt_str("filter", s.filter);
    set_opt_str("fontFamily", s.font_family);
    set_opt_float("fontSize", s.font_size);
    set_opt_int("fontWeight", s.font_weight);
    set_opt_str("fontStyle", s.font_style);
    set_opt_str("textAlign", s.text_align);
    set_opt_float("letterSpacing", s.letter_spacing);
    set_opt_float("lineHeight", s.line_height);
    set_opt_str("textTransform", s.text_transform);
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
    l.gap = get_float(obj, "gap");
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

static IRNode parse_ir_node(const choc::value::ValueView& obj) {
    IRNode node;
    node.type = get_string(obj, "type", "frame");
    node.name = get_string(obj, "name");
    node.text_content = get_string(obj, "content");

    if (obj.hasObjectMember("style"))
        node.style = parse_ir_style(obj["style"]);
    if (obj.hasObjectMember("layout"))
        node.layout = parse_ir_layout(obj["layout"]);

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
    return tokens;
}

// ── Source adapters ─────────────────────────────────────────────────────

DesignIR parse_figma_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::figma;

    auto root = choc::json::parse(json);

    // Figma export can be the IR format directly (from MCP or export tool)
    ir.root = parse_ir_node(root);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);

    return ir;
}

DesignIR parse_stitch_html(const std::string& html) {
    DesignIR ir;
    ir.source = DesignSource::stitch;

    // Try parsing as JSON IR first (from Stitch MCP get_screen)
    try {
        auto root = choc::json::parse(html);
        ir.root = parse_ir_node(root);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
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

    return ir;
}

DesignIR parse_v0_tsx(const std::string& tsx) {
    DesignIR ir;
    ir.source = DesignSource::v0;

    // Try parsing as JSON IR first (pre-processed by AI pipeline)
    try {
        auto root = choc::json::parse(tsx);
        ir.root = parse_ir_node(root);
        if (root.hasObjectMember("tokens"))
            ir.tokens = parse_ir_tokens(root["tokens"]);
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

    return ir;
}

DesignIR parse_pencil_json(const std::string& json) {
    DesignIR ir;
    ir.source = DesignSource::pencil;

    auto root = choc::json::parse(json);

    // Pencil MCP batch_get returns a node tree similar to our IR
    ir.root = parse_ir_node(root);

    if (root.hasObjectMember("tokens"))
        ir.tokens = parse_ir_tokens(root["tokens"]);
    if (root.hasObjectMember("variables"))
        ir.tokens = parse_ir_tokens(root["variables"]);

    return ir;
}

// ── Code generator ──────────────────────────────────────────────────────

static std::string indent(int level, int spaces) {
    return std::string(static_cast<size_t>(level * spaces), ' ');
}

static std::string sanitize_var(const std::string& name) {
    std::string result;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            result += c;
        else if (c == ' ' || c == '-' || c == '.')
            result += '_';
    }
    if (result.empty()) result = "el";
    if (std::isdigit(static_cast<unsigned char>(result[0])))
        result = "_" + result;
    return result;
}

static std::string format_px(float v) {
    if (v == std::floor(v))
        return std::to_string(static_cast<int>(v)) + "px";
    std::ostringstream ss;
    ss << v << "px";
    return ss.str();
}

static const char* audio_widget_type_name(AudioWidgetType t) {
    switch (t) {
        case AudioWidgetType::knob:     return "Knob";
        case AudioWidgetType::fader:    return "Fader";
        case AudioWidgetType::meter:    return "Meter";
        case AudioWidgetType::xy_pad:   return "XYPad";
        case AudioWidgetType::waveform: return "WaveformView";
        case AudioWidgetType::spectrum: return "SpectrumView";
        default: return nullptr;
    }
}

static const char* align_to_css(LayoutAlign a) {
    switch (a) {
        case LayoutAlign::flex_start:    return "flex-start";
        case LayoutAlign::flex_end:      return "flex-end";
        case LayoutAlign::center:        return "center";
        case LayoutAlign::stretch:       return "stretch";
        case LayoutAlign::space_between: return "space-between";
        case LayoutAlign::space_around:  return "space-around";
    }
    return "flex-start";
}

static void generate_node(std::ostringstream& ss, const IRNode& node,
                           const CodeGenOptions& opts, int depth,
                           int& var_counter, const std::string& parent_var) {
    std::string var = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) var += std::to_string(var_counter++);
    else var = opts.root_variable;

    std::string ind = indent(depth, opts.indent_spaces);

    // Audio widgets get special treatment
    if (node.audio_widget != AudioWidgetType::none) {
        auto widget_name = audio_widget_type_name(node.audio_widget);
        if (widget_name) {
            if (opts.include_comments && !node.audio_label.empty())
                ss << ind << "// " << node.audio_label << " " << widget_name << "\n";

            ss << ind << "const " << var << " = create" << widget_name << "({\n";
            if (!node.audio_label.empty())
                ss << ind << "  label: '" << node.audio_label << "',\n";
            ss << ind << "  min: " << node.audio_min << ",\n";
            ss << ind << "  max: " << node.audio_max << ",\n";
            ss << ind << "  defaultValue: " << node.audio_default << "\n";
            ss << ind << "});\n";

            if (!parent_var.empty())
                ss << ind << parent_var << ".appendChild(" << var << ");\n";
            ss << "\n";
            return;
        }
    }

    // Determine HTML tag
    std::string tag = "div";
    if (node.type == "text")    tag = "span";
    if (node.type == "button")  tag = "button";
    if (node.type == "input")   tag = "input";
    if (node.type == "image")   tag = "img";
    if (node.type == "label")   tag = "span";

    if (opts.include_comments && !node.name.empty() && depth > 0)
        ss << ind << "// " << node.name << "\n";

    ss << ind << "const " << var << " = document.createElement('" << tag << "');\n";

    // Apply layout styles for container nodes
    if (!node.children.empty() || node.type == "frame") {
        ss << ind << var << ".style.display = 'flex';\n";
        ss << ind << var << ".style.flexDirection = '"
           << (node.layout.direction == LayoutDirection::row ? "row" : "column") << "';\n";

        if (node.layout.gap > 0)
            ss << ind << var << ".style.gap = '" << format_px(node.layout.gap) << "';\n";

        // Padding
        bool uniform_padding = (node.layout.padding_top == node.layout.padding_right &&
                                 node.layout.padding_right == node.layout.padding_bottom &&
                                 node.layout.padding_bottom == node.layout.padding_left);
        if (uniform_padding && node.layout.padding_top > 0) {
            ss << ind << var << ".style.padding = '" << format_px(node.layout.padding_top) << "';\n";
        } else {
            if (node.layout.padding_top > 0)
                ss << ind << var << ".style.paddingTop = '" << format_px(node.layout.padding_top) << "';\n";
            if (node.layout.padding_right > 0)
                ss << ind << var << ".style.paddingRight = '" << format_px(node.layout.padding_right) << "';\n";
            if (node.layout.padding_bottom > 0)
                ss << ind << var << ".style.paddingBottom = '" << format_px(node.layout.padding_bottom) << "';\n";
            if (node.layout.padding_left > 0)
                ss << ind << var << ".style.paddingLeft = '" << format_px(node.layout.padding_left) << "';\n";
        }

        if (node.layout.justify != LayoutAlign::flex_start)
            ss << ind << var << ".style.justifyContent = '" << align_to_css(node.layout.justify) << "';\n";
        if (node.layout.align != LayoutAlign::stretch)
            ss << ind << var << ".style.alignItems = '" << align_to_css(node.layout.align) << "';\n";
        if (node.layout.wrap)
            ss << ind << var << ".style.flexWrap = 'wrap';\n";

        // Sizing
        if (node.layout.width_mode == SizingMode::fill)
            ss << ind << var << ".style.flexGrow = '1';\n";
        if (node.layout.height_mode == SizingMode::fill)
            ss << ind << var << ".style.flexGrow = '1';\n";
    }

    // Apply visual styles
    auto& s = node.style;
    auto emit_str = [&](const char* prop, const std::optional<std::string>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << *val << "';\n";
    };
    auto emit_px = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << format_px(*val) << "';\n";
    };
    auto emit_float = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << *val << "';\n";
    };

    emit_str("backgroundColor", s.background_color);
    if (s.background_gradient)
        ss << ind << var << ".style.background = '" << *s.background_gradient << "';\n";
    emit_str("color", s.color);
    emit_float("opacity", s.opacity);
    emit_px("borderRadius", s.border_radius);
    emit_str("border", s.border);
    emit_str("boxShadow", s.box_shadow);
    emit_str("filter", s.filter);
    emit_str("fontFamily", s.font_family);
    emit_px("fontSize", s.font_size);
    if (s.font_weight)
        ss << ind << var << ".style.fontWeight = '" << *s.font_weight << "';\n";
    emit_str("fontStyle", s.font_style);
    emit_str("textAlign", s.text_align);
    emit_px("letterSpacing", s.letter_spacing);
    emit_float("lineHeight", s.line_height);
    emit_str("textTransform", s.text_transform);
    emit_str("overflow", s.overflow);
    emit_str("cursor", s.cursor);
    emit_str("position", s.position);
    emit_px("top", s.top);
    emit_px("left", s.left);
    emit_px("right", s.right);
    emit_px("bottom", s.bottom);
    if (s.z_index)
        ss << ind << var << ".style.zIndex = '" << *s.z_index << "';\n";
    emit_str("transform", s.transform);
    emit_px("width", s.width);
    emit_px("height", s.height);
    emit_px("minWidth", s.min_width);
    emit_px("minHeight", s.min_height);
    emit_px("maxWidth", s.max_width);
    emit_px("maxHeight", s.max_height);

    // Text content
    if (!node.text_content.empty())
        ss << ind << var << ".textContent = '" << node.text_content << "';\n";

    // Append to parent
    if (!parent_var.empty())
        ss << ind << parent_var << ".appendChild(" << var << ");\n";

    ss << "\n";

    // Recurse into children
    for (auto& child : node.children)
        generate_node(ss, child, opts, depth + 1, var_counter, var);
}

// ── Native Pulp API code generator ──────────────────────────────────────
// Uses createCol/createRow/createKnob/setFlex — the native widget bridge API.
// Encodes Yoga layout constraints learned from testing:
//   - Every container MUST have explicit height, min_height, or flex_grow
//   - Labels inside containers need min_height (14px text, 12px small)
//   - Faders need min_width >= 40 for thumb rendering
//   - Meters need min_width >= 20 for bar visibility
//   - Knobs need width and height >= 56 for arc rendering

// Minimum sizes for Yoga layout (prevents zero-height collapse)
static constexpr float kMinLabelHeight = 14.0f;
static constexpr float kMinSmallLabelHeight = 12.0f;
static constexpr float kMinRowHeight = 16.0f;
static constexpr float kMinKnobSize = 56.0f;
static constexpr float kMinFaderWidth = 40.0f;
static constexpr float kMinFaderHeight = 80.0f;
static constexpr float kMinMeterWidth = 20.0f;
static constexpr float kMinMeterHeight = 80.0f;

// Compute the actual rendered height of a node from its Pencil/IR data.
// Uses exact child dimensions — no guessing.
static float compute_node_height(const IRNode& node);

static float compute_container_height(const IRNode& node) {
    bool is_row = (node.layout.direction == LayoutDirection::row);
    float gap = node.layout.gap;
    float pad = node.layout.padding_top + node.layout.padding_bottom;

    if (is_row) {
        float max_h = 0;
        for (auto& child : node.children)
            max_h = std::max(max_h, compute_node_height(child));
        return std::max(max_h + pad, kMinRowHeight);
    } else {
        float h = 0;
        for (size_t i = 0; i < node.children.size(); ++i) {
            if (i > 0) h += gap;
            h += compute_node_height(node.children[i]);
        }
        return std::max(h + pad, kMinRowHeight);
    }
}

static float compute_node_height(const IRNode& node) {
    // Audio widget frames: the generated wrapper column contains the widget +
    // separate labels. Compute from children's actual dimensions.
    if (node.audio_widget != AudioWidgetType::none) {
        float widget_h = 0;
        float labels_h = 0;
        float gap = node.layout.gap > 0 ? node.layout.gap : 8;
        int n_items = 0;

        for (auto& c : node.children) {
            if (c.type == "ellipse" || c.type == "rectangle") {
                widget_h = std::max(widget_h, c.style.height.value_or(kMinFaderHeight));
                n_items++;
            } else if (c.type == "text" || c.type == "label") {
                labels_h += kMinLabelHeight;
                n_items++;
            }
        }
        // Minimums if no children found
        if (widget_h == 0) widget_h = (node.audio_widget == AudioWidgetType::knob) ? kMinKnobSize : kMinFaderHeight;
        if (n_items == 0) n_items = 1;

        return widget_h + labels_h + gap * static_cast<float>(n_items);
    }

    // Non-audio node with explicit height
    if (node.style.height) return *node.style.height;

    // Text/label: font-dependent height
    if (node.type == "text" || node.type == "label")
        return node.style.font_size.value_or(14.0f) * 1.4f;

    // Container with children: compute recursively
    if (!node.children.empty())
        return compute_container_height(node);

    // Leaf node without dimensions
    return kMinRowHeight;
}

static void generate_native_node(std::ostringstream& ss, const IRNode& node,
                                  const CodeGenOptions& opts, int depth,
                                  int& var_counter, const std::string& parent_id) {
    std::string id = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) id += std::to_string(var_counter++);
    else id = opts.root_variable;

    std::string ind = indent(depth, opts.indent_spaces);
    std::string pid = parent_id.empty() ? "''" : ("'" + parent_id + "'");

    // Audio widgets use native widget API
    if (node.audio_widget != AudioWidgetType::none) {
        auto wtype = node.audio_widget;

        // Extract label text, value text, and stroke color from child nodes
        std::string label_text = node.audio_label;
        std::string value_text;
        std::string stroke_color;  // Per-widget stroke color from child ellipse
        for (auto& child : node.children) {
            if (child.type == "text" || child.type == "label") {
                if (label_text.empty() && !child.text_content.empty())
                    label_text = child.text_content;
                else if (!child.text_content.empty() && child.text_content != label_text)
                    value_text = child.text_content;
            }
            // Extract stroke color from child ellipse/rectangle for per-knob coloring
            if ((child.type == "ellipse" || child.type == "rectangle") && stroke_color.empty()) {
                if (child.attributes.count("stroke_color"))
                    stroke_color = child.attributes.at("stroke_color");
            }
        }

        if (opts.include_comments && !label_text.empty())
            ss << ind << "// " << label_text << "\n";

        // Helper: emit setWidgetStyle if preview mode
        auto emit_style = [&](const std::string& wid) {
            if (opts.preview_mode)
                ss << ind << "setWidgetStyle('" << wid << "', 'minimal');\n";
        };

        // Create a wrapper column for the widget + value label
        std::string col_id = id + "_col";
        ss << ind << "createCol('" << col_id << "', " << pid << ");\n";
        ss << ind << "setFlex('" << col_id << "', 'align_items', 'center');\n";
        ss << ind << "setFlex('" << col_id << "', 'gap', 4);\n";

        if (wtype == AudioWidgetType::knob) {
            // shape_width/height = child ellipse size (widget), frame width = column container
            float shape_w = kMinKnobSize, shape_h = kMinKnobSize;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float frame_w = node.style.width.value_or(shape_w + 20);
            float col_h = shape_h + 20 + (value_text.empty() ? 0 : 16);
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createKnob('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << shape_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Clear built-in label — use separate Yoga-positioned labels for exact placement
            ss << ind << "setLabel('" << id << "', ' ');\n";
            ss << ind << "setValue('" << id << "', " << node.audio_default << ");\n";
            emit_style(id);
            // Per-knob stroke color from child ellipse (used by minimal paint path)
            if (!stroke_color.empty())
                ss << ind << "setBorder('" << id << "', '" << stroke_color << "', 2.5, " << (shape_w * 0.5f) << ");\n";
            // Labels below knob — center-aligned, Yoga-positioned
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
            if (!value_text.empty()) {
                std::string val_id = id + "_val";
                ss << ind << "createLabel('" << val_id << "', '" << value_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << val_id << "', 'height', " << kMinSmallLabelHeight << ");\n";
                ss << ind << "setFontSize('" << val_id << "', 10);\n";
                ss << ind << "setTextColor('" << val_id << "', '#6c7086');\n";
                ss << ind << "setTextAlign('" << val_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::fader) {
            // shape_width/height from child rectangle, frame width for column
            float frame_w = node.style.width.value_or(kMinFaderWidth);
            float shape_w = frame_w;
            float shape_h = kMinFaderHeight;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            // Use frame width for column, but ensure widget is at least usable
            float widget_w = std::max(shape_w, 6.0f);
            float col_h = shape_h + 20;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createFader('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Fader label overlaps track when rendered inside bounds — use separate label
            ss << ind << "setLabel('" << id << "', ' ');\n";  // Clear built-in label
            ss << ind << "setValue('" << id << "', " << node.audio_default << ");\n";
            emit_style(id);
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::meter) {
            float frame_w = node.style.width.value_or(kMinMeterWidth);
            float shape_w = frame_w;
            float shape_h = kMinMeterHeight;
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float widget_w = std::max(shape_w, 8.0f);
            float col_h = shape_h + 20;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            ss << ind << "createMeter('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            ss << ind << "setMeterLevel('" << id << "', -6);\n";
            emit_style(id);
            // Meter has no setLabel — always add a separate label
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << label_text << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
        }
        else if (wtype == AudioWidgetType::xy_pad) {
            float sz = std::max(node.style.width.value_or(100.0f), 80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (sz + 20) << ");\n";
            ss << ind << "createXYPad('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << sz << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << sz << ");\n";
        }
        else if (wtype == AudioWidgetType::waveform || wtype == AudioWidgetType::spectrum) {
            float w = node.style.width.value_or(200.0f);
            float h = node.style.height.value_or(80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (h + 20) << ");\n";
            auto fn = (wtype == AudioWidgetType::waveform) ? "createWaveform" : "createSpectrum";
            ss << ind << fn << "('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << h << ");\n";
        }

        ss << "\n";
        return;
    }

    // Container or text node
    bool is_container = !node.children.empty() || node.type == "frame";
    bool is_text = (node.type == "text" || node.type == "label");
    bool is_row = (node.layout.direction == LayoutDirection::row);

    if (is_text) {
        // Text node → createLabel with explicit height (Yoga requirement)
        ss << ind << "createLabel('" << id << "', '" << node.text_content << "', " << pid << ");\n";

        float font_h = node.style.font_size.value_or(14.0f);
        float label_h = std::max(font_h * 1.4f, kMinLabelHeight);
        ss << ind << "setFlex('" << id << "', 'height', " << label_h << ");\n";

        if (node.style.font_size)
            ss << ind << "setFontSize('" << id << "', " << *node.style.font_size << ");\n";
        if (node.style.font_weight)
            ss << ind << "setFontWeight('" << id << "', '" << *node.style.font_weight << "');\n";
        if (node.style.color)
            ss << ind << "setTextColor('" << id << "', '" << *node.style.color << "');\n";

        ss << "\n";
        return;
    }

    if (is_container) {
        // Container → createRow or createCol
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";

        ss << ind << (is_row ? "createRow" : "createCol")
           << "('" << id << "', " << pid << ");\n";

        // Yoga: every container MUST have explicit height
        // Priority: _layoutHeight (from snapshot_layout) > style.height > fill > computed
        if (node.attributes.count("_layoutHeight")) {
            int lh = std::stoi(node.attributes.at("_layoutHeight"));
            if (lh > 0) ss << ind << "setFlex('" << id << "', 'height', " << lh << ");\n";
        } else if (node.style.height) {
            ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
        } else if (node.layout.height_mode == SizingMode::fill) {
            ss << ind << "setFlex('" << id << "', 'flex_grow', 1);\n";
        } else {
            float est = compute_container_height(node);
            ss << ind << "setFlex('" << id << "', 'height', " << est << ");\n";
        }

        // Width: use _layoutWidth if available, then style.width
        if (node.attributes.count("_layoutWidth")) {
            int lw = std::stoi(node.attributes.at("_layoutWidth"));
            if (lw > 0) ss << ind << "setFlex('" << id << "', 'width', " << lw << ");\n";
        } else if (node.style.width) {
            ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
        }

        if (node.layout.gap > 0)
            ss << ind << "setFlex('" << id << "', 'gap', " << node.layout.gap << ");\n";

        // Padding
        bool uniform = (node.layout.padding_top == node.layout.padding_right &&
                         node.layout.padding_right == node.layout.padding_bottom &&
                         node.layout.padding_bottom == node.layout.padding_left);
        if (uniform && node.layout.padding_top > 0)
            ss << ind << "setFlex('" << id << "', 'padding', " << node.layout.padding_top << ");\n";
        else {
            if (node.layout.padding_top > 0)
                ss << ind << "setFlex('" << id << "', 'padding_top', " << node.layout.padding_top << ");\n";
            if (node.layout.padding_right > 0)
                ss << ind << "setFlex('" << id << "', 'padding_right', " << node.layout.padding_right << ");\n";
            if (node.layout.padding_bottom > 0)
                ss << ind << "setFlex('" << id << "', 'padding_bottom', " << node.layout.padding_bottom << ");\n";
            if (node.layout.padding_left > 0)
                ss << ind << "setFlex('" << id << "', 'padding_left', " << node.layout.padding_left << ");\n";
        }

        if (node.layout.justify != LayoutAlign::flex_start)
            ss << ind << "setFlex('" << id << "', 'justify_content', '" << align_to_css(node.layout.justify) << "');\n";
        if (node.layout.align != LayoutAlign::stretch)
            ss << ind << "setFlex('" << id << "', 'align_items', '" << align_to_css(node.layout.align) << "');\n";

        // Visual styles
        if (node.style.background_color)
            ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
        if (node.style.border_radius)
            ss << ind << "setCornerRadius('" << id << "', 'All', " << *node.style.border_radius << ");\n";

        ss << "\n";

        // Recurse children
        // For space_between rows, right-align the last text child
        bool is_space_between = is_row && node.layout.justify == LayoutAlign::space_between;
        std::string last_text_child_id;
        for (auto& child : node.children) {
            // Capture the var_counter before generation to reconstruct this child's ID
            if (is_space_between && (child.type == "text" || child.type == "label")) {
                std::string child_id = sanitize_var(child.name.empty() ? child.type : child.name);
                child_id += std::to_string(var_counter);  // counter value before this child runs
                last_text_child_id = child_id;
            }
            generate_native_node(ss, child, opts, depth + 1, var_counter, id);
        }
        // Right-align the last label in space_between rows
        if (!last_text_child_id.empty()) {
            std::string child_ind = indent(depth + 1, opts.indent_spaces);
            ss << child_ind << "setTextAlign('" << last_text_child_id << "', 'right');\n\n";
        }

        return;
    }

    // Generic frame without children (divider, spacer, etc.)
    ss << ind << "createRow('" << id << "', " << pid << ");\n";
    if (node.style.height)
        ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
    else
        ss << ind << "setFlex('" << id << "', 'height', 1);\n";
    if (node.style.background_color)
        ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
    ss << "\n";
}

// ── Public code generation ──────────────────────────────────────────────

std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts) {
    std::ostringstream ss;

    if (opts.include_comments) {
        ss << "// Generated by Pulp import-design from " << design_source_name(ir.source) << "\n";
        if (!ir.source_file.empty())
            ss << "// Source: " << ir.source_file << "\n";
        ss << "\n";
    }

    ss << "setTheme('dark');\n\n";

    // Token assignments
    if (opts.include_tokens && (!ir.tokens.colors.empty() ||
                                 !ir.tokens.dimensions.empty() ||
                                 !ir.tokens.strings.empty())) {
        if (opts.include_comments)
            ss << "// Design tokens\n";

        if (opts.mode == CodeGenMode::native) {
            for (auto& [name, value] : ir.tokens.colors)
                ss << "setColorToken('" << name << "', '" << value << "');\n";
            for (auto& [name, value] : ir.tokens.dimensions)
                ss << "setDimensionToken('" << name << "', " << value << ");\n";
        } else {
            for (auto& [name, value] : ir.tokens.colors)
                ss << "theme.colors[\"" << name << "\"] = '" << value << "';\n";
            for (auto& [name, value] : ir.tokens.dimensions)
                ss << "theme.dimensions[\"" << name << "\"] = " << value << ";\n";
            for (auto& [name, value] : ir.tokens.strings)
                ss << "theme.strings[\"" << name << "\"] = '" << value << "';\n";
        }

        ss << "\n";
    }

    if (opts.mode == CodeGenMode::native) {
        // Native Pulp API
        int var_counter = 0;
        generate_native_node(ss, ir.root, opts, 0, var_counter, "");
        ss << "void 0;\n";
    } else {
        // Web-compat DOM API
        int var_counter = 0;
        generate_node(ss, ir.root, opts, 0, var_counter, "");
        ss << "document.body.appendChild(" << opts.root_variable << ");\n";
    }

    return ss.str();
}

// ── W3C Design Tokens ───────────────────────────────────────────────────

static Color parse_hex_color_str(const std::string& hex) {
    if (hex.empty() || hex[0] != '#') return {};
    try {
        auto val = std::stoul(hex.substr(1), nullptr, 16);
        if (hex.size() == 7)
            return color_from_hex(static_cast<uint32_t>(val));
        if (hex.size() == 9)
            return color_from_hex_alpha(static_cast<uint32_t>(val));
    } catch (...) {}
    return {};
}

Theme parse_w3c_tokens(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // W3C/DTCG Design Tokens Format:
    // - Top-level groups with $type (inherited by children)
    // - Nested tokens with $value/$type
    // - Alias references: { "$value": "{color.primary}" } resolve to other tokens
    // - $description stored but not used in Theme (available for documentation)

    // Evaluate simple math expressions: "8 * 2" → "16", "{spacing.base} * 2" after alias resolution
    auto eval_math = [](const std::string& expr) -> std::string {
        // Only handle simple "number op number" patterns
        auto trim = [](const std::string& s) -> std::string {
            auto a = s.find_first_not_of(" \t");
            auto b = s.find_last_not_of(" \t");
            return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
        };
        std::string s = trim(expr);
        // Strip trailing units (px, rem, em)
        std::string unit;
        for (auto& u : {"px", "rem", "em"}) {
            if (s.size() > 2 && s.substr(s.size() - std::strlen(u)) == u) {
                unit = u;
                s = trim(s.substr(0, s.size() - std::strlen(u)));
                break;
            }
        }
        // Find operator
        for (char op : {'*', '+', '-', '/'}) {
            auto pos = s.find(op);
            if (pos != std::string::npos && pos > 0 && pos < s.size() - 1) {
                try {
                    float a = std::stof(trim(s.substr(0, pos)));
                    float b = std::stof(trim(s.substr(pos + 1)));
                    float result = 0;
                    if (op == '*') result = a * b;
                    else if (op == '+') result = a + b;
                    else if (op == '-') result = a - b;
                    else if (op == '/' && b != 0) result = a / b;
                    // Return as clean number string
                    if (result == std::floor(result))
                        return std::to_string(static_cast<int>(result));
                    std::ostringstream oss;
                    oss << result;
                    return oss.str();
                } catch (...) {}
            }
        }
        return expr;  // Not a math expression, return as-is
    };

    // First pass: collect all raw token values (including unresolved aliases)
    struct RawToken { std::string type; std::string value; };
    std::unordered_map<std::string, RawToken> raw_tokens;

    std::function<void(const choc::value::ValueView&, const std::string&, const std::string&)> walk;
    walk = [&](const choc::value::ValueView& obj, const std::string& prefix,
               const std::string& inherited_type) {
        if (!obj.isObject()) return;

        // Group-level $type applies to all children without their own $type
        auto group_type = get_string(obj, "$type", "");
        if (group_type.empty()) group_type = inherited_type;

        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            std::string key(member.name);
            if (key.empty() || key[0] == '$') continue;

            auto& val = member.value;
            std::string full_name = prefix.empty() ? key : prefix + "." + key;

            // Leaf token: has $value
            if (val.isObject() && val.hasObjectMember("$value")) {
                auto type_str = get_string(val, "$type", "");
                if (type_str.empty()) type_str = group_type;

                auto dollar_val = val["$value"];

                // Composite tokens: typography, shadow, border have object $value
                if (dollar_val.isObject()) {
                    if (type_str == "typography") {
                        // Flatten: typography.fontFamily, typography.fontSize, etc.
                        if (dollar_val.hasObjectMember("fontFamily"))
                            raw_tokens[full_name + ".fontFamily"] = { "fontFamily", std::string(dollar_val["fontFamily"].toString()) };
                        if (dollar_val.hasObjectMember("fontSize"))
                            raw_tokens[full_name + ".fontSize"] = { "dimension", std::string(dollar_val["fontSize"].toString()) };
                        if (dollar_val.hasObjectMember("fontWeight"))
                            raw_tokens[full_name + ".fontWeight"] = { "number", std::string(dollar_val["fontWeight"].toString()) };
                        if (dollar_val.hasObjectMember("lineHeight"))
                            raw_tokens[full_name + ".lineHeight"] = { "number", std::string(dollar_val["lineHeight"].toString()) };
                        if (dollar_val.hasObjectMember("letterSpacing"))
                            raw_tokens[full_name + ".letterSpacing"] = { "dimension", std::string(dollar_val["letterSpacing"].toString()) };
                    } else if (type_str == "shadow") {
                        // Flatten shadow: offsetX, offsetY, blur, spread, color
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("offsetX"))
                            raw_tokens[full_name + ".offsetX"] = { "dimension", std::string(dollar_val["offsetX"].toString()) };
                        if (dollar_val.hasObjectMember("offsetY"))
                            raw_tokens[full_name + ".offsetY"] = { "dimension", std::string(dollar_val["offsetY"].toString()) };
                        if (dollar_val.hasObjectMember("blur"))
                            raw_tokens[full_name + ".blur"] = { "dimension", std::string(dollar_val["blur"].toString()) };
                        if (dollar_val.hasObjectMember("spread"))
                            raw_tokens[full_name + ".spread"] = { "dimension", std::string(dollar_val["spread"].toString()) };
                    } else if (type_str == "border") {
                        if (dollar_val.hasObjectMember("color"))
                            raw_tokens[full_name + ".color"] = { "color", std::string(dollar_val["color"].toString()) };
                        if (dollar_val.hasObjectMember("width"))
                            raw_tokens[full_name + ".width"] = { "dimension", std::string(dollar_val["width"].toString()) };
                        if (dollar_val.hasObjectMember("style"))
                            raw_tokens[full_name + ".style"] = { "string", std::string(dollar_val["style"].toString()) };
                    } else {
                        // Unknown composite — store as string
                        raw_tokens[full_name] = { type_str, choc::json::toString(dollar_val) };
                    }
                } else {
                    // Simple scalar $value
                    auto value_str = std::string(dollar_val.toString());
                    raw_tokens[full_name] = { type_str, value_str };
                }
            } else if (val.isObject()) {
                // Nested group — recurse with inherited type
                walk(val, full_name, group_type);
            }
        }
    };

    walk(root, "", "");

    // Second pass: resolve aliases (values like "{color.primary}" or "{spacing.base} * 2")
    // Handles both whole-value aliases and partial aliases embedded in expressions
    // Supports up to 10 levels of chained aliases to prevent infinite loops
    auto resolve_value = [&](const std::string& value) -> std::string {
        std::string resolved = value;
        std::unordered_set<std::string> visited;  // Cycle detection
        for (int depth = 0; depth < 10; ++depth) {
            // Find any {reference.path} pattern in the string
            auto open = resolved.find('{');
            auto close = resolved.find('}');
            if (open == std::string::npos || close == std::string::npos || close <= open)
                break;

            auto ref = resolved.substr(open + 1, close - open - 1);
            if (visited.count(ref))
                break;  // Circular alias — stop resolving
            visited.insert(ref);

            auto it = raw_tokens.find(ref);
            if (it != raw_tokens.end()) {
                // Replace {ref} with resolved value
                resolved = resolved.substr(0, open) + it->second.value + resolved.substr(close + 1);
                continue;  // May contain more aliases
            }
            break;  // Reference not found
        }
        return resolved;
    };

    // Third pass: resolve aliases, evaluate math, store into Theme
    for (auto& [name, token] : raw_tokens) {
        auto value_str = eval_math(resolve_value(token.value));
        auto& type_str = token.type;

        if (type_str == "color") {
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            }
        } else if (type_str == "dimension") {
            float v = 0;
            try { v = std::stof(value_str); } catch (...) {}
            theme.dimensions[name] = v;
        } else if (type_str == "fontFamily" || type_str == "string") {
            theme.strings[name] = value_str;
        } else if (type_str == "number") {
            float v = 0;
            try { v = std::stof(value_str); } catch (...) {}
            theme.dimensions[name] = v;
        } else {
            // Infer type from resolved value
            if (!value_str.empty() && value_str[0] == '#') {
                theme.colors[name] = parse_hex_color_str(value_str);
            } else {
                try {
                    float v = std::stof(value_str);
                    theme.dimensions[name] = v;
                } catch (...) {
                    theme.strings[name] = value_str;
                }
            }
        }
    }

    return theme;
}

std::string export_w3c_tokens(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Group tokens by prefix (before the dot)
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> color_groups;
    for (auto& [name, color] : theme.colors) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "color";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;

        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());

        color_groups[group].emplace_back(key, buf);
    }

    std::map<std::string, std::vector<std::pair<std::string, float>>> dim_groups;
    for (auto& [name, value] : theme.dimensions) {
        auto dot = name.find('.');
        std::string group = dot != std::string::npos ? name.substr(0, dot) : "dimension";
        std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
        dim_groups[group].emplace_back(key, value);
    }

    bool first_group = true;

    // Colors
    for (auto& [group, entries] : color_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, hex] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << hex << "\", \"$type\": \"color\" }";
        }
        ss << "\n  }";
    }

    // Dimensions
    for (auto& [group, entries] : dim_groups) {
        if (!first_group) ss << ",\n";
        first_group = false;
        ss << "  \"" << group << "\": {\n";
        bool first = true;
        for (auto& [key, val] : entries) {
            if (!first) ss << ",\n";
            first = false;
            ss << "    \"" << key << "\": { \"$value\": \"" << val << "\", \"$type\": \"dimension\" }";
        }
        ss << "\n  }";
    }

    // Strings
    if (!theme.strings.empty()) {
        if (!first_group) ss << ",\n";
        ss << "  \"string\": {\n";
        bool first = true;
        for (auto& [name, value] : theme.strings) {
            if (!first) ss << ",\n";
            first = false;
            auto dot = name.find('.');
            std::string key = dot != std::string::npos ? name.substr(dot + 1) : name;
            ss << "    \"" << key << "\": { \"$value\": \"" << value << "\", \"$type\": \"string\" }";
        }
        ss << "\n  }";
    }

    ss << "\n}\n";
    return ss.str();
}

Theme ir_tokens_to_theme(const IRTokens& tokens) {
    Theme theme;
    for (auto& [name, hex] : tokens.colors)
        theme.colors[name] = parse_hex_color_str(hex);
    theme.dimensions = tokens.dimensions;
    theme.strings = tokens.strings;
    return theme;
}

IRTokens theme_to_ir_tokens(const Theme& theme) {
    IRTokens tokens;
    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        tokens.colors[name] = buf;
    }
    tokens.dimensions = theme.dimensions;
    tokens.strings = theme.strings;
    return tokens;
}

// ── Figma Variables sync ────────────────────────────────────────────────

Theme parse_figma_variables(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Figma Variables JSON structure (from MCP get_variable_defs):
    // { "variables": [ { "name": "color/primary", "resolvedValue": "#89B4FA",
    //                     "type": "COLOR" }, ... ],
    //   "collections": [ { "name": "Tokens", "modes": [...] } ] }
    // OR flat array of variables

    auto parse_vars = [&](const choc::value::ValueView& vars) {
        for (uint32_t i = 0; i < vars.size(); ++i) {
            auto v = vars[static_cast<int>(i)];
            auto name = get_string(v, "name", "");
            auto type = get_string(v, "type", "");
            if (name.empty()) continue;

            // Figma uses slash-separated paths: "color/primary" → "color.primary"
            std::string dotted = name;
            for (auto& c : dotted) if (c == '/') c = '.';

            auto resolved = get_string(v, "resolvedValue", "");
            if (resolved.empty() && v.hasObjectMember("value"))
                resolved = get_string(v, "value", "");

            if (type == "COLOR" || type == "color") {
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
            } else if (type == "FLOAT" || type == "float" || type == "number") {
                try { theme.dimensions[dotted] = std::stof(resolved); } catch (...) {}
            } else if (type == "STRING" || type == "string") {
                theme.strings[dotted] = resolved;
            } else {
                // Infer from value
                if (!resolved.empty() && resolved[0] == '#')
                    theme.colors[dotted] = parse_hex_color_str(resolved);
                else {
                    try { theme.dimensions[dotted] = std::stof(resolved); }
                    catch (...) { theme.strings[dotted] = resolved; }
                }
            }
        }
    };

    if (root.hasObjectMember("variables") && root["variables"].isArray())
        parse_vars(root["variables"]);
    else if (root.isArray())
        parse_vars(root);

    return theme;
}

std::string export_figma_variables(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n  \"variables\": [\n";

    bool first = true;
    auto emit = [&](const std::string& name, const std::string& type, const std::string& value) {
        if (!first) ss << ",\n";
        first = false;
        // Convert dot-separated to slash-separated for Figma
        std::string figma_name = name;
        for (auto& c : figma_name) if (c == '.') c = '/';
        ss << "    { \"name\": \"" << figma_name << "\", \"type\": \"" << type
           << "\", \"value\": \"" << value << "\" }";
    };

    for (auto& [name, color] : theme.colors) {
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        emit(name, "COLOR", buf);
    }
    for (auto& [name, value] : theme.dimensions) {
        std::ostringstream vs;
        if (value == std::floor(value)) vs << static_cast<int>(value);
        else vs << value;
        emit(name, "FLOAT", vs.str());
    }
    for (auto& [name, value] : theme.strings)
        emit(name, "STRING", value);

    ss << "\n  ]\n}\n";
    return ss.str();
}

// ── Stitch Design System sync ──────────────────────────────────────────

Theme parse_stitch_design_system(const std::string& json) {
    Theme theme;
    auto root = choc::json::parse(json);

    // Stitch Design System JSON (from MCP list_design_systems):
    // { "name": "My Theme",
    //   "colors": { "primary": "#89B4FA", "background": "#1E1E2E", ... },
    //   "fonts": { "heading": "Inter", "body": "Roboto" },
    //   "roundness": "medium",
    //   "spacing": 8 }

    if (root.hasObjectMember("colors")) {
        auto colors = root["colors"];
        for (uint32_t i = 0; i < colors.size(); ++i) {
            auto m = colors.getObjectMemberAt(i);
            auto hex = std::string(m.value.toString());
            if (!hex.empty() && hex[0] == '#')
                theme.colors[std::string("color.") + std::string(m.name)] = parse_hex_color_str(hex);
        }
    }

    if (root.hasObjectMember("fonts")) {
        auto fonts = root["fonts"];
        for (uint32_t i = 0; i < fonts.size(); ++i) {
            auto m = fonts.getObjectMemberAt(i);
            theme.strings[std::string("font.") + std::string(m.name)] = std::string(m.value.toString());
        }
    }

    if (root.hasObjectMember("roundness")) {
        auto r = get_string(root, "roundness", "medium");
        float radius = 8.0f;
        if (r == "none") radius = 0;
        else if (r == "small") radius = 4;
        else if (r == "medium") radius = 8;
        else if (r == "large") radius = 16;
        else if (r == "full") radius = 999;
        else { try { radius = std::stof(r); } catch (...) {} }
        theme.dimensions["roundness"] = radius;
    }

    if (root.hasObjectMember("spacing")) {
        theme.dimensions["spacing.base"] = get_float(root, "spacing", 8);
    }

    return theme;
}

std::string export_stitch_design_system(const Theme& theme) {
    std::ostringstream ss;
    ss << "{\n";

    // Colors
    ss << "  \"colors\": {\n";
    bool first = true;
    for (auto& [name, color] : theme.colors) {
        if (!first) ss << ",\n";
        first = false;
        // Strip "color." prefix for Stitch
        auto key = name;
        if (key.substr(0, 6) == "color.") key = key.substr(6);
        char buf[10];
        if (color.a8() == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r8(), color.g8(), color.b8());
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r8(), color.g8(), color.b8(), color.a8());
        ss << "    \"" << key << "\": \"" << buf << "\"";
    }
    ss << "\n  },\n";

    // Fonts
    ss << "  \"fonts\": {\n";
    first = true;
    for (auto& [name, value] : theme.strings) {
        if (name.find("font.") != 0) continue;
        if (!first) ss << ",\n";
        first = false;
        auto key = name.substr(5);
        ss << "    \"" << key << "\": \"" << value << "\"";
    }
    ss << "\n  },\n";

    // Roundness
    float roundness = 8;
    if (theme.dimensions.count("roundness"))
        roundness = theme.dimensions.at("roundness");
    std::string r_name = "medium";
    if (roundness <= 0) r_name = "none";
    else if (roundness <= 4) r_name = "small";
    else if (roundness <= 8) r_name = "medium";
    else if (roundness <= 16) r_name = "large";
    else r_name = "full";
    ss << "  \"roundness\": \"" << r_name << "\",\n";

    // Spacing
    float spacing = 8;
    if (theme.dimensions.count("spacing.base"))
        spacing = theme.dimensions.at("spacing.base");
    ss << "  \"spacing\": " << static_cast<int>(spacing) << "\n";

    ss << "}\n";
    return ss.str();
}

} // namespace pulp::view
