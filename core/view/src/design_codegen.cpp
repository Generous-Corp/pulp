// design_codegen.cpp — design-IR → code generators.
//
// Two code-generation backends plus the shared public entry point:
//
//   * web-compat JS generator   — emits @pulp/react-compatible JS from
//     the design IR (the default import target).
//   * native Pulp API generator — emits direct Pulp widget API calls.
//   * generate_pulp_js()        — public dispatch over CodeGenOptions.
//
// Definitions only; declarations stay in pulp/view/design_import.hpp.

#include <pulp/view/design_import.hpp>
#include <pulp/view/design_fidelity.hpp>
#include <pulp/view/input_events.hpp>

#include "design_import_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace pulp::view {

/// Escape a string for emission inside a JavaScript single-quoted literal
/// The codegen backends emit calls like
/// `createLabel('<id>', '<text>', '<parent>')` where the middle arg is
/// arbitrary user text. A multi-line `<style>` block imported from a
/// Claude Design HTML file would otherwise emit raw newlines into the JS
/// source — JavaScript treats that as an unterminated string. Mirror the
/// standard JS string-escape rules so any user text round-trips safely.
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

// Emit a text node's content as per-range <span>s (web-compat). Runs that
// override style become styled <span> children; the gaps between them are
// appended as plain text so they inherit the node's dominant style. Offsets are
// byte indices into text_content (UTF-16 surrogate handling is intentionally
// out of scope here; callers/tests use byte-safe ranges).
static void emit_web_text_runs(std::ostringstream& ss, const std::string& ind,
                               const std::string& var, const IRNode& node) {
    const std::string& text = node.text_content;
    const int n = static_cast<int>(text.size());
    std::vector<const IRTextRun*> runs;
    for (const auto& r : node.text_runs)
        if (r.start < r.end && r.start < n) runs.push_back(&r);
    std::sort(runs.begin(), runs.end(),
              [](const IRTextRun* a, const IRTextRun* b) { return a->start < b->start; });

    // Run offsets are UTF-8 byte offsets, but defend against a source that
    // passes a boundary landing mid-codepoint: snap forward to the next
    // codepoint start (continuation bytes are 10xxxxxx) so we never slice a
    // multibyte character in half and emit invalid UTF-8.
    auto snap = [&](int k) {
        while (k > 0 && k < n && (static_cast<unsigned char>(text[k]) & 0xC0) == 0x80) ++k;
        return k;
    };

    auto append_plain = [&](int a, int b) {
        if (b <= a) return;
        ss << ind << var << ".appendChild(document.createTextNode('"
           << js_single_quote_escape(text.substr(a, b - a)) << "'));\n";
    };

    int cursor = 0, idx = 0;
    for (const auto* r : runs) {
        int rs = snap(std::max(0, r->start)), re = snap(std::min(n, r->end));
        if (re <= cursor) continue;          // wholly behind cursor (overlap) — skip
        if (rs < cursor) rs = cursor;        // clip a partial overlap
        append_plain(cursor, rs);            // base-styled gap before the run
        const std::string child = var + "_r" + std::to_string(idx++);
        ss << ind << "const " << child << " = document.createElement('span');\n";
        // Carry the node's dominant style onto the run child first — Pulp's
        // web-compat Labels do not inherit from the parent span, so without this
        // a run that overrides only one field would render the rest with Label
        // defaults. The run overrides below win over these base values.
        const auto& base = node.style;
        if (base.font_family)
            ss << ind << child << ".style.fontFamily = '" << js_single_quote_escape(*base.font_family) << "';\n";
        if (base.font_size)
            ss << ind << child << ".style.fontSize = '" << *base.font_size << "px';\n";
        if (base.font_weight)
            ss << ind << child << ".style.fontWeight = '" << *base.font_weight << "';\n";
        if (base.color)
            ss << ind << child << ".style.color = '" << js_single_quote_escape(*base.color) << "';\n";
        if (base.letter_spacing)
            ss << ind << child << ".style.letterSpacing = '" << *base.letter_spacing << "px';\n";
        if (r->font_weight)
            ss << ind << child << ".style.fontWeight = '" << *r->font_weight << "';\n";
        if (r->font_size)
            ss << ind << child << ".style.fontSize = '" << *r->font_size << "px';\n";
        if (r->font_style)
            ss << ind << child << ".style.fontStyle = '" << js_single_quote_escape(*r->font_style) << "';\n";
        if (r->color)
            ss << ind << child << ".style.color = '" << js_single_quote_escape(*r->color) << "';\n";
        if (r->letter_spacing)
            ss << ind << child << ".style.letterSpacing = '" << *r->letter_spacing << "px';\n";
        if (r->text_decoration)
            ss << ind << child << ".style.textDecoration = '" << js_single_quote_escape(*r->text_decoration) << "';\n";
        ss << ind << child << ".textContent = '" << js_single_quote_escape(text.substr(rs, re - rs)) << "';\n";
        ss << ind << var << ".appendChild(" << child << ");\n";
        cursor = re;
    }
    append_plain(cursor, n);  // trailing base-styled text
}

static void generate_node(std::ostringstream& ss, const IRNode& node,
                           const CodeGenOptions& opts, int depth,
                           int& var_counter, const std::string& parent_var) {
    std::string var = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) var += std::to_string(var_counter++);
    else var = opts.root_variable;

    std::string ind = indent(depth, opts.indent_spaces);

    // Emit an anchor trail comment so the runtime inspector can map
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
        // Escape raw CSS values. clip-path and mask can carry url("...") with
        // quotes, and must not break out of the JS string literal.
        // js_single_quote_escape is a no-op for the common quote-free
        // keyword/color values, so this is regression-safe.
        if (val) ss << ind << var << ".style." << prop << " = '"
                    << js_single_quote_escape(*val) << "';\n";
    };
    auto emit_px = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << format_px(*val) << "';\n";
    };
    auto emit_float = [&](const char* prop, const std::optional<float>& val) {
        if (val) ss << ind << var << ".style." << prop << " = '" << *val << "';\n";
    };

    emit_str("backgroundColor", s.background_color);
    if (s.background_gradient)
        ss << ind << var << ".style.background = '" << js_single_quote_escape(*s.background_gradient) << "';\n";
    emit_str("color", s.color);
    emit_float("opacity", s.opacity);
    emit_str("mixBlendMode", s.mix_blend_mode);
    emit_str("clipPath", s.clip_path);
    emit_str("mask", s.mask);
    emit_str("maskImage", s.mask_image);
    emit_str("maskSize", s.mask_size);
    emit_px("borderRadius", s.border_radius);
    emit_str("border", s.border);
    if (!s.box_shadow.empty())
        ss << ind << var << ".style.boxShadow = '" << box_shadow_to_css(s.box_shadow) << "';\n";
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

    // Vertically center single-line text in a slot taller than its font, so the
    // web-compat LIVE render matches the native materializer (which applies
    // Label::set_vertical_align(center) under the SAME rule). The shim maps
    // style.verticalAlign → setVerticalAlign → Label, so both render paths
    // converge on one mechanism and the screenshot-parity invariant holds.
    // Figma reserves a tall text slot for centered text, but the IR drops
    // textAlignVertical, so derive it from slot-vs-font. Without this the
    // <span> top-aligns and parity diverges (control-strip fixture).
    if (node.type == "text" && s.height && s.font_size &&
        *s.height > *s.font_size * 1.15f)
        ss << ind << var << ".style.verticalAlign = 'middle';\n";

    // Reference-free image-sizing fidelity self-check on the web-compat path
    // too (mirrors generate_native_node). The web-compat <img> emits the style
    // box directly, so the emitted geometry is exactly s.width/s.height. The
    // widget/text slot checks depend on native-emitted geometry and don't map
    // 1:1 to web-compat output.
    if (node.type == "image" && opts.fidelity_report && s.width && s.height) {
        run_fidelity_checks({node, var, *s.width, *s.height, FidelityElement::image},
                            *opts.fidelity_report);
    }

    // Text content. Mixed-style text (per-range runs) emits nested styled
    // <span>s; plain single-style text keeps the simple textContent assignment.
    if (!node.text_runs.empty() && !node.text_content.empty())
        emit_web_text_runs(ss, ind, var, node);
    else if (!node.text_content.empty())
        ss << ind << var << ".textContent = '" << js_single_quote_escape(node.text_content) << "';\n";

    // Append to parent
    if (!parent_var.empty())
        ss << ind << parent_var << ".appendChild(" << var << ");\n";

    // Bind the anchor to the live widget so the inspector
    // can key tweaks against it. Emitted unconditionally (NOT gated on
    // include_comments) — the inspector needs the anchor to function
    // even in minified bundles. js_single_quote_escape() is defensive;
    // anchors are typically [a-z0-9:/-] but adapters can supply
    // anything.
    //
    // In web-compat codegen the JS variable name is NOT the bridge widget
    // id: `document.createElement` (web-compat.js) auto-generates an
    // internal `__el_N__` id and exposes it as `<var>._id`. setAnchor
    // must receive that id, not the JS variable name, otherwise the
    // bridge's `widget(id)` lookup misses and the anchor wiring silently
    // no-ops. Pass `<var>._id` so the bridge finds the right widget even
    // when user code also called setId() on the element.
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

static void generate_native_node_impl(std::ostringstream& ss, const IRNode& node,
                                  const CodeGenOptions& opts, int depth,
                                  int& var_counter, const std::string& parent_id,
                                  std::unordered_map<const IRNode*, std::string>* id_map);

/// Emit one node's bridge-native JS, then bind its anchor to the live widget.
///
/// The anchor binding lives HERE, in a wrapper, rather than in the body: the
/// body has a terminal branch per node kind (widget, vector, image, text,
/// container, bare frame), each with its own create call and its own `return`,
/// so binding inside it would mean six call sites and a seventh kind silently
/// unanchored. The body already funnels the one fact the binding needs — the
/// exact bridge id it emitted — into `id_map` at its single entry point, so the
/// wrapper reads it back and binds afterwards. setAnchor only needs the widget
/// to exist by the time the script finishes, so trailing the subtree is fine.
///
/// Without this the native lane produced anchor COMMENTS and nothing else: the
/// views carried no anchor, so the inspector could not key tweaks against
/// source, and every node-keyed validation (fidelity_diff, --dump-layout parity)
/// had nothing to join on and skipped — reporting success by finding nothing.
static void generate_native_node(std::ostringstream& ss, const IRNode& node,
                                  const CodeGenOptions& opts, int depth,
                                  int& var_counter, const std::string& parent_id,
                                  std::unordered_map<const IRNode*, std::string>* id_map = nullptr) {
    // The caller's map when it wants the ids, a local one otherwise — the
    // binding must not depend on whether someone else asked for the mapping.
    std::unordered_map<const IRNode*, std::string> local_ids;
    auto* ids = id_map ? id_map : &local_ids;
    generate_native_node_impl(ss, node, opts, depth, var_counter, parent_id, ids);
    if (!node.stable_anchor_id || node.stable_anchor_id->empty()) return;
    auto it = ids->find(&node);
    if (it == ids->end()) return;
    ss << indent(depth, opts.indent_spaces) << "setAnchor('" << it->second << "', '"
       << js_single_quote_escape(*node.stable_anchor_id) << "');\n";
}

static void generate_native_node_impl(std::ostringstream& ss, const IRNode& node,
                                  const CodeGenOptions& opts, int depth,
                                  int& var_counter, const std::string& parent_id,
                                  std::unordered_map<const IRNode*, std::string>* id_map) {
    std::string id = sanitize_var(node.name.empty() ? node.type : node.name);
    if (depth > 0) id += std::to_string(var_counter++);
    else id = opts.root_variable;
    // Record the exact emitted bridge id so the tree-level fidelity pass
    // (check_vector_renderability) can point findings at the real node id
    // codegen used, not a re-derived best-effort name.
    if (id_map) (*id_map)[&node] = id;

    std::string ind = indent(depth, opts.indent_spaces);
    std::string pid = parent_id.empty() ? "''" : ("'" + parent_id + "'");

    // Emit setPosition('absolute') + setLeft/setTop/setRight/setBottom for
    // nodes whose IRStyle indicates absolute positioning (Figma plugin import
    // lane: children of non-auto-layout frames). The target may be the node
    // id or a wrapper id (e.g. audio-widget col_id).
    // Map Figma resize CONSTRAINTS (node.layout.h/v_constraint) onto flex within
    // the parent: center → auto margins both sides; right/bottom → auto margin on
    // the leading side (pushes to the trailing edge); scale → flex-grow:1 (fill
    // the main axis); stretch (pin both edges) → align-self:stretch (fill the
    // cross axis). left/top are the flex default (start-anchored) so they emit
    // nothing. Stays entirely within Flexbox primitives — no new layout engine.
    auto emit_layout_constraints = [&](const std::string& target_id) {
        if (depth == 0) return;  // the root is not laid out within a parent
        const auto& L = node.layout;
        // flex-shrink is the one flex property the IR could carry but codegen
        // never wrote, so a source lane that set it was overruled by Yoga's
        // default of 1 without a word. An importer replaying an already-solved
        // layout needs shrink:0 to keep the sizes it emitted.
        if (L.flex_shrink)
            ss << ind << "setFlex('" << target_id << "', 'flex_shrink', " << *L.flex_shrink << ");\n";
        bool grow_done = false, stretch_done = false;
        auto grow = [&] {
            if (grow_done) return;
            ss << ind << "setFlex('" << target_id << "', 'flex_grow', 1);\n";
            grow_done = true;
        };
        auto stretch = [&] {
            if (stretch_done) return;
            ss << ind << "setFlex('" << target_id << "', 'align_self', 'stretch');\n";
            stretch_done = true;
        };
        if (L.h_constraint) {
            const std::string& h = *L.h_constraint;
            if (h == "center") {
                ss << ind << "setFlex('" << target_id << "', 'margin_left', 'auto');\n";
                ss << ind << "setFlex('" << target_id << "', 'margin_right', 'auto');\n";
            } else if (h == "right") {
                ss << ind << "setFlex('" << target_id << "', 'margin_left', 'auto');\n";
            } else if (h == "scale") {
                grow();
            } else if (h == "stretch") {
                stretch();
                // Pin both horizontal edges = fill width. min-width:100% keeps
                // this effective even when the node ALSO has an explicit width
                // (Yoga clamps the final width up to min-width), so STRETCH is no
                // longer a no-op against a defined cross-size.
                ss << ind << "setFlex('" << target_id << "', 'min_width', '100%');\n";
            }
        }
        if (L.v_constraint) {
            const std::string& v = *L.v_constraint;
            if (v == "center") {
                ss << ind << "setFlex('" << target_id << "', 'margin_top', 'auto');\n";
                ss << ind << "setFlex('" << target_id << "', 'margin_bottom', 'auto');\n";
            } else if (v == "bottom") {
                ss << ind << "setFlex('" << target_id << "', 'margin_top', 'auto');\n";
            } else if (v == "scale") {
                grow();
            } else if (v == "stretch") {
                stretch();
                ss << ind << "setFlex('" << target_id << "', 'min_height', '100%');\n";
            }
        }
    };

    // Grid item placement: a child of a grid container can carry grid-column /
    // grid-row LINE placement ("1 / 3", "2", "1 / span 2"). Lower to setGrid
    // column/row start/end on the child. A "span N" END resolves to start+N.
    // Span-only (no start line) and named lines are left to auto-placement.
    auto emit_grid_item_placement = [&](const std::string& target_id) {
        if (depth == 0) return;
        auto emit_track = [&](const std::optional<std::string>& v,
                              const char* start_key, const char* end_key) {
            if (!v || v->empty()) return;
            auto trim = [](std::string x) {
                auto a = x.find_first_not_of(" \t");
                auto b = x.find_last_not_of(" \t");
                return a == std::string::npos ? std::string() : x.substr(a, b - a + 1);
            };
            const std::string& raw = *v;
            auto slash = raw.find('/');
            std::string lo = trim(slash == std::string::npos ? raw : raw.substr(0, slash));
            std::string hi = slash == std::string::npos ? std::string() : trim(raw.substr(slash + 1));
            auto as_int = [](const std::string& t, int& out) {
                if (t.empty()) return false;
                char* e = nullptr;
                long n = std::strtol(t.c_str(), &e, 10);
                if (e == t.c_str()) return false;  // named / auto -> skip
                out = static_cast<int>(n);
                return true;
            };
            int lo_i = 0;
            const bool have_lo = as_int(lo, lo_i);
            if (have_lo)
                ss << ind << "setGrid('" << target_id << "', '" << start_key << "', " << lo_i << ");\n";
            int hi_i;
            if (as_int(hi, hi_i)) {
                ss << ind << "setGrid('" << target_id << "', '" << end_key << "', " << hi_i << ");\n";
            } else if (have_lo && hi.rfind("span", 0) == 0) {
                // "<start> / span <n>"  ->  end line = start + n
                int span = 0;
                if (as_int(trim(hi.substr(4)), span) && span > 0)
                    ss << ind << "setGrid('" << target_id << "', '" << end_key << "', "
                       << (lo_i + span) << ");\n";
            }
        };
        emit_track(node.layout.grid_column, "column_start", "column_end");
        emit_track(node.layout.grid_row, "row_start", "row_end");
    };

    auto emit_position_if_absolute = [&](const std::string& target_id) {
        emit_layout_constraints(target_id);
        emit_grid_item_placement(target_id);
        const auto& s = node.style;
        if (!s.position || *s.position != "absolute") return;
        ss << ind << "setPosition('" << target_id << "', 'absolute');\n";
        if (s.left)   ss << ind << "setLeft('"   << target_id << "', " << *s.left   << ");\n";
        if (s.top)    ss << ind << "setTop('"    << target_id << "', " << *s.top    << ");\n";
        if (s.right)  ss << ind << "setRight('"  << target_id << "', " << *s.right  << ");\n";
        if (s.bottom) ss << ind << "setBottom('" << target_id << "', " << *s.bottom << ");\n";
    };

    // Emit a node's per-View visual overrides that the native engine applies
    // at compositing/paint time: mix-blend-mode (View::set_mix_blend_mode), the
    // clip-path, and the mask shorthand / image / size (View::set_clip_path /
    // set_mask / set_mask_image / set_mask_size). mix-blend-mode is
    // parse-normalized (normal / pass-through dropped); the clip/mask values
    // are raw CSS the bridge parses.
    auto emit_node_visual_overrides = [&](const std::string& target_id) {
        const auto& st = node.style;
        if (st.mix_blend_mode && !st.mix_blend_mode->empty())
            ss << ind << "setMixBlendMode('" << target_id << "', '"
               << js_single_quote_escape(*st.mix_blend_mode) << "');\n";
        if (st.clip_path && !st.clip_path->empty())
            ss << ind << "setClipPath('" << target_id << "', '"
               << js_single_quote_escape(*st.clip_path) << "');\n";
        if (st.mask && !st.mask->empty())
            ss << ind << "setMask('" << target_id << "', '"
               << js_single_quote_escape(*st.mask) << "');\n";
        if (st.mask_image && !st.mask_image->empty())
            ss << ind << "setMaskImage('" << target_id << "', '"
               << js_single_quote_escape(*st.mask_image) << "');\n";
        if (st.mask_size && !st.mask_size->empty())
            ss << ind << "setMaskSize('" << target_id << "', '"
               << js_single_quote_escape(*st.mask_size) << "');\n";
        // A shadow is a visual override like any other here, and it belongs on
        // EVERY kind of node — not just frames. The frame branch carries its own
        // copy of this emit (and does not call this lambda), so for a long time
        // a shadow only painted on a frame: a design whose 44 knob bases are
        // ellipses had 49 shadowed nodes in its scene and emitted exactly ONE
        // setBoxShadow, leaving every knob flat. Nothing said so, because the
        // boxes were the right size in the right place — they just had no depth.
        if (!st.box_shadow.empty()) {
            // The bridge takes a single drop shadow; CSS paints the first layer
            // on top, so that is the one that reads.
            const auto& sh = st.box_shadow.front();
            const std::string color = sh.color.empty() ? "#00000050" : sh.color;
            ss << ind << "setBoxShadow('" << target_id << "', "
               << sh.offset_x << ", " << sh.offset_y << ", " << sh.blur << ", " << sh.spread
               << ", '" << js_single_quote_escape(color) << "'"
               << (sh.inset ? ", true" : "") << ");\n";
        }
    };

    // Emit the anchor trail in bridge-native-JS codegen too. Same
    // gate + format as generate_node(), so downstream tooling has one
    // pattern to grep for regardless of which codegen mode produced
    // the JS.
    if (opts.include_comments && node.stable_anchor_id &&
        !node.stable_anchor_id->empty()) {
        ss << ind << "// @pulp-anchor " << *node.stable_anchor_id << "\n";
    }
    // The matching setAnchor(id, anchor) call is emitted by the
    // generate_native_node wrapper once this body has created the widget and
    // recorded its bridge id — see the rationale there.

    // Audio widgets use native widget API
    if (node.audio_widget != AudioWidgetType::none) {
        auto wtype = node.audio_widget;
        float fid_w = 0.0f, fid_h = 0.0f;  // emitted widget dims, set per sub-branch (fidelity)

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
        //
        // ...but ONLY when the design gave us nothing to draw. Once .fig
        // instances expand, a knob carries the designer's own art (frames +
        // vectors), and the caption they wrote is a SIBLING text node — so both
        // label slots on the knob itself read empty and this guard fires,
        // stamping an invented "VALUE" across artwork we were asked to import.
        // That is the overwrite the comment above promises never to do: the
        // check was written when an empty slot really did mean "no label
        // anywhere", which stopped being true.
        const bool design_supplied_art = !node.children.empty();
        if (opts.use_silver_knobs && label_text.empty() && value_text.empty() &&
            !design_supplied_art) {
            label_text = "VALUE";
        }

        if (opts.include_comments && !label_text.empty())
            ss << ind << "// " << label_text << "\n";

        // Helper: emit setWidgetStyle if preview mode
        auto emit_style = [&](const std::string& wid) {
            if (opts.preview_mode)
                ss << ind << "setWidgetStyle('" << wid << "', 'minimal');\n";
        };

        // Format an audio value the way the Pulp Library components do: a whole
        // number prints with no decimals (880, 0, -60), a fractional value with
        // one decimal (-6.0). Generalizable — no per-instance hardcoding.
        auto fmt_audio_value = [](float v) -> std::string {
            std::ostringstream os;
            if (std::fabs(v - std::round(v)) < 0.05f)
                os << static_cast<long long>(std::llround(v));
            else {
                os.setf(std::ios::fixed);
                os.precision(1);
                os << v;
            }
            return os.str();
        };

        // The figma-plugin export carries each widget's value / range / unit /
        // binding as NODE METADATA (audio_min/max/default + attributes
        // units/binding), NOT as child text nodes — the captured PNG bakes them
        // in. Reconstruct the Pulp Library display stack from that metadata so
        // the imported widget reads like the reference: the widget label, then
        // the formatted value, then a small grey sub-stack of min / max / units /
        // binding (decreasing emphasis, grey #6c7086). Emitted below the widget
        // inside its column. Fully generalizable from metadata.
        auto units_attr = node.attributes.find("units");
        std::string units_text = (units_attr != node.attributes.end()) ? units_attr->second : std::string();
        auto binding_attr = node.attributes.find("binding");
        std::string binding_text = (binding_attr != node.attributes.end()) ? binding_attr->second : std::string();
        bool has_value_stack = node.has_audio_range || !units_text.empty() || !binding_text.empty();
        auto emit_value_stack = [&](const std::string& container_id) {
            if (!has_value_stack) return;
            // Primary value (slightly dimmer than the label, larger than the
            // grey sub-stack). Skip when a child-text value label was already
            // emitted by the widget branch (non-figma-plugin import paths), so
            // we never double-print the value.
            if (value_text.empty()) {
                std::string vid = id + "_val";
                ss << ind << "createLabel('" << vid << "', '"
                   << js_single_quote_escape(fmt_audio_value(node.audio_default))
                   << "', '" << container_id << "');\n";
                ss << ind << "setFlex('" << vid << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << vid << "', 10);\n";
                ss << ind << "setTextColor('" << vid << "', '#9399b2');\n";
                ss << ind << "setTextAlign('" << vid << "', 'center');\n";
            }
            // Grey sub-stack: min, max, units, binding (each its own small line).
            int sub_i = 0;
            auto emit_sub = [&](const std::string& text) {
                if (text.empty()) return;
                std::string sid = id + "_sub" + std::to_string(sub_i++);
                ss << ind << "createLabel('" << sid << "', '"
                   << js_single_quote_escape(text) << "', '" << container_id << "');\n";
                ss << ind << "setFlex('" << sid << "', 'height', " << kMinSmallLabelHeight << ");\n";
                ss << ind << "setFontSize('" << sid << "', 9);\n";
                ss << ind << "setTextColor('" << sid << "', '#6c7086');\n";
                ss << ind << "setTextAlign('" << sid << "', 'center');\n";
            };
            if (node.has_audio_range) {
                emit_sub(fmt_audio_value(node.audio_min));
                emit_sub(fmt_audio_value(node.audio_max));
            }
            emit_sub(units_text);
            emit_sub(binding_text);
        };
        // Extra column height to reserve for the value stack (value + up to 4
        // sub-lines), so the wrapper column hugs its content instead of clipping.
        int value_stack_lines = 0;
        if (has_value_stack) {
            value_stack_lines = 1;  // value
            if (node.has_audio_range) value_stack_lines += 2;  // min, max
            if (!units_text.empty()) value_stack_lines += 1;
            if (!binding_text.empty()) value_stack_lines += 1;
        }
        float value_stack_h = static_cast<float>(value_stack_lines) * (kMinSmallLabelHeight + 4.0f);

        // When the IR has NO inlined label or value text children, the widget
        // doesn't need its own wrapper column — the parent (typically a Figma
        // auto-layout row that already arranges siblings) does the layout.
        // Wrapping in a col with +20 padding for absent labels makes the col
        // taller than the parent's hugged row height, breaking Yoga's flex
        // layout.
        bool needs_label_wrapper = !label_text.empty() || !value_text.empty() || has_value_stack;

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
            // Keeps skinned knobs at source size so sprite-strip PNGs are not
            // stretched or allowed to overlap neighboring widgets.
            float shape_w = node.style.width.value_or(kMinKnobSize);
            float shape_h = node.style.height.value_or(kMinKnobSize);
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float frame_w = shape_w;
            float col_h = shape_h + 20 + (value_text.empty() ? 0 : 16) + value_stack_h;
            if (needs_label_wrapper) {
                ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
                ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            }
            fid_w = shape_w; fid_h = shape_h;  // emitted widget dims (fidelity)
            ss << ind << "createKnob('" << id << "', '" << col_id << "');\n";
            if (!needs_label_wrapper) emit_position_if_absolute(id);
            ss << ind << "setFlex('" << id << "', 'width', " << shape_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Clear built-in label — use separate Yoga-positioned labels for exact placement
            ss << ind << "setLabel('" << id << "', ' ');\n";
            // Normalize the captured value to 0..1 — Knob::set_value clamps to
            // [0,1], so a raw audio value (e.g. 880 Hz) would clamp to 1 and
            // park the indicator at the far end. The native silver knob maps
            // 0..1 linearly to its [-135°,+135°] sweep, so the NORMALIZED value
            // must already encode the parameter taper. For a frequency-unit
            // knob (Hz / kHz) use a LOG taper — that's how audio cutoff/freq
            // controls are laid out (and how Figma's library knob is drawn), so
            // 880 Hz in [20, 20000] lands near the center (≈0.55), indicator
            // ~straight up, matching the design. Linear units fall back to the
            // plain (value-min)/(max-min) map. Generalizable rule keyed on the
            // IR's own units attribute — no per-instance angle hardcoding.
            float knob_norm = node.audio_default;
            {
                float lo = node.audio_min, hi = node.audio_max;
                auto uit = node.attributes.find("units");
                std::string units;
                if (uit != node.attributes.end()) {
                    units = uit->second;
                    for (auto& c : units)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                const bool freq_unit = (units == "hz" || units == "khz");
                if (freq_unit && lo > 0.0f && hi > lo && node.audio_default > 0.0f) {
                    float ln_lo = std::log(lo), ln_hi = std::log(hi);
                    knob_norm = std::clamp(
                        (std::log(node.audio_default) - ln_lo) / (ln_hi - ln_lo),
                        0.0f, 1.0f);
                } else if (hi > lo) {
                    knob_norm = std::clamp((node.audio_default - lo) / (hi - lo), 0.0f, 1.0f);
                }
            }
            ss << ind << "setValue('" << id << "', " << knob_norm << ");\n";
            // Attach a designer-supplied sprite-strip skin when the figma-plugin
            // CLI lane (or anyone else) pre-resolved an asset_path onto this knob
            // node. Frame count defaults to 1 (static body); a multi-frame strip
            // lets the indicator rotate by value.
            //
            // When opts.use_silver_knobs is true, emit the native-vector silver
            // render style instead. The sprite-strip PNG path is skipped — the
            // chrome look comes from canvas primitives + radial gradient +
            // indicator notch (WidgetRenderStyle::silver). Crisp at any size,
            // no PNG bleed, no GPU texture upload.
            //
            // Per-node override: a Figma node name ending in "@sprite"
            // or "@silver" overrides the global default for THIS knob
            // only. Lets a designer mark specific knobs as
            // pixel-exact-PNG (e.g. a hero knob whose Figma rendering
            // is intentional) while the rest of the design uses the
            // crisper vector path. Convention rationale: same `@` style
            // Figma uses for variants (`Knob/State=hover`) and that
            // Mitosis / Penpot adopted for code-target hints.
            // Parse the figma-plugin per-node knob-style suffix.
            // Tolerates:
            //   - case variation (`@Sprite`, `@SILVER`)
            //   - trailing whitespace (Figma's rename UI keeps it)
            //   - Figma variant separator (`Knob@sprite/State=hover`)
            //   - Figma's `Knob/Hero@sprite` component-instance pattern
            // Strictly matches "@sprite" or "@silver" as a complete tag
            // at the end of the name OR before a variant separator. We
            // do NOT match inside the middle of the name to avoid
            // catching layer comments like "Knob@sprite-old-style".
            bool node_wants_sprite = false;
            bool node_wants_silver = false;
            {
                std::string n = node.name;
                // Trim trailing whitespace.
                while (!n.empty() &&
                       std::isspace(static_cast<unsigned char>(n.back())))
                    n.pop_back();
                // Cut at Figma variant separator (`@sprite/State=hover`
                // → `@sprite`). Only the segment containing the @-tag
                // matters for the suffix.
                if (auto slash = n.find_last_of('/'); slash != std::string::npos) {
                    // Only consider the slash a "variant cut" if there's
                    // an @-tag before it AND no @-tag after it. Otherwise
                    // it's part of a component path (`Knob/Hero@sprite`),
                    // which we want to keep intact for the lowercase
                    // ends-with check below.
                    auto post = n.substr(slash + 1);
                    auto pre = n.substr(0, slash);
                    if (pre.find('@') != std::string::npos &&
                        post.find('@') == std::string::npos) {
                        n = pre;
                    }
                }
                // Lowercase the whole string for case-insensitive matching.
                for (auto& c : n) c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
                auto ends_with = [&](std::string_view suf) {
                    if (n.size() < suf.size()) return false;
                    return n.compare(n.size() - suf.size(), suf.size(), suf) == 0;
                };
                if (ends_with("@sprite")) node_wants_sprite = true;
                else if (ends_with("@silver")) node_wants_silver = true;
            }
            bool use_silver_here =
                node_wants_silver ? true :
                node_wants_sprite ? false :
                opts.use_silver_knobs;

            auto skin_it = node.attributes.find("asset_path");
            if (use_silver_here) {
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
                // Core-fit: when the importer recovered the body art's opaque
                // core (single-frame sprite bodies only), pass it so the engine
                // scales the disc to fill the knob box (shadow bleed extends
                // beyond) and the native rotating indicator sweeps within it.
                // Same have_core data the image branch uses — no hardcoding.
                if (frames == 1) {
                    auto kattr_f = [&](const char* k) -> float {
                        auto a = node.attributes.find(k);
                        return a != node.attributes.end()
                                   ? std::strtof(a->second.c_str(), nullptr) : 0.0f;
                    };
                    const float cw = kattr_f("art_core_w");
                    const float ch = kattr_f("art_core_h");
                    if (cw > 0.0f && ch > 0.0f) {
                        ss << ind << "setKnobSpriteCore('" << id << "', "
                           << kattr_f("art_core_x") << ", " << kattr_f("art_core_y")
                           << ", " << cw << ", " << ch << ");\n";
                    }
                }
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
            emit_value_stack(col_id);
        }
        else if (wtype == AudioWidgetType::fader) {
            // shape_width/height from child rectangle, frame width for column.
            // Default height to the node's own design height (not the bare
            // kMin floor) so a tall fader stays tall — the kMin is only a
            // floor for nodes that genuinely carry no height.
            float frame_w = node.style.width.value_or(kMinFaderWidth);
            float shape_w = frame_w;
            float shape_h = node.style.height.value_or(kMinFaderHeight);
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            // Use frame width for column, but ensure widget is at least usable
            float widget_w = std::max(shape_w, 6.0f);
            float col_h = shape_h + 20 + value_stack_h;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            fid_w = widget_w; fid_h = shape_h;  // emitted widget dims (fidelity)
            ss << ind << "createFader('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Fader label overlaps track when rendered inside bounds — use separate label
            ss << ind << "setLabel('" << id << "', ' ');\n";  // Clear built-in label
            // Normalize the captured value (audio_default, in [audio_min,
            // audio_max]) to 0..1 so the imported fader's thumb sits where the
            // capture shows it. setValue clamps to [0,1]; a raw value like a dB
            // figure would clamp and mis-position the thumb.
            float fader_norm = node.audio_default;
            {
                float lo = node.audio_min, hi = node.audio_max;
                if (hi > lo) fader_norm = std::clamp((node.audio_default - lo) / (hi - lo), 0.0f, 1.0f);
            }
            // Prefer the captured thumb position when the sampler recovered it:
            // an audio fader's value→position map is non-linear, so the
            // linear seed above lands the thumb wrong; the captured position
            // reproduces where the design drew it.
            if (auto pit = node.attributes.find("skin_thumb_position");
                pit != node.attributes.end() && !pit->second.empty())
                fader_norm = std::clamp(std::stof(pit->second), 0.0f, 1.0f);
            ss << ind << "setValue('" << id << "', " << fader_norm << ");\n";
            // Value-driven skin derived from the captured asset. The importer
            // sampled the PNG's track/fill/thumb colors; emit setFaderSkin so
            // the native fader renders the captured look while the thumb still
            // moves with setValue(). No per-instance hardcoding: every value
            // comes from node.attributes stamped by the sampler.
            if (opts.skin_faders) {
                auto attr = [&](const char* k) -> std::string {
                    auto it = node.attributes.find(k);
                    return it != node.attributes.end() ? it->second : std::string();
                };
                std::string tc = attr("skin_track_color");
                std::string fc = attr("skin_fill_color");
                std::string thc = attr("skin_thumb_color");
                std::string tbc = attr("skin_thumb_border_color");
                if (!tc.empty() || !fc.empty() || !thc.empty() || !tbc.empty()) {
                    ss << ind << "setFaderSkin('" << id << "', '"
                       << tc << "', '" << fc << "', '" << thc << "', '" << tbc << "');\n";
                }
                // Derived thin track width (logical px). Drives the fader's
                // track/fill thickness so it draws the narrow captured line
                // instead of a fraction of the wide widget box.
                if (node.attributes.count("skin_track_width")) {
                    ss << ind << "setFaderTrackWidth('" << id << "', "
                       << node.attributes.at("skin_track_width") << ");\n";
                }
                // Derived empty-track outline color. Strokes the track rect
                // so the empty channel above the thumb shows the captured edge
                // instead of a flat dark slab.
                std::string tbo = attr("skin_track_border_color");
                if (!tbo.empty()) {
                    ss << ind << "setFaderTrackBorder('" << id << "', '" << tbo << "');\n";
                }
            }
            emit_style(id);
            if (!label_text.empty()) {
                std::string lbl_id = id + "_lbl";
                ss << ind << "createLabel('" << lbl_id << "', '" << js_single_quote_escape(label_text) << "', '" << col_id << "');\n";
                ss << ind << "setFlex('" << lbl_id << "', 'height', " << kMinLabelHeight << ");\n";
                ss << ind << "setFontSize('" << lbl_id << "', 11);\n";
                ss << ind << "setTextColor('" << lbl_id << "', '#a6adc8');\n";
                ss << ind << "setTextAlign('" << lbl_id << "', 'center');\n";
            }
            emit_value_stack(col_id);
        }
        else if (wtype == AudioWidgetType::meter) {
            float frame_w = node.style.width.value_or(kMinMeterWidth);
            float shape_w = frame_w;
            // Honor the node's design height so a tall meter stays tall.
            float shape_h = node.style.height.value_or(kMinMeterHeight);
            if (node.attributes.count("shape_width"))
                shape_w = std::stof(node.attributes.at("shape_width"));
            if (node.attributes.count("shape_height"))
                shape_h = std::stof(node.attributes.at("shape_height"));
            float widget_w = std::max(shape_w, 8.0f);
            float col_h = shape_h + 20 + value_stack_h;
            ss << ind << "setFlex('" << col_id << "', 'height', " << col_h << ");\n";
            ss << ind << "setFlex('" << col_id << "', 'min_width', " << frame_w << ");\n";
            fid_w = widget_w; fid_h = shape_h;  // emitted widget dims (fidelity)
            ss << ind << "createMeter('" << id << "', 'vertical', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << widget_w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << shape_h << ");\n";
            // Initial level: normalize the captured value (audio_default, in
            // [audio_min, audio_max]) to 0..1 so the imported meter shows the
            // captured fill. setMeterLevel clamps to [0,1]; a raw dB value
            // (e.g. -6) would clamp to 0 and read empty.
            float meter_norm = 0.0f;
            {
                float lo = node.audio_min, hi = node.audio_max;
                if (hi > lo) meter_norm = std::clamp((node.audio_default - lo) / (hi - lo), 0.0f, 1.0f);
            }
            // Prefer the captured fill level when recovered; it matches where
            // the design filled the meter rather than a linear dB map.
            if (auto lit = node.attributes.find("skin_fill_level");
                lit != node.attributes.end() && !lit->second.empty())
                meter_norm = std::clamp(std::stof(lit->second), 0.0f, 1.0f);
            ss << ind << "setMeterLevel('" << id << "', " << meter_norm << ", " << meter_norm << ");\n";
            // Value-driven gradient skin sampled from the captured meter PNG.
            // setMeterColors hands the meter the recovered gradient stops
            // (low→high); the meter redraws them CLIPPED to the level so the
            // fill still animates with setMeterLevel(). No hardcoding.
            if (opts.skin_meters) {
                auto grad_it = node.attributes.find("skin_meter_gradient");
                if (grad_it != node.attributes.end() && !grad_it->second.empty()) {
                    std::string bg;
                    if (auto bg_it = node.attributes.find("skin_meter_background");
                        bg_it != node.attributes.end())
                        bg = bg_it->second;
                    ss << ind << "setMeterColors('" << id << "', '" << bg << "', '"
                       << grad_it->second << "');\n";
                }
                // Colored-bar/housing width ratio. Insets the gradient bar so
                // it reads as a recessed fill in the wider dark housing,
                // matching the captured meter's structure.
                if (node.attributes.count("skin_meter_bar_ratio")) {
                    ss << ind << "setMeterBarRatio('" << id << "', "
                       << node.attributes.at("skin_meter_bar_ratio") << ");\n";
                }
            }
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
            emit_value_stack(col_id);
        }
        else if (wtype == AudioWidgetType::xy_pad) {
            float sz = std::max(node.style.width.value_or(100.0f), 80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (sz + 20) << ");\n";
            fid_w = sz; fid_h = sz;  // emitted widget dims (fidelity)
            ss << ind << "createXYPad('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << sz << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << sz << ");\n";
        }
        else if (wtype == AudioWidgetType::waveform || wtype == AudioWidgetType::spectrum) {
            float w = node.style.width.value_or(200.0f);
            float h = node.style.height.value_or(80.0f);
            ss << ind << "setFlex('" << col_id << "', 'height', " << (h + 20) << ");\n";
            auto fn = (wtype == AudioWidgetType::waveform) ? "createWaveform" : "createSpectrum";
            fid_w = w; fid_h = h;  // emitted widget dims (fidelity)
            ss << ind << fn << "('" << id << "', '" << col_id << "');\n";
            ss << ind << "setFlex('" << id << "', 'width', " << w << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << h << ");\n";
        }

        // Reference-free fidelity self-checks for this widget (see design_fidelity).
        if (opts.fidelity_report)
            run_fidelity_checks({node, id, fid_w, fid_h, FidelityElement::widget},
                                *opts.fidelity_report);
        ss << "\n";
        return;
    }

    // Vector / path node carrying SVG path-data → native SvgPathWidget.
    // Terminal: it renders the path itself, so codegen does not descend into
    // children (mirrors the image / text / widget terminals). Sources that ship
    // a path `d` (Pencil / Stitch / v0 / Claude / RN SVG) lower here instead of
    // silently dropping to an empty frame (the dropped-vector invariant); the
    // figma lane rasterizes vectors to PNG and takes the image branch instead.
    {
        // Any vector/path-like kind carrying path-data lowers here — including
        // the rect/line/ellipse/polygon/star primitives whose `d` the importer
        // synthesizes from geometry (synthesize_primitive_paths). Shares
        // is_vector_kind with the dropped-vector invariant so the two never
        // disagree about what counts as a path node.
        const bool is_path_kind = is_vector_kind(node.type);
        auto pd = node.attributes.find("path_data");
        if (is_path_kind && pd != node.attributes.end() && !pd->second.empty()) {
            if (opts.include_comments && !node.name.empty() && depth > 0)
                ss << ind << "// " << node.name << "\n";
            ss << ind << "createSvgPath('" << id << "', " << pid << ");\n";
            emit_position_if_absolute(id);
            emit_node_visual_overrides(id);
            ss << ind << "setSvgPath('" << id << "', '"
               << js_single_quote_escape(pd->second) << "');\n";
            // viewBox is "minX minY width height" — the widget scales the path
            // into its box from the (width, height) pair.
            if (auto vb = node.attributes.find("svg_viewbox");
                vb != node.attributes.end()) {
                const char* p = vb->second.c_str();
                char* end = nullptr;
                float vals[4]; int got = 0;
                while (got < 4) {
                    float v = std::strtof(p, &end);
                    if (end == p) break;
                    vals[got++] = v; p = end;
                }
                if (got == 4 && vals[2] > 0.0f && vals[3] > 0.0f)
                    ss << ind << "setSvgViewBox('" << id << "', "
                       << vals[2] << ", " << vals[3] << ");\n";
            }
            if (auto f = node.attributes.find("svg_fill"); f != node.attributes.end())
                ss << ind << "setSvgFill('" << id << "', '"
                   << js_single_quote_escape(f->second) << "');\n";
            // Emission order is irrelevant — SvgPathWidget resolves the two at
            // paint time, preferring the gradient and using the solid only as
            // the parse-failure fallback — but the solid is emitted first so
            // the generated JS reads the way it resolves.
            if (auto g = node.attributes.find("svg_fill_gradient");
                g != node.attributes.end() && !g->second.empty())
                ss << ind << "setSvgFillGradient('" << id << "', '"
                   << js_single_quote_escape(g->second) << "');\n";
            if (auto s = node.attributes.find("svg_stroke"); s != node.attributes.end())
                ss << ind << "setSvgStroke('" << id << "', '"
                   << js_single_quote_escape(s->second) << "');\n";
            if (auto sw = node.attributes.find("svg_stroke_width");
                sw != node.attributes.end())
                ss << ind << "setSvgStrokeWidth('" << id << "', " << sw->second << ");\n";
            const float vw = node.style.width.value_or(0.0f);
            const float vh = node.style.height.value_or(0.0f);
            if (vw > 0.0f) ss << ind << "setFlex('" << id << "', 'width', " << vw << ");\n";
            if (vh > 0.0f) ss << ind << "setFlex('" << id << "', 'height', " << vh << ");\n";
            if (opts.fidelity_report)
                run_fidelity_checks({node, id, vw, vh, FidelityElement::container},
                                    *opts.fidelity_report);
            ss << "\n";
            return;
        }
    }

    // Container, image, or text node
    bool is_image = (node.type == "image" || node.attributes.count("asset_path") > 0);
    bool is_container = !is_image && (!node.children.empty() || node.type == "frame");
    bool is_text = (node.type == "text" || node.type == "label");
    bool is_row = (node.layout.direction == LayoutDirection::row);
    // A grid container lowers to the native grid layout (createGrid +
    // LayoutMode::grid) instead of flex. Signal: display:grid or an explicit
    // track template. Pulp's engine owns the grid layout (no Yoga grid needed).
    bool is_grid = is_container &&
        ((node.layout.display && *node.layout.display == "grid") ||
         node.layout.grid_template_columns || node.layout.grid_template_rows);

    if (is_image) {
        // Image node → createImage + setImageSource. Honors absolute positioning
        // emitted from the figma-plugin lane. asset_path is pre-resolved to an
        // absolute filesystem path by the CLI's asset resolution pass; nodes
        // missing the attribute (bare type=image callers) fall
        // through with no source set.
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";
        ss << ind << "createImage('" << id << "', " << pid << ");\n";
        emit_position_if_absolute(id);
        emit_node_visual_overrides(id);
        // Opacity. The frame path emits this further down, but an image node
        // returns before reaching it and emit_node_visual_overrides covers only
        // blend/clip/mask — so an image's opacity was dropped entirely. A design
        // that dims an overlay to 10% (a grain/noise texture over the whole UI
        // is the canonical case) rendered it at FULL strength, burying the
        // interface under it.
        if (node.style.opacity)
            ss << ind << "setOpacity('" << id << "', " << *node.style.opacity << ");\n";
        auto it = node.attributes.find("asset_path");
        if (it != node.attributes.end() && !it->second.empty()) {
            ss << ind << "setImageSource('" << id << "', '"
               << js_single_quote_escape(it->second) << "');\n";
        }
        // Sprite sizing. A captured sprite (knob graphic, etc.) is exported as
        // a PNG that bleeds well past its layout box: the solid art sits in
        // part of the PNG with a soft drop-shadow/glow filling the rest. The
        // renderer always stretches the PNG to its element box (setObjectFit
        // is storage-only), so to avoid skew + get the size right we size the
        // ELEMENT to the PNG's real aspect and place it so the art's SOLID
        // CORE lands on the layout box. The solid-core bbox + true pixel dims
        // are recovered from the PNG itself at asset resolution
        // (art_core_*/png_natural_*); nothing is hardcoded and render_bounds —
        // which is unreliable for scaled component instances (e.g. a knob
        // whose render_bounds aspect is 1.81 while its PNG is 0.87) — is not
        // trusted for
        // sizing. Falls back to the declared box size when no core data exists.
        const float box_w = node.style.width.value_or(0.0f);
        const float box_h = node.style.height.value_or(0.0f);
        auto attr_f = [&](const char* k) -> float {
            auto it = node.attributes.find(k);
            return it != node.attributes.end() ? std::strtof(it->second.c_str(), nullptr) : 0.0f;
        };
        const float png_w = attr_f("png_natural_w");
        const float png_h = attr_f("png_natural_h");
        const float core_w = attr_f("art_core_w");
        const float core_h = attr_f("art_core_h");
        const float core_x = attr_f("art_core_x");
        const float core_y = attr_f("art_core_y");
        const bool have_core = png_w > 0.0f && png_h > 0.0f &&
                               core_w > 0.0f && core_h > 0.0f &&
                               box_w > 0.0f && box_h > 0.0f;
        // A sprite "bleeds" past its box either because the export carried a
        // render_bounds extent OR because the importer's pixel-vs-box heuristic
        // stamped asset_bleed=1 (PNG dims ≫ layout box). Both mark art that
        // must preserve its source aspect; an asset_bleed-only sprite has no
        // render_bounds, and object-fit is storage-only, so without this it
        // would size to its box and skew.
        const bool is_bleed_sprite =
            node.style.render_bounds.has_value() ||
            (node.attributes.count("asset_bleed") &&
             node.attributes.at("asset_bleed") == "1");

        // Capture the dimensions actually emitted so the fidelity self-check
        // can verify them against the source PNG aspect (no-skew invariant).
        float emitted_w = 0.0f, emitted_h = 0.0f;

        if (have_core) {
            // Uniform scale that fits the solid core inside the layout box
            // (preserves aspect — never skews). The whole PNG scales by `s`,
            // so the shadow extends naturally beyond the box.
            const float s = std::min(box_w / core_w, box_h / core_h);
            const float ew = png_w * s;
            const float eh = png_h * s;
            emitted_w = ew; emitted_h = eh;
            ss << ind << "setFlex('" << id << "', 'width', " << ew << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << eh << ");\n";
            // Place so the core's top-left lands on the layout box, then nudge
            // so the core is centered within the box on each axis.
            const float core_box_w = core_w * s, core_box_h = core_h * s;
            const float pad_x = (box_w - core_box_w) * 0.5f;
            const float pad_y = (box_h - core_box_h) * 0.5f;
            if (node.style.position && *node.style.position == "absolute") {
                if (node.style.left)
                    ss << ind << "setLeft('" << id << "', " << (*node.style.left - core_x * s + pad_x) << ");\n";
                if (node.style.top)
                    ss << ind << "setTop('" << id << "', " << (*node.style.top - core_y * s + pad_y) << ");\n";
            }
        } else if (is_bleed_sprite && png_w > 0.0f && png_h > 0.0f &&
                   box_w > 0.0f && box_h > 0.0f) {
            // A bleed sprite (render_bounds or asset_bleed) whose opaque core
            // couldn't be recovered → contain-fit preserving aspect. Gated on
            // the bleed markers: an ORDINARY image/icon must keep the box the
            // IR declared (a 100×100 node with a 200×100 bitmap fills its
            // 100×100 slot, as Figma's image-fill intends), so aspect-
            // preservation is limited to bleed sprites and never reshapes
            // normal images.
            const float png_aspect = png_w / png_h;
            float ew = box_w, eh = box_h;
            if (box_w / box_h > png_aspect) { eh = box_h; ew = box_h * png_aspect; }
            else                            { ew = box_w; eh = box_w / png_aspect; }
            emitted_w = ew; emitted_h = eh;
            ss << ind << "setFlex('" << id << "', 'width', " << ew << ");\n";
            ss << ind << "setFlex('" << id << "', 'height', " << eh << ");\n";
            // Center the contained element within the declared box — the bleed
            // art is documented as centered, and leaving the original top/left
            // (from emit_position_if_absolute) would pin the reduced element to
            // the box's top-left, visibly shifting it.
            if (node.style.position && *node.style.position == "absolute") {
                const float pad_x = (box_w - ew) * 0.5f;
                const float pad_y = (box_h - eh) * 0.5f;
                if (node.style.left)
                    ss << ind << "setLeft('" << id << "', " << (*node.style.left + pad_x) << ");\n";
                if (node.style.top)
                    ss << ind << "setTop('" << id << "', " << (*node.style.top + pad_y) << ");\n";
            }
        } else {
            // Ordinary image (no bleed) or unknown dims → keep the declared box.
            if (node.style.width)
                ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
            if (node.style.height)
                ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
            emitted_w = node.style.width.value_or(0.0f);
            emitted_h = node.style.height.value_or(0.0f);
            auto bleed_it = node.attributes.find("asset_bleed");
            if (bleed_it != node.attributes.end() && bleed_it->second == "1")
                ss << ind << "setObjectFit('" << id << "', 'none');\n";
        }
        // Reference-free fidelity self-checks for this image (see design_fidelity).
        if (opts.fidelity_report)
            run_fidelity_checks({node, id, emitted_w, emitted_h, FidelityElement::image},
                                *opts.fidelity_report);
        ss << "\n";
        return;
    }

    if (is_text) {
        // Text node → createLabel with explicit height (Yoga requirement)
        ss << ind << "createLabel('" << id << "', '" << js_single_quote_escape(node.text_content) << "', " << pid << ");\n";
        emit_position_if_absolute(id);
        emit_node_visual_overrides(id);

        // Honor the IR-declared height when present; absolute-positioned labels
        // that are centered in a slot rely on the emitted height matching that
        // slot rather than being recomputed from font size alone.
        float font_h = node.style.font_size.value_or(14.0f);
        bool ir_height_is_explicit = node.style.height.has_value();
        float label_h = ir_height_is_explicit
            ? std::max(*node.style.height, font_h * 1.0f)
            : std::max(font_h * 1.4f, kMinLabelHeight);
        ss << ind << "setFlex('" << id << "', 'height', " << label_h << ");\n";
        // When the IR carries an explicit height that's meaningfully taller
        // than the font (Figma's Auto-Layout / text-frame conventions use
        // a height-greater-than-font-size to RESERVE a vertical slot the
        // text is supposed to be CENTERED within), emit setVerticalAlign:
        // center so Pulp's Label draws its glyphs at the slot's optical
        // middle. Without this Label defaults to top-aligned, and the
        // SEARCH input's "Search" text rides above the magnifying-glass
        // icon instead of baseline-aligning with it.
        bool emitted_vcenter = false;
        if (ir_height_is_explicit && label_h > font_h * 1.15f) {
            ss << ind << "setVerticalAlign('" << id << "', 'center');\n";
            emitted_vcenter = true;
        }

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
        // Per-range styled text → native styled runs (the bridge builds a
        // canvas::AttributedString; mirrors the web nested-<span> path). Offsets
        // are UTF-8 byte offsets, matching emit_web_text_runs.
        if (!node.text_runs.empty()) {
            ss << ind << "setTextRuns('" << id << "', [";
            for (size_t ri = 0; ri < node.text_runs.size(); ++ri) {
                const auto& r = node.text_runs[ri];
                if (ri) ss << ", ";
                ss << "{ start: " << r.start << ", end: " << r.end;
                if (r.font_weight) ss << ", fontWeight: " << *r.font_weight;
                if (r.font_size) ss << ", fontSize: " << *r.font_size;
                if (r.font_style) ss << ", fontStyle: '" << js_single_quote_escape(*r.font_style) << "'";
                if (r.color) ss << ", color: '" << js_single_quote_escape(*r.color) << "'";
                if (r.letter_spacing) ss << ", letterSpacing: " << *r.letter_spacing;
                ss << " }";
            }
            ss << "]);\n";
        }
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

            // Multi-line text box: when the design's own box is taller than a
            // single line of this font, the designer intended the string to
            // WRAP within its declared width (a paragraph / subtitle), so emit
            // the box WIDTH (a hard bound, not just a min) and put the label in
            // multi-line mode. Without this a long string runs off the parent
            // (visible bug: the smoke-test subtitle at width 720 overflowed the
            // panel). The decision keys on the IR's own height vs. font size —
            // generalizable, no per-node hardcoding. A single-line label
            // (height ≈ one line, e.g. a title) is intentionally NOT bounded
            // here: forcing its narrow hug-width as a hard wrap box would make
            // it wrap when Pulp's font metrics run a hair wider than Figma's,
            // breaking a title that the design drew on one line.
            // Multi-line only when the box clears ~1.8 line boxes. Prefer the
            // IR's declared line_height; when absent assume a tight font*1.2
            // (a single rendered line), NOT font*1.4 — the looser fallback let
            // a genuine two-line paragraph (e.g. 26px box at 11px font) read as
            // one line. A single line of small text in a tall padded box (e.g.
            // a 17px search field around 8px text, line_height 9.84) still
            // stays single. Generalizable: reads Figma's declared metrics.
            const float line_h = node.style.line_height.value_or(font_h * 1.2f);
            bool multiline_box =
                node.style.height && *node.style.height > line_h * 1.8f;
            if (multiline_box) {
                ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
                ss << ind << "setMultiLine('" << id << "', true);\n";
            }
        }

        // Reference-free fidelity self-check for this text (see design_fidelity):
        // a single-line label given a tall slot must be vertically centered.
        // node is const here, so stamp the emitted vertical-align onto a copy.
        if (opts.fidelity_report) {
            IRNode fnode = node;
            // When the IR carried no explicit height, label_h is synthesized
            // from the font (font_h * 1.4) — there is no design-reserved slot,
            // so stamp "n-a" and the text check self-skips. Only an explicit
            // taller-than-font slot is held to the vertical-centering invariant.
            fnode.attributes["_emitted_vertical_align"] =
                !ir_height_is_explicit ? "n-a" : (emitted_vcenter ? "center" : "top");
            run_fidelity_checks({fnode, id, 0.0f, label_h, FidelityElement::text},
                                *opts.fidelity_report);
        }
        ss << "\n";
        return;
    }

    if (is_container) {
        // Container → createRow or createCol
        if (opts.include_comments && !node.name.empty() && depth > 0)
            ss << ind << "// " << node.name << "\n";

        ss << ind << (is_grid ? "createGrid" : (is_row ? "createRow" : "createCol"))
           << "('" << id << "', " << pid << ");\n";
        emit_position_if_absolute(id);
        emit_node_visual_overrides(id);
        if (is_grid) {
            // The native grid layout drops all children when its column track
            // list is empty, so a `display:grid` node with no explicit columns
            // (only rows, or none) gets a single implicit column rather than
            // vanishing — matches CSS auto-column behavior closely enough.
            if (node.layout.grid_template_columns)
                ss << ind << "setGrid('" << id << "', 'template_columns', '"
                   << js_single_quote_escape(*node.layout.grid_template_columns) << "');\n";
            else
                ss << ind << "setGrid('" << id << "', 'template_columns', '1fr');\n";
            if (node.layout.grid_template_rows)
                ss << ind << "setGrid('" << id << "', 'template_rows', '"
                   << js_single_quote_escape(*node.layout.grid_template_rows) << "');\n";
            if (node.layout.grid_auto_flow)
                ss << ind << "setGrid('" << id << "', 'auto_flow', '"
                   << js_single_quote_escape(*node.layout.grid_auto_flow) << "');\n";
        }

        // Yoga: every container MUST have explicit height
        // Priority: _layoutHeight (from snapshot_layout) > style.height > fill > computed
        float fidelity_emitted_h = 0.0f;  // captured for the gross-size self-check
        if (node.attributes.count("_layoutHeight")) {
            int lh = std::stoi(node.attributes.at("_layoutHeight"));
            if (lh > 0) {
                ss << ind << "setFlex('" << id << "', 'height', " << lh << ");\n";
                fidelity_emitted_h = static_cast<float>(lh);
            }
        } else if (node.style.height) {
            ss << ind << "setFlex('" << id << "', 'height', " << *node.style.height << ");\n";
            fidelity_emitted_h = *node.style.height;
        } else if (node.layout.height_mode == SizingMode::fill) {
            ss << ind << "setFlex('" << id << "', 'flex_grow', 1);\n";
        } else {
            float est = compute_container_height(node);
            ss << ind << "setFlex('" << id << "', 'height', " << est << ");\n";
            fidelity_emitted_h = est;
        }

        // Width: use _layoutWidth if available, then style.width
        float fidelity_emitted_w = 0.0f;  // captured for the gross-size self-check
        if (node.attributes.count("_layoutWidth")) {
            int lw = std::stoi(node.attributes.at("_layoutWidth"));
            if (lw > 0) {
                ss << ind << "setFlex('" << id << "', 'width', " << lw << ");\n";
                fidelity_emitted_w = static_cast<float>(lw);
            }
        } else if (node.style.width) {
            ss << ind << "setFlex('" << id << "', 'width', " << *node.style.width << ");\n";
            fidelity_emitted_w = *node.style.width;
        }

        // Reference-free fidelity self-checks for this container (see design_fidelity).
        if (opts.fidelity_report)
            run_fidelity_checks({node, id, fidelity_emitted_w, fidelity_emitted_h,
                                 FidelityElement::container}, *opts.fidelity_report);

        if (node.layout.gap > 0)
            ss << ind << (is_grid ? "setGrid('" : "setFlex('") << id
               << "', 'gap', " << node.layout.gap << ");\n";

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

        // Single-child space_between → center.
        // Figma designers commonly mark a flex container "space-between" to
        // mean "spread items out", then drop a single child in. With one
        // child, CSS / Yoga space-between degenerates to flex-start, so
        // the lone item left-aligns instead of centering — visible bug
        // on a numbered tab button (e.g. "1" "2" "3" "4" sitting at the
        // left edge of their 29×20 button boxes). When the IR
        // says space_between AND there's exactly one child, the design
        // intent is centering — emit that.
        // Flex alignment (justify-content / align-items) does not apply to a
        // grid container — its track template + item placement drive layout —
        // so emit it for the flex path only (guarded with !is_grid).
        LayoutAlign effective_justify = node.layout.justify;
        if (effective_justify == LayoutAlign::space_between &&
            node.children.size() == 1) {
            effective_justify = LayoutAlign::center;
        }
        if (!is_grid && effective_justify != LayoutAlign::flex_start)
            ss << ind << "setFlex('" << id << "', 'justify_content', '" << align_to_css(effective_justify) << "');\n";
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
        if (!is_grid && baseline_override) {
            ss << ind << "setFlex('" << id << "', 'align_items', 'baseline');\n";
        } else if (!is_grid && node.layout.align != LayoutAlign::stretch)
            ss << ind << "setFlex('" << id << "', 'align_items', '" << align_to_css(node.layout.align) << "');\n";

        // Visual styles
        if (node.style.background_color)
            ss << ind << "setBackground('" << id << "', '" << *node.style.background_color << "');\n";
        if (node.style.background_gradient)
            ss << ind << "setBackgroundGradient('" << id << "', '" << js_single_quote_escape(*node.style.background_gradient) << "');\n";
        if (node.style.border_radius)
            ss << ind << "setCornerRadius('" << id << "', 'All', " << *node.style.border_radius << ");\n";
        // Emit border (Figma frame stroke) as setBorder(id, color, width).
        // Column frames inside a gradient panel can carry a 1px rgba(...)
        // border that Figma renders as the thin vertical separators between
        // columns. The bridge's parseColor accepts both hex and rgba(...), so
        // pass border_color verbatim from the IR.
        if (node.style.border_color && node.style.border_width &&
            *node.style.border_width > 0.0f) {
            float br = node.style.border_radius.value_or(0.0f);
            ss << ind << "setBorder('" << id << "', '"
               << js_single_quote_escape(*node.style.border_color) << "', "
               << *node.style.border_width << ", " << br << ");\n";
        }
        if (!node.style.box_shadow.empty()) {
            // The IR carries parsed CSS box-shadow layers. The bridge's
            // setBoxShadow(id, ox, oy, blur, spread, color, inset?) takes a
            // single drop shadow; emit the first layer (CSS paints the first
            // layer on top). An omitted color falls back to the bridge's
            // default tint rather than letting setBoxShadow guess numerics.
            const auto& sh = node.style.box_shadow.front();
            const std::string color = sh.color.empty() ? "#00000050" : sh.color;
            ss << ind << "setBoxShadow('" << id << "', "
               << sh.offset_x << ", " << sh.offset_y << ", " << sh.blur << ", " << sh.spread
               << ", '" << js_single_quote_escape(color) << "'"
               << (sh.inset ? ", true" : "") << ");\n";
        }
        if (node.style.opacity)
            ss << ind << "setOpacity('" << id << "', " << *node.style.opacity << ");\n";

        ss << "\n";

        // Recurse children.
        // For space_between rows with TWO OR MORE text children (the canonical
        // "label … value" or "key … units" pattern), right-align the LAST
        // text child so the value hugs the right edge. Single-text-child rows
        // keep their own alignment so dropdown labels do not right-align into
        // trailing image controls.
        bool is_space_between = is_row && node.layout.justify == LayoutAlign::space_between;
        int text_child_count = 0;
        if (is_space_between) {
            for (auto& child : node.children) {
                if (child.type == "text" || child.type == "label") ++text_child_count;
            }
        }
        // Cap-height nudge for [small icon, UPPERCASE label] header rows.
        // Figma vertically centers icons on the label's cap-height optical
        // center. CSS / Yoga `align-items: center` uses the line-box math
        // center, which sits ~font_size * 0.15 BELOW the cap-glyph optical
        // center because the line box reserves descender slack the
        // uppercase glyphs don't occupy. Pulp's Label::resolved_state()
        // produces the same math-center baseline, so the dot ends up
        // visually below the label glyphs. Generalisable rule: when a row
        // has align_items: center, at least one uppercase text child, and
        // any image child whose min-dim ≤ that label's font_size, emit a
        // negative margin_top on the icon so its center lifts to the
        // cap-glyph center. No hardcoded constants — the nudge is derived
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
        // adjusted box). So to lift the icon's center by font_size * 0.15
        // (the cap-vs-math center delta for an uppercase line-box) we
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
            generate_native_node(ss, child, opts, depth + 1, var_counter, id, id_map);
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
    emit_layout_constraints(id);
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

    // Tag the bundle with a motion-observability provenance
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

    // Register bundled (non-system) fonts BEFORE any setFontFamily, so a
    // family like "Clash Grotesk" / "Inter" resolves to the shipped face
    // instead of silently falling back to a same-named system font (or a
    // generic). resolved_path is the absolute .ttf/.otf path stamped by the
    // CLI's font asset-resolution pass; skip entries that didn't resolve.
    if (!ir.font_family_assets.empty()) {
        bool any = false;
        for (const auto& fa : ir.font_family_assets) {
            if (fa.family.empty() || fa.resolved_path.empty()) continue;
            if (!any && opts.include_comments) ss << "// Bundled fonts\n";
            any = true;
            ss << "registerFont('" << js_single_quote_escape(fa.family) << "', '"
               << js_single_quote_escape(fa.resolved_path) << "');\n";
        }
        if (any) ss << "\n";
    }

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

    // Auto-imported keyboard shortcuts:
    // register a native intercept per detected chord; the handler thunk
    // re-dispatches a synthetic W3C keydown into __dispatch__ so the
    // original React handlers in the bundled JS (which still own the
    // component-state closures) fire naturally. We don't try to port the
    // handler bodies — the OS-level intercept and the JS-level closure
    // semantics each stay in their natural home.
    if (!opts.shortcuts.empty()) {
        if (opts.include_comments) {
            ss << "// Auto-imported keyboard shortcuts. Each\n"
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
        // Escape a key string for a single-quoted JS literal. Printable keys can
        // include characters like `'` and `\`; escape them at emission time so
        // generated shortcut thunks remain valid JS.
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

            // Preserve Ctrl-only / Meta-only / cross-platform `metaKey||ctrlKey`
            // intent. If BOTH modifiers were collected we emit two bindings so
            // the user can hit either physical chord (Cmd on macOS, Ctrl on
            // Win/Linux) and each synthetic event carries the right modifier flags.
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
        // Native-bridge JS API.
        //
        // First synthesize SVG path-data for bare vector shape primitives
        // (rect/line/ellipse/polygon/star) on a mutable copy of the tree, so
        // both the emit walk below and the dropped-vector fidelity pass see the
        // synthesized `path_data` and lower the shape to a native SvgPath
        // instead of dropping it to an empty frame. The copy keeps the caller's
        // IR untouched; the web-compat arm (which does not own the dropped-shape
        // fall-through) is intentionally not affected.
        IRNode native_root = ir.root;
        synthesize_primitive_paths(native_root);

        int var_counter = 0;
        std::unordered_map<const IRNode*, std::string> id_map;
        generate_native_node(ss, native_root, opts, 0, var_counter, "", &id_map);
        ss << "void 0;\n";

        // Tree-level fidelity pass: catch vector/path nodes codegen dropped to
        // empty frames. No per-node branch fires for them (the generic-frame
        // fall-through has no run_fidelity_checks call site), so this runs once
        // over the IR after the emit walk. Native arm only — the web-compat
        // lowering does not own the dropped-shape fall-through.
        if (opts.fidelity_report) {
            check_vector_renderability(
                native_root, ir.diagnostics,
                [&id_map](const IRNode& n) -> std::string {
                    auto it = id_map.find(&n);
                    if (it != id_map.end()) return it->second;
                    return sanitize_var(n.name.empty() ? n.type : n.name);
                },
                *opts.fidelity_report);
        }
    } else {
        // Web-compat DOM API
        int var_counter = 0;
        generate_node(ss, ir.root, opts, 0, var_counter, "");
        ss << "document.body.appendChild(" << opts.root_variable << ");\n";
    }

    return ss.str();
}

} // namespace pulp::view
