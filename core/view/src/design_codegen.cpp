// design_codegen.cpp — design-IR → code generators, extracted from
// design_import.cpp in the 2026-05 Phase 6 (A3) refactor.
//
// Two code-generation backends plus the shared public entry point:
//
//   * web-compat JS generator   — emits @pulp/react-compatible JS from
//     the design IR (the default import target).
//   * native Pulp API generator — emits direct Pulp widget API calls.
//   * generate_pulp_js()        — public dispatch over CodeGenOptions.
//
// Definitions only; declarations stay in pulp/view/design_import.hpp.
// Relocated so codegen work no longer recompiles the whole importer.

#include <pulp/view/design_import.hpp>
#include <pulp/view/input_events.hpp>

#include "design_import_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace pulp::view {

/// Escape a string for emission inside a JavaScript single-quoted literal
/// (pulp #81). The codegen backends emit calls like
/// `createLabel('<id>', '<text>', '<parent>')` where the middle arg is
/// arbitrary user text. A multi-line `<style>` block imported from a
/// Claude Design HTML file would otherwise emit raw newlines into the JS
/// source — JavaScript treats that as an unterminated string. Mirror the
/// standard JS string-escape rules so any user text round-trips safely.
///
/// Used only by the codegen backends — moved here from design_import.cpp
/// in the Phase 6 A3 split.
std::string js_single_quote_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\0': out += "\\x00"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
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

    // Phase 0a: anchor trail comment so the runtime inspector can map
    // a generated element back to its stable_anchor_id (tweaks-layer key).
    // Comments are gated on opts.include_comments so minified codegen
    // strips them automatically.
    if (opts.include_comments && node.stable_anchor_id &&
        !node.stable_anchor_id->empty()) {
        ss << ind << "// @pulp-anchor " << *node.stable_anchor_id << "\n";
    }

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
        ss << ind << var << ".textContent = '" << js_single_quote_escape(node.text_content) << "';\n";  // pulp #81

    // Append to parent
    if (!parent_var.empty())
        ss << ind << parent_var << ".appendChild(" << var << ");\n";

    // Phase 0b: bind the anchor to the live widget so the inspector
    // can key tweaks against it. Emitted unconditionally (NOT gated on
    // include_comments) — the inspector needs the anchor to function
    // even in minified bundles. js_single_quote_escape() is defensive;
    // anchors are typically [a-z0-9:/-] but adapters can supply
    // anything.
    //
    // Codex P1 (#2303 follow-up): in web-compat codegen the JS variable
    // name is NOT the bridge widget id — `document.createElement` (web-
    // compat.js) auto-generates an internal `__el_N__` id and exposes
    // it as `<var>._id`. setAnchor must receive that id, not the JS
    // variable name, otherwise the bridge's `widget(id)` lookup misses
    // and the anchor wiring silently no-ops. Pass `<var>._id` so the
    // bridge finds the right widget regardless of whether the user
    // also called setId() on the element.
    if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
        ss << ind << "setAnchor(" << var << "._id, '"
           << js_single_quote_escape(*node.stable_anchor_id) << "');\n";
    }

    ss << "\n";

    // Recurse into children
    for (auto& child : node.children)
        generate_node(ss, child, opts, depth + 1, var_counter, var);
}

// ── Native-bridge JS code generator ─────────────────────────────────────
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

    // Emit setPosition('absolute') + setLeft/setTop/setRight/setBottom for
    // nodes whose IRStyle indicates absolute positioning (Figma plugin import
    // lane: children of non-auto-layout frames). The target may be the node
    // id or a wrapper id (e.g. audio-widget col_id).
    auto emit_position_if_absolute = [&](const std::string& target_id) {
        const auto& s = node.style;
        if (!s.position || *s.position != "absolute") return;
        ss << ind << "setPosition('" << target_id << "', 'absolute');\n";
        if (s.left)   ss << ind << "setLeft('"   << target_id << "', " << *s.left   << ");\n";
        if (s.top)    ss << ind << "setTop('"    << target_id << "', " << *s.top    << ");\n";
        if (s.right)  ss << ind << "setRight('"  << target_id << "', " << *s.right  << ");\n";
        if (s.bottom) ss << ind << "setBottom('" << target_id << "', " << *s.bottom << ");\n";
    };

    // Phase 0a: emit the anchor trail in bridge-native-JS codegen too. Same
    // gate + format as generate_node(), so downstream tooling has one
    // pattern to grep for regardless of which codegen mode produced
    // the JS.
    if (opts.include_comments && node.stable_anchor_id &&
        !node.stable_anchor_id->empty()) {
        ss << ind << "// @pulp-anchor " << *node.stable_anchor_id << "\n";
    }
    // Phase 0b: TODO — emit `setAnchor(id, anchor)` calls in this
    // bridge-native-JS codegen path too. The web-compat path
    // (generate_node) is wired; bridge-native-JS has many early returns
    // and several create call sites, so a small follow-up PR will
    // factor that out cleanly. For now, bridge-native-JS imports do not
    // bind anchors to live widgets — affects inspector tweaks for
    // imports that opted into native codegen. Web-compat is the
    // default mode, so most imports are unaffected.

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

        // Silver-knob synthesis: in the sprite-strip path the "VALUE" label
        // typically lives baked into the captured PNG (Figma knob components
        // include it as a flattened sub-text). When --knob-style=silver
        // swaps the PNG for native vector rendering there's nothing carrying
        // that label, so the knob reads as bare metal with no caption.
        // Synthesize a generic "VALUE" label so the silver path stays
        // visually parity with sprite. Only fires when both label slots
        // are empty (we'd never overwrite a real Figma label).
        if (opts.use_silver_knobs && label_text.empty() && value_text.empty()) {
            label_text = "VALUE";
        }

        if (opts.include_comments && !label_text.empty())
            ss << ind << "// " << label_text << "\n";

        // Helper: emit setWidgetStyle if preview mode
        auto emit_style = [&](const std::string& wid) {
            if (opts.preview_mode)
                ss << ind << "setWidgetStyle('" << wid << "', 'minimal');\n";
        };

        // When the IR has NO inlined label or value text children, the widget
        // doesn't need its own wrapper column — the parent (typically a Figma
        // auto-layout row that already arranges siblings) does the layout.
        // Wrapping in a col with +20 padding for absent labels makes the col
        // taller than the parent's hugged row height, breaking Yoga's flex
        // layout (Track A regression on the ELYSIUM knob row: the wrapper
        // was 28x61 / 62x111 / 28x61 inside a 158x91 parent).
        bool needs_label_wrapper = !label_text.empty() || !value_text.empty();

        // Create a wrapper column for the widget + value label (only when
        // there's actual label/value text to stack below the widget).
        std::string col_id = needs_label_wrapper ? (id + "_col") : parent_id;
        if (needs_label_wrapper) {
            ss << ind << "createCol('" << col_id << "', " << pid << ");\n";
            emit_position_if_absolute(col_id);
            ss << ind << "setFlex('" << col_id << "', 'align_items', 'center');\n";
            ss << ind << "setFlex('" << col_id << "', 'gap', 4);\n";
        } else {
            // No wrapper. Emit any absolute position on the widget itself.
            // (emit_position_if_absolute is called below at the createKnob site
            // by using id directly — we don't need to repeat here.)
        }

        if (wtype == AudioWidgetType::knob) {
            // Knob sizing priority:
            //   1. node.attributes["shape_width"/"shape_height"] — explicit
            //      override from designs that ship a non-default child
            //      ellipse size.
            //   2. node.style.width / height — figma-plugin lane sets these
            //      to the actual Figma instance bounds.
            //   3. kMinKnobSize fallback for purely heuristic detections.
            // The previous default-min behavior stretched skinned knobs to
            // 56x56 regardless of source size; with sprite-strip skins
            // (Track A) that distorted the PNG and overlapped neighbors.
            float shape_w = node.style.width.value_or(kMinKnobSize);
            float shape_h = node.style.height.value_or(kMinKnobSize);
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float frame_w = shape_w;
            float col_h = shape_h + 20 + (value_text.empty() ? 0 : 16);
            if (needs_label_wrapper) {
                ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
                ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            }
            ss << ind << "createKnob('" << id << "', '" << col_id << "');\n";
            if (!needs_label_wrapper) emit_position_if_absolute(id);
            ss << ind << "setFlex('" << id << "', 'width', " << shape_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Clear built-in label — use separate Yoga-positioned labels for exact placement
            ss << ind << "setLabel('" << id << "', ' ');\n";
            ss << ind << "setValue('" << id << "', " << node.audio_default << ");\n";
            // Track A3 — attach a designer-supplied sprite-strip skin when the
            // figma-plugin CLI lane (or anyone else) pre-resolved an asset_path
            // onto this knob node. Frame count defaults to 1 (static body);
            // a multi-frame strip lets the indicator rotate by value.
            //
            // Track D (alt-button): when opts.use_silver_knobs is true,
            // emit the native-vector silver render style instead. The
            // sprite-strip PNG path is skipped — the chrome look comes
            // from canvas primitives + radial gradient + indicator notch
            // (WidgetRenderStyle::silver). Crisp at any size, no PNG
            // bleed, no GPU texture upload.
            auto skin_it = node.attributes.find("asset_path");
            if (opts.use_silver_knobs) {
                ss << ind << "setWidgetStyle('" << id << "', 'silver');\n";
            } else if (skin_it != node.attributes.end() && !skin_it->second.empty()) {
                int frames = 1;
                auto fc_it = node.attributes.find("sprite_strip_frame_count");
                if (fc_it != node.attributes.end()) {
                    try { frames = std::max(1, std::stoi(fc_it->second)); } catch (...) {}
                }
                ss << ind << "setKnobSpriteStrip('" << id << "', '"
                   << js_single_quote_escape(skin_it->second) << "', "
                   << frames << ", 'vertical');\n";
            }
            emit_style(id);
            // Per-knob stroke color from child ellipse (used by minimal paint path)
            if (!stroke_color.empty())
                ss << ind << "setBorder('" << id << "', '" << stroke_color << "', 2.5, " << (shape_w * 0.5f) << ");\n";
            // Labels below knob — center-aligned, Yoga-positioned
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << js_single_quote_escape(label_text) << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
            if (!value_text.empty()) {
                std::string val_id = id + "_val";
                ss << ind << "createLabel('" << val_id << "', '" << js_single_quote_escape(value_text) << "', '" << col_id << "');\n";
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
                ss << ind << "createLabel('" << lbl_id << "', '" << js_single_quote_escape(label_text) << "', '" << col_id << "');\n";
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
                ss << ind << "createLabel('" << lbl_id << "', '" << js_single_quote_escape(label_text) << "', '" << col_id << "');\n";
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

    // Container, image, or text node
    bool is_image = (node.type == "image" || node.attributes.count("asset_path") > 0);
    bool is_container = !is_image && (!node.children.empty() || node.type == "frame");
    bool is_text = (node.type == "text" || node.type == "label");
    bool is_row = (node.layout.direction == LayoutDirection::row);

    if (is_image) {
        // Image node → createImage + setImageSource. Honors absolute positioning
        // emitted from the figma-plugin lane. asset_path is pre-resolved to an
        // absolute filesystem path by the CLI's asset resolution pass; nodes
        // missing the attribute (legacy callers with bare type=image) fall
        // through with no source set.
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";
        ss << ind << "createImage('" << id << "', " << pid << ");\n";
        emit_position_if_absolute(id);
        auto it = node.attributes.find("asset_path");
        if (it != node.attributes.end() && !it->second.empty()) {
            ss << ind << "setImageSource('" << id << "', '"
               << js_single_quote_escape(it->second) << "');\n";
        }
        if (node.style.width)
            ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
        if (node.style.height)
            ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
        // When the importer flagged this asset as bleed (PNG pixel dims
        // exceed twice the layout dims by 1.5× — typical drop-shadow case),
        // tell ImageView to render at natural size centered instead of
        // stretching the PNG into the smaller layout box. Same effect as
        // the Knob::paint natural-size fix, generalised through CSS
        // object-fit:none which ImageView already supports.
        auto bleed_it = node.attributes.find("asset_bleed");
        if (bleed_it != node.attributes.end() && bleed_it->second == "1") {
            ss << ind << "setObjectFit('" << id << "', 'none');\n";
        }
        ss << "\n";
        return;
    }

    if (is_text) {
        // Text node → createLabel with explicit height (Yoga requirement)
        ss << ind << "createLabel('" << id << "', '" << js_single_quote_escape(node.text_content) << "', " << pid << ");\n";  // pulp #81
        emit_position_if_absolute(id);

        // Honour the IR-declared height when present. Pre-fix this branch
        // unconditionally recomputed from font_size, which clobbered Figma's
        // own label box height — and any absolute-positioned label that
        // expected to be CENTRED in a slot relied on its own height matching
        // the slot. Visible bug: the SEARCH input's text+icon sit at design
        // y=5 / y=6 with IR heights 17 / 15; clobbering the text height to
        // 14 shifted its glyph baseline up so it no longer aligned with the
        // icon centre.
        float font_h = node.style.font_size.value_or(14.0f);
        float label_h = node.style.height
            ? std::max(*node.style.height, font_h * 1.0f)
            : std::max(font_h * 1.4f, kMinLabelHeight);
        ss << ind << "setFlex('" << id << "', 'height', " << label_h << ");\n";

        if (node.style.font_size)
            ss << ind << "setFontSize('" << id << "', " << *node.style.font_size << ");\n";
        if (node.style.font_weight)
            ss << ind << "setFontWeight('" << id << "', '" << *node.style.font_weight << "');\n";
        if (node.style.color)
            ss << ind << "setTextColor('" << id << "', '" << *node.style.color << "');\n";
        if (node.style.font_family)
            ss << ind << "setFontFamily('" << id << "', '" << js_single_quote_escape(*node.style.font_family) << "');\n";
        if (node.style.text_transform)
            ss << ind << "setTextTransform('" << id << "', '" << *node.style.text_transform << "');\n";
        if (node.style.text_align)
            ss << ind << "setTextAlign('" << id << "', '" << *node.style.text_align << "');\n";
        if (node.style.letter_spacing)
            ss << ind << "setLetterSpacing('" << id << "', " << *node.style.letter_spacing << ");\n";
        // Inflate min-width when the label is text-transformed to uppercase.
        // Figma stores the source-text width but renders the transformed
        // glyphs — uppercase Latin is typically ~15-20% wider than the
        // original mixed-case. Without compensation the label's reserved
        // box is too narrow for the rendered glyphs, so the text spills
        // into the next flex sibling (visible bug: "FILTER & EQHOLLOW
        // PUNCH" — the EQ label overflows its 77-px slot, painting on
        // top of the dropdown's "Hollow Punch" text).
        if (node.style.width) {
            bool uppercase = node.style.text_transform &&
                             *node.style.text_transform == "uppercase";
            float w = *node.style.width;
            if (uppercase) w *= 1.20f;  // empirical: caps run ~15-20% wider
            ss << ind << "setFlex('" << id << "', 'min_width', " << w << ");\n";
        }

        ss << "\n";
        return;
    }

    if (is_container) {
        // Container → createRow or createCol
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";

        ss << ind << (is_row ? "createRow" : "createCol")
           << "('" << id << "', " << pid << ");\n";
        emit_position_if_absolute(id);

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
        // Baseline override: a row that flexes text children of DIFFERENT
        // font sizes (typical "BIG_TITLE small_subtitle" header pattern)
        // visually wants align-items:baseline, not center. Figma's
        // auto-layout doesn't distinguish — it stores center — but
        // Yoga supports YGAlignBaseline and the visual difference between
        // box-center and baseline is significant once font sizes differ
        // by more than ~2pt. Triggers only on flex-row + 2+ text children
        // + size variance — leaves all other rows alone.
        bool baseline_override = false;
        if (is_row && node.layout.align == LayoutAlign::center) {
            std::vector<float> text_sizes;
            for (const auto& child : node.children) {
                if (child.type == "text" || child.type == "label") {
                    if (child.style.font_size) text_sizes.push_back(*child.style.font_size);
                }
            }
            if (text_sizes.size() >= 2) {
                float mn = *std::min_element(text_sizes.begin(), text_sizes.end());
                float mx = *std::max_element(text_sizes.begin(), text_sizes.end());
                if (mx - mn >= 1.5f) baseline_override = true;
            }
        }
        if (baseline_override) {
            ss << ind << "setFlex('" << id << "', 'align_items', 'baseline');\n";
        } else if (node.layout.align != LayoutAlign::stretch)
            ss << ind << "setFlex('" << id << "', 'align_items', '" << align_to_css(node.layout.align) << "');\n";

        // Visual styles
        if (node.style.background_color)
            ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
        if (node.style.background_gradient)
            ss << ind << "setBackgroundGradient('" << id << "', '" << js_single_quote_escape(*node.style.background_gradient) << "');\n";
        if (node.style.border_radius)
            ss << ind << "setCornerRadius('" << id << "', 'All', " << *node.style.border_radius << ");\n";
        // Emit border (Figma frame stroke) as setBorder(id, color, width).
        // The codegen was previously dropping these — visible effect: the
        // 4 column frames inside the ELYSIUM gradient panel each have a
        // 1px rgba(0,0,0,0.1) border that Figma renders as the thin
        // vertical separators between GRAINS/CURSOR/RANGE/TUNING. The
        // bridge's parseColor accepts both hex and rgba(...) so we can
        // pass border_color verbatim from the IR.
        if (node.style.border_color && node.style.border_width &&
            *node.style.border_width > 0.0f) {
            float br = node.style.border_radius.value_or(0.0f);
            ss << ind << "setBorder('" << id << "', '"
               << js_single_quote_escape(*node.style.border_color) << "', "
               << *node.style.border_width << ", " << br << ");\n";
        }
        if (node.style.box_shadow) {
            // Parse CSS-style "<ox> <oy> <blur> [<spread>] <color>" into the
            // bridge's setBoxShadow(id, ox, oy, blur, spread, color, inset?)
            // signature — passing the raw string makes the bridge fall back
            // to default (2,4,#00000050) numerics, which doesn't match the
            // designer's intent.
            const std::string& s = *node.style.box_shadow;
            float ox = 0, oy = 0, blur = 0, spread = 0;
            std::string color = "#00000050";
            bool inset = false;
            // Strip trailing/leading whitespace + detect inset.
            std::string body = s;
            auto find_lower = [](const std::string& h, const std::string& n) {
                auto it = std::search(h.begin(), h.end(), n.begin(), n.end(),
                    [](char a, char b){ return std::tolower(static_cast<unsigned char>(a))
                                              == std::tolower(static_cast<unsigned char>(b)); });
                return it == h.end() ? std::string::npos : (size_t)(it - h.begin());
            };
            if (auto p = find_lower(body, "inset"); p != std::string::npos) {
                inset = true;
                body.erase(p, 5);
            }
            // Tokenize: pull color first (hex or rgba(...)), then 3–4 lengths.
            auto rgba_p = body.find("rgba(");
            auto rgb_p  = body.find("rgb(");
            auto hex_p  = body.find('#');
            if (rgba_p != std::string::npos) {
                auto end = body.find(')', rgba_p);
                if (end != std::string::npos) {
                    color = body.substr(rgba_p, end - rgba_p + 1);
                    body.erase(rgba_p, end - rgba_p + 1);
                }
            } else if (rgb_p != std::string::npos) {
                auto end = body.find(')', rgb_p);
                if (end != std::string::npos) {
                    color = body.substr(rgb_p, end - rgb_p + 1);
                    body.erase(rgb_p, end - rgb_p + 1);
                }
            } else if (hex_p != std::string::npos) {
                auto end = body.find_first_of(" \t,", hex_p);
                color = body.substr(hex_p, end == std::string::npos ? std::string::npos : end - hex_p);
                body.erase(hex_p, end == std::string::npos ? std::string::npos : end - hex_p);
            }
            // Collect numeric tokens (strip "px").
            std::vector<float> nums;
            std::string tok;
            auto flush = [&]() {
                if (tok.empty()) return;
                // strip "px" suffix
                if (tok.size() > 2 && tok.substr(tok.size()-2) == "px") tok.resize(tok.size()-2);
                try { nums.push_back(std::stof(tok)); } catch (...) {}
                tok.clear();
            };
            for (char c : body) {
                if (std::isspace(static_cast<unsigned char>(c)) || c == ',') flush();
                else tok.push_back(c);
            }
            flush();
            if (nums.size() >= 2) { ox = nums[0]; oy = nums[1]; }
            if (nums.size() >= 3) blur = nums[2];
            if (nums.size() >= 4) spread = nums[3];
            ss << ind << "setBoxShadow('" << id << "', "
               << ox << ", " << oy << ", " << blur << ", " << spread
               << ", '" << js_single_quote_escape(color) << "'"
               << (inset ? ", true" : "") << ");\n";
        }
        if (node.style.opacity)
            ss << ind << "setOpacity('" << id << "', " << *node.style.opacity << ");\n";

        ss << "\n";

        // Recurse children.
        // For space_between rows with TWO OR MORE text children (the canonical
        // "label … value" or "key … units" pattern), right-align the LAST
        // text child so the value hugs the right edge. The earlier version
        // applied this to single-text-child rows too, which misfired on
        // dropdowns like ENVELOPE → "Short Plucks" + chevrons-image:
        // Short Plucks was the only text and got right-aligned within its
        // own min-width box, pushing the visible glyphs onto the chevrons.
        bool is_space_between = is_row && node.layout.justify == LayoutAlign::space_between;
        int text_child_count = 0;
        if (is_space_between) {
            for (auto& child : node.children) {
                if (child.type == "text" || child.type == "label") ++text_child_count;
            }
        }
        // Cap-height nudge for [small icon, UPPERCASE label] header rows.
        // Figma vertically centers icons on the label's cap-height optical
        // centre. CSS / Yoga `align-items: center` uses the line-box math
        // centre, which sits ~font_size * 0.15 BELOW the cap-glyph optical
        // centre because the line box reserves descender slack the
        // uppercase glyphs don't occupy. Pulp's Label::resolved_state()
        // produces the same math-centre baseline, so the dot ends up
        // visually below the label glyphs. Generalisable rule: when a row
        // has align_items: center, at least one uppercase text child, and
        // any image child whose min-dim ≤ that label's font_size, emit a
        // negative margin_top on the icon so its centre lifts to the
        // cap-glyph centre. No hardcoded constants — the nudge is derived
        // from the label's own font_size.
        float upper_font_size = 0.0f;
        if (is_row && node.layout.align == LayoutAlign::center && !baseline_override) {
            for (const auto& c : node.children) {
                bool is_txt = (c.type == "text" || c.type == "label");
                if (!is_txt) continue;
                if (!c.style.text_transform || *c.style.text_transform != "uppercase")
                    continue;
                upper_font_size = std::max(upper_font_size,
                                           c.style.font_size.value_or(0.0f));
            }
        }
        // In flex with align-items: center, a margin_top of -M shifts the
        // child's position UP by M/2 (Yoga centers around the margin-
        // adjusted box). So to lift the icon's centre by font_size * 0.15
        // (the cap-vs-math centre delta for an uppercase line-box) we
        // need a -2 × that margin.
        float cap_nudge = (upper_font_size > 0.0f)
                              ? std::round(upper_font_size * 0.30f)
                              : 0.0f;

        std::string last_text_child_id;
        for (auto& child : node.children) {
            std::string child_var_id_pre;
            if (is_space_between && text_child_count >= 2 &&
                (child.type == "text" || child.type == "label")) {
                std::string child_id = sanitize_var(child.name.empty() ? child.type : child.name);
                child_id += std::to_string(var_counter);  // counter value before this child runs
                last_text_child_id = child_id;
            }
            // Compute the image child's about-to-be-emitted var id so we
            // can pin a setFlex margin_top onto it after generation.
            bool is_img_child = (child.type == "image" ||
                                  child.attributes.count("asset_path") > 0);
            bool nudge_this_child = false;
            if (cap_nudge > 0.0f && is_img_child) {
                float cw = child.style.width.value_or(0.0f);
                float ch = child.style.height.value_or(0.0f);
                float small = (cw > 0.0f && ch > 0.0f) ? std::min(cw, ch) : 0.0f;
                if (small > 0.0f && small <= upper_font_size) {
                    child_var_id_pre = sanitize_var(child.name.empty() ? child.type : child.name);
                    child_var_id_pre += std::to_string(var_counter);
                    nudge_this_child = true;
                }
            }
            generate_native_node(ss, child, opts, depth + 1, var_counter, id);
            if (nudge_this_child) {
                std::string child_ind = indent(depth + 1, opts.indent_spaces);
                ss << child_ind << "setFlex('" << child_var_id_pre
                   << "', 'margin_top', " << -cap_nudge << ");\n";
            }
        }
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

    // Phase 9: tag the bundle with a motion-observability provenance
    // envelope so any animation it drives (view.animate, CSS transitions,
    // rAF publishes) inherits source_kind="design-import",
    // source_id="<vendor>:<root-node-or-file>". The native bridge falls
    // through cleanly when `motion` isn't installed (e.g. when the
    // bundle is rendered by an older Pulp build), so the line is safe
    // to emit unconditionally.
    {
        std::string root_id;
        if (!ir.root.name.empty()) root_id = ir.root.name;
        else if (!ir.source_file.empty()) {
            // Strip directory + extension for a compact id.
            auto& p = ir.source_file;
            auto slash = p.find_last_of("/\\");
            auto base = (slash == std::string::npos) ? p : p.substr(slash + 1);
            auto dot = base.find_last_of('.');
            root_id = (dot == std::string::npos) ? base : base.substr(0, dot);
        } else {
            root_id = "bundle";
        }
        // Escape single quotes for embedding in the JS string literal.
        std::string safe_id;
        safe_id.reserve(root_id.size());
        for (char c : root_id) {
            if (c == '\'' || c == '\\') safe_id += '\\';
            safe_id += c;
        }
        ss << "if (typeof motion !== 'undefined' && motion.setProvenance)\n"
           << "    motion.setProvenance('design-import', '"
           << design_source_vendor_key(ir.source) << ":" << safe_id << "');\n\n";
    }

    ss << "setTheme('dark');\n\n";

    // Token assignments
    if (opts.include_tokens && (!ir.tokens.colors.empty() ||
                                 !ir.tokens.dimensions.empty() ||
                                 !ir.tokens.strings.empty())) {
        if (opts.include_comments)
            ss << "// Design tokens\n";

        if (opts.mode == CodeGenMode::bridge_native_js) {
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

    // pulp #2116 V2 — auto-imported keyboard shortcuts. Strategy A:
    // register a native intercept per detected chord; the handler thunk
    // re-dispatches a synthetic W3C keydown into __dispatch__ so the
    // original React handlers in the bundled JS (which still own the
    // component-state closures) fire naturally. We don't try to port the
    // handler bodies — the OS-level intercept and the JS-level closure
    // semantics each stay in their natural home.
    if (!opts.shortcuts.empty()) {
        if (opts.include_comments) {
            ss << "// Auto-imported keyboard shortcuts (pulp #2116). Each\n"
               << "// registerShortcut binds a native chord intercept; the\n"
               << "// __pulpShortcutHandler_N thunk re-dispatches the\n"
               << "// synthetic keydown so the original React handler in\n"
               << "// the bundled JS fires with its live closure state.\n";
        }
        // Emit one (key, mask) -> handler binding. Generates the
        // __dispatch__ thunk with the modifier flags that match the
        // physical chord we're intercepting — so the bundled React
        // handler's `e.ctrlKey`, `e.metaKey`, `e.metaKey || e.ctrlKey`
        // checks all see the right state.
        // Escape a key string for a single-quoted JS literal. Codex P1 on
        // #2128: widening `key_string_to_keycode` to accept all printable
        // ASCII let characters like `'` and `\` pass validation, but the
        // generator interpolates them raw into `key: '...'`. A source
        // with `e.key === "'"` produces syntactically-invalid JS and the
        // whole script fails to load. Escape backslash + single-quote at
        // emission time so any future printable-key default is safe too.
        auto js_escape_single_quoted = [](const std::string& in) {
            std::string out;
            out.reserve(in.size() + 2);
            for (char c : in) {
                if (c == '\\' || c == '\'') out += '\\';
                out += c;
            }
            return out;
        };

        auto emit_binding = [&](int kc, int mask, const std::string& key_str,
                                const std::string& handler) {
            const std::string key_esc = js_escape_single_quoted(key_str);
            ss << "globalThis." << handler << " = function() {\n";
            ss << "    if (typeof __dispatch__ !== 'function') return;\n";
            ss << "    __dispatch__('__global__', 'keydown', {\n";
            ss << "        key: '" << key_esc << "',\n";
            ss << "        keyCode: " << kc << ",\n";
            ss << "        ctrlKey: " << ((mask & kModCtrl) ? "true" : "false") << ",\n";
            ss << "        shiftKey: " << ((mask & kModShift) ? "true" : "false") << ",\n";
            ss << "        altKey: " << ((mask & kModAlt) ? "true" : "false") << ",\n";
            // Set metaKey true whenever the intercept owns the platform-
            // primary modifier (kModCmd OR kModMeta) — covers macOS Cmd
            // and Windows/Linux Meta.
            ss << "        metaKey: " << (((mask & kModCmd) || (mask & kModMeta)) ? "true" : "false") << ",\n";
            ss << "        mods: " << mask << "\n";
            ss << "    });\n";
            ss << "};\n";
            ss << "registerShortcut(" << kc << ", " << mask << ", '" << handler << "');\n";
        };

        for (size_t i = 0; i < opts.shortcuts.size(); ++i) {
            const auto& s = opts.shortcuts[i];
            int kc = key_string_to_keycode(s.key);
            if (kc == 0) {
                if (opts.include_comments) {
                    ss << "// shortcut #" << i << " skipped: unmapped key \""
                       << s.key << "\" (handler: " << s.handler_excerpt << ")\n";
                }
                continue;
            }

            // Codex P1 on #2119: preserve Ctrl-only / Meta-only / cross-
            // platform `metaKey||ctrlKey` intent. If BOTH modifiers were
            // collected we emit two bindings so the user can hit either
            // physical chord (Cmd on macOS, Ctrl on Win/Linux) and each
            // synthetic event carries the right modifier flags.
            bool has_meta = false, has_ctrl = false;
            for (const auto& m : s.modifiers) {
                if (m == "meta") has_meta = true;
                else if (m == "ctrl") has_ctrl = true;
            }
            if (has_meta && has_ctrl) {
                // Build the non-meta/ctrl portion of the mask once
                // (shift, alt) and OR the platform modifier per emit.
                std::vector<std::string> other_mods;
                for (const auto& m : s.modifiers) {
                    if (m != "meta" && m != "ctrl") other_mods.push_back(m);
                }
                int base = modifier_strings_to_mask(other_mods);
                std::string base_handler = "__pulpShortcutHandler_" + std::to_string(i);
                emit_binding(kc, base | kModCmd,  s.key, base_handler + "_cmd");
                emit_binding(kc, base | kModCtrl, s.key, base_handler + "_ctrl");
            } else {
                int mask = modifier_strings_to_mask(s.modifiers);
                std::string handler = "__pulpShortcutHandler_" + std::to_string(i);
                emit_binding(kc, mask, s.key, handler);
            }
        }
        ss << "\n";
    }

    if (opts.mode == CodeGenMode::bridge_native_js) {
        // Native-bridge JS API
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

} // namespace pulp::view
