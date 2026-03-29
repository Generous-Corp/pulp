#include <pulp/view/design_import.hpp>
#include <choc/text/choc_JSON.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <regex>
#include <cmath>
#include <map>

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

    // Audio widget detection
    node.audio_widget = detect_audio_widget(node.name);
    if (node.audio_widget == AudioWidgetType::none && !node.type.empty())
        node.audio_widget = detect_audio_widget(node.type);

    if (obj.hasObjectMember("label"))
        node.audio_label = get_string(obj, "label");
    if (obj.hasObjectMember("min"))
        node.audio_min = get_float(obj, "min", 0.0f);
    if (obj.hasObjectMember("max"))
        node.audio_max = get_float(obj, "max", 1.0f);
    if (obj.hasObjectMember("default"))
        node.audio_default = get_float(obj, "default", 0.5f);

    // Children
    if (obj.hasObjectMember("children") && obj["children"].isArray()) {
        auto children = obj["children"];
        for (uint32_t i = 0; i < children.size(); ++i)
            node.children.push_back(parse_ir_node(children[static_cast<int>(i)]));
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

std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts) {
    std::ostringstream ss;

    if (opts.include_comments) {
        ss << "// Generated by Pulp import-design from " << design_source_name(ir.source) << "\n";
        if (!ir.source_file.empty())
            ss << "// Source: " << ir.source_file << "\n";
        ss << "\n";
    }

    // Token assignments
    if (opts.include_tokens && (!ir.tokens.colors.empty() ||
                                 !ir.tokens.dimensions.empty() ||
                                 !ir.tokens.strings.empty())) {
        if (opts.include_comments)
            ss << "// Design tokens\n";

        for (auto& [name, value] : ir.tokens.colors)
            ss << "theme.colors[\"" << name << "\"] = '" << value << "';\n";
        for (auto& [name, value] : ir.tokens.dimensions)
            ss << "theme.dimensions[\"" << name << "\"] = " << value << ";\n";
        for (auto& [name, value] : ir.tokens.strings)
            ss << "theme.strings[\"" << name << "\"] = '" << value << "';\n";

        ss << "\n";
    }

    // Element tree
    int var_counter = 0;
    generate_node(ss, ir.root, opts, 0, var_counter, "");

    // Append root to document
    ss << "document.body.appendChild(" << opts.root_variable << ");\n";

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

    // W3C Design Tokens Format:
    // Top-level groups with $type, or nested tokens with $value/$type
    std::function<void(const choc::value::ValueView&, const std::string&)> walk;
    walk = [&](const choc::value::ValueView& obj, const std::string& prefix) {
        if (!obj.isObject()) return;

        for (uint32_t i = 0; i < obj.size(); ++i) {
            auto member = obj.getObjectMemberAt(i);
            std::string key(member.name);
            if (key.empty() || key[0] == '$') continue;

            auto& val = member.value;
            std::string full_name = prefix.empty() ? key : prefix + "." + key;

            // Leaf token: has $value
            if (val.isObject() && val.hasObjectMember("$value")) {
                auto type_str = get_string(val, "$type", "");
                auto value_str = std::string(val["$value"].toString());

                if (type_str == "color") {
                    // Parse hex color → Theme::colors
                    if (!value_str.empty() && value_str[0] == '#') {
                        auto c = parse_hex_color_str(value_str);
                        theme.colors[full_name] = c;
                    }
                } else if (type_str == "dimension") {
                    // Parse "8px" or "1.5rem" → float
                    float v = 0;
                    try { v = std::stof(value_str); } catch (...) {}
                    theme.dimensions[full_name] = v;
                } else if (type_str == "fontFamily" || type_str == "string") {
                    theme.strings[full_name] = value_str;
                } else if (type_str == "number") {
                    float v = 0;
                    try { v = std::stof(value_str); } catch (...) {}
                    theme.dimensions[full_name] = v;
                } else {
                    // Try to infer type from value
                    if (!value_str.empty() && value_str[0] == '#') {
                        auto c = parse_hex_color_str(value_str);
                        theme.colors[full_name] = c;
                    } else {
                        try {
                            float v = std::stof(value_str);
                            theme.dimensions[full_name] = v;
                        } catch (...) {
                            theme.strings[full_name] = value_str;
                        }
                    }
                }
            } else if (val.isObject()) {
                // Nested group — recurse
                walk(val, full_name);
            }
        }
    };

    walk(root, "");
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
        if (color.a == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r, color.g, color.b);
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);

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
        if (color.a == 255)
            snprintf(buf, sizeof(buf), "#%02x%02x%02x", color.r, color.g, color.b);
        else
            snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", color.r, color.g, color.b, color.a);
        tokens.colors[name] = buf;
    }
    tokens.dimensions = theme.dimensions;
    tokens.strings = theme.strings;
    return tokens;
}

} // namespace pulp::view
