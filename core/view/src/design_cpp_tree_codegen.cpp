#include "design_cpp_codegen_internal.hpp"

#include <pulp/runtime/base64.hpp>

#include "design_ir_helpers.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view::cpp_codegen {
namespace {

bool body_is_painted_beneath(const IRNode& node) {
    const auto it = node.attributes.find("designed_body");
    return it != node.attributes.end() &&
           (it->second == "underlay" || it->second == "capture");
}

std::string widget_make_expr(const IRNode& node,
                             const ResolvedNativeNode& resolved,
                             const IRAssetManifest& manifest) {
    const auto text = resolved.text.value_or(node.text_content);
    switch (resolved.kind) {
        case NativeWidgetKind::label:
            return "std::make_unique<pulp::view::Label>(" + cpp_string_literal(text) + ")";
        case NativeWidgetKind::text_button:
            return "std::make_unique<pulp::view::TextButton>(" + cpp_string_literal(text) + ")";
        case NativeWidgetKind::text_editor:
            return "std::make_unique<pulp::view::TextEditor>()";
        case NativeWidgetKind::checkbox:
            return "std::make_unique<pulp::view::Checkbox>()";
        case NativeWidgetKind::toggle_button:
            return "std::make_unique<pulp::view::ToggleButton>()";
        case NativeWidgetKind::segmented:
            return "std::make_unique<pulp::view::SegmentedControl>()";
        case NativeWidgetKind::stepper:
            return "std::make_unique<pulp::view::Stepper>()";
        case NativeWidgetKind::knob:
            return "std::make_unique<pulp::view::Knob>()";
        case NativeWidgetKind::fader:
            return "std::make_unique<pulp::view::Fader>()";
        case NativeWidgetKind::meter:
            return "std::make_unique<pulp::view::Meter>()";
        case NativeWidgetKind::xy_pad:
            return "std::make_unique<pulp::view::XYPad>()";
        case NativeWidgetKind::waveform:
            return "std::make_unique<pulp::view::WaveformView>()";
        case NativeWidgetKind::spectrum:
            return "std::make_unique<pulp::view::SpectrumView>()";
        case NativeWidgetKind::image_view:
            return "std::make_unique<pulp::view::ImageView>()";
        case NativeWidgetKind::canvas:
            return "std::make_unique<pulp::view::CanvasWidget>()";
        case NativeWidgetKind::svg_path:
            return "std::make_unique<pulp::view::SvgPathWidget>()";
        case NativeWidgetKind::svg_rect:
            return "std::make_unique<pulp::view::SvgRectWidget>()";
        case NativeWidgetKind::svg_line:
            return "std::make_unique<pulp::view::SvgLineWidget>()";
        case NativeWidgetKind::view:
            return "std::make_unique<pulp::view::View>()";
    }
    (void)manifest;
    return "std::make_unique<pulp::view::View>()";
}

std::string span_color_expr(const EmitContext& ctx, canvas::Color color) {
    // This serializes an authored run color from DesignIR; it is not a Pulp
    // theme literal.  Generated code must preserve the exact authored RGBA.
    return "pulp::canvas::Color::rgba(" +  // token-lint:allow
           float_expr(ctx, color.r) + ", " +
           float_expr(ctx, color.g) + ", " + float_expr(ctx, color.b) + ", " +
           float_expr(ctx, color.a) + ")";
}

void emit_attributed_text(std::ostringstream& out,
                          int depth,
                          const EmitContext& ctx,
                          std::string_view var,
                          const IRNode& node) {
    if (node.text_runs.empty()) return;
    auto attributed = attributed_text_for_node(node);
    if (attributed.empty()) return;

    const std::string attributed_var = std::string(var) + "_attributed";
    emit_line(out, depth, ctx.opts.indent_spaces,
              "pulp::canvas::AttributedString " + attributed_var + ";");
    for (const auto& span : attributed.spans()) {
        emit_line(out, depth, ctx.opts.indent_spaces, "{");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "pulp::canvas::TextSpan span;");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.text = " + cpp_string_literal(span.text) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.font_family = " + cpp_string_literal(span.font_family) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.font_size = " + float_expr(ctx, span.font_size) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.font_weight = " + std::to_string(span.font_weight) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  std::string("span.italic = ") + (span.italic ? "true;" : "false;"));
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.font_slant = " + std::to_string(span.font_slant) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.color = " + span_color_expr(ctx, span.color) + ";");
        const char* decoration = "none";
        if (span.decoration == canvas::TextDecoration::underline)
            decoration = "underline";
        else if (span.decoration == canvas::TextDecoration::strikethrough)
            decoration = "strikethrough";
        else if (span.decoration == canvas::TextDecoration::overline)
            decoration = "overline";
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.decoration = pulp::canvas::TextDecoration::" +
                      std::string(decoration) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  std::string("span.decoration_override = ") +
                      (span.decoration_override ? "true;" : "false;"));
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "span.letter_spacing = " +
                      float_expr(ctx, span.letter_spacing) + ";");
        const auto emit_inherit = [&](std::string_view field, bool value) {
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "span." + std::string(field) + " = " +
                          (value ? "true;" : "false;"));
        };
        emit_inherit("inherit_font_family", span.inherit_font_family);
        emit_inherit("inherit_font_size", span.inherit_font_size);
        emit_inherit("inherit_font_weight", span.inherit_font_weight);
        emit_inherit("inherit_font_slant", span.inherit_font_slant);
        emit_inherit("inherit_color", span.inherit_color);
        emit_inherit("inherit_letter_spacing", span.inherit_letter_spacing);
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  attributed_var + ".append(std::move(span));");
        emit_line(out, depth, ctx.opts.indent_spaces, "}");
    }
    emit_line(out, depth, ctx.opts.indent_spaces,
              std::string(var) + "->set_attributed_string(std::move(" +
                  attributed_var + "));");
}

void emit_captured_text_layout(std::ostringstream& out,
                               int depth,
                               const EmitContext& ctx,
                               std::string_view var,
                               const IRNode& node) {
    if (node.text_line_boxes.empty() || !node.text_layout_basis) return;
    std::string boxes = "{";
    for (std::size_t i = 0; i < node.text_line_boxes.size(); ++i) {
        const auto& box = node.text_line_boxes[i];
        if (i) boxes += ", ";
        boxes += "{" + float_expr(ctx, box.left) + ", " +
                 float_expr(ctx, box.top) + ", " +
                 float_expr(ctx, box.width) + ", " +
                 float_expr(ctx, box.height) + ", " +
                 std::to_string(box.start) + ", " +
                 std::to_string(box.length) + "}";
    }
    boxes += "}";
    const bool explicit_nowrap =
        node.style.white_space && *node.style.white_space == "nowrap";
    const bool wrap_on_cache_miss =
        !explicit_nowrap && node.text_line_boxes.size() == 1;
    emit_line(out, depth, ctx.opts.indent_spaces,
              std::string(var) + "->set_cached_line_boxes(" + boxes + ", " +
                  float_expr(ctx, node.text_layout_basis->width) + ", " +
                  cpp_string_literal(node.text_layout_basis->resolved_face) +
                  ", " + (wrap_on_cache_miss ? "true" : "false") + ");");
    if (!explicit_nowrap && node.text_line_boxes.size() > 1)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  std::string(var) + "->set_multi_line(true);");
}

void emit_widget_specific(std::ostringstream& out,
                          int depth,
                          const EmitContext& ctx,
                          std::string_view var,
                          const IRNode& node,
                          const ResolvedNativeNode& resolved,
                          const IRAssetManifest& manifest) {
    const auto& opts = ctx.opts;
    const auto semantics = imported_widget_semantics(node, resolved);
    const auto& text = semantics.text;
    switch (resolved.kind) {
        case NativeWidgetKind::label:
            emit_label_style(out, depth, ctx, var, node.style);
            emit_attributed_text(out, depth, ctx, var, node);
            emit_captured_text_layout(out, depth, ctx, var, node);
            break;
        case NativeWidgetKind::text_editor:
            if (semantics.text_placeholder)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->placeholder = " + cpp_string_literal(*semantics.text_placeholder) + ";");
            if (semantics.text_value)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_text(" + cpp_string_literal(*semantics.text_value) + ");");
            break;
        case NativeWidgetKind::checkbox:
            if (semantics.checked)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_checked(true);");
            break;
        case NativeWidgetKind::toggle_button:
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            if (semantics.toggle_on)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on(true);");
            if (semantics.toggle_on_background_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_background_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_background_color(" + expr + ");");
            }
            if (semantics.toggle_off_background_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_background_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_background_color(" + expr + ");");
            }
            if (semantics.toggle_on_text_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_text_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_text_color(" + expr + ");");
            }
            if (semantics.toggle_off_text_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_text_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_text_color(" + expr + ");");
            }
            if (semantics.toggle_on_border_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_on_border_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_on_border_color(" + expr + ");");
            }
            if (semantics.toggle_off_border_color) {
                if (auto expr = color_expr(ctx, *semantics.toggle_off_border_color); !expr.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_off_border_color(" + expr + ");");
            }
            if (semantics.toggle_corner_radius)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_corner_radius(" + float_expr(ctx, *semantics.toggle_corner_radius) + ");");
            if (semantics.toggle_font_size)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_font_size(" + float_expr(ctx, *semantics.toggle_font_size) + ");");
            if (body_is_painted_beneath(node))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_designed_overlay(true);");
            break;
        case NativeWidgetKind::segmented: {
            std::string labels = "{";
            for (std::size_t i = 0; i < semantics.segments.size(); ++i) {
                if (i != 0) labels += ", ";
                labels += cpp_string_literal(semantics.segments[i]);
            }
            labels += "}";
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_segments(" + labels + ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_selected_silent(" +
                          std::to_string(selector_segment_index(
                              semantics.normalized_value,
                              static_cast<int>(semantics.segments.size()))) +
                          ");");
            if (body_is_painted_beneath(node))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_designed_overlay(true);");
            break;
        }
        case NativeWidgetKind::stepper:
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_range(" +
                          float_expr(ctx, node.audio_min) + ", " +
                          float_expr(ctx, node.audio_max) + ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_step(" +
                          float_expr(ctx, static_cast<float>(semantics.stepper_step)) +
                          ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_value(" +
                          float_expr(ctx, static_cast<float>(stepper_plain_value(
                              semantics.normalized_value, node.audio_min,
                              node.audio_max, semantics.stepper_step))) +
                          ");");
            if (body_is_painted_beneath(node))
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_designed_overlay(true);");
            break;
        case NativeWidgetKind::knob: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            const auto value = float_expr(ctx, semantics.normalized_value);
            const auto default_value = float_expr(ctx, semantics.normalized_default);
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_value(/* imported static param value */ " + value + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_default_value(" + default_value + ");");
            if (semantics.widget_schema)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*semantics.widget_schema) + ");");
            if (!semantics.show_internal_label)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_show_label(false);");
            break;
        }
        case NativeWidgetKind::fader: {
            if (!text.empty())
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_label(" + cpp_string_literal(text) + ");");
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_value(/* imported static param value */ " +
                          float_expr(ctx, semantics.normalized_value) + ");");
            if (semantics.horizontal)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Fader::Orientation::horizontal);");
            if (semantics.widget_schema)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_widget_schema(" + cpp_string_literal(*semantics.widget_schema) + ");");
            if (semantics.fader_thumb_shape) {
                if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::rectangle)
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_thumb_shape(pulp::view::Fader::ThumbShape::rectangle);");
                else if (*semantics.fader_thumb_shape == ImportedFaderThumbShape::circle)
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_thumb_shape(pulp::view::Fader::ThumbShape::circle);");
            }
            if (semantics.fader_thumb_width || semantics.fader_thumb_height) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_size(" +
                              float_expr(ctx, semantics.fader_thumb_width.value_or(0.0f)) + ", " +
                              float_expr(ctx, semantics.fader_thumb_height.value_or(0.0f)) + ");");
            }
            if (semantics.fader_thumb_corner_radius) {
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_thumb_corner_radius(" + float_expr(ctx, *semantics.fader_thumb_corner_radius) + ");");
            }
            break;
        }
        case NativeWidgetKind::meter: {
            const auto value = float_expr(ctx, semantics.normalized_value);
            const auto peak = float_expr(ctx, semantics.peak_value);
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_level(/* imported static meter level */ " + value + ", " + peak + ");");
            if (semantics.horizontal)
                emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_orientation(pulp::view::Meter::Orientation::horizontal);");
            break;
        }
        case NativeWidgetKind::xy_pad:
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x(" + float_expr(ctx, semantics.x_value) + ");");
            emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y(" + float_expr(ctx, semantics.y_value) + ");");
            if (semantics.x_label) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_x_label(" + cpp_string_literal(*semantics.x_label) + ");");
            if (semantics.y_label) emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_y_label(" + cpp_string_literal(*semantics.y_label) + ");");
            break;
        case NativeWidgetKind::waveform:
            if (semantics.waveform_shape)
                emit_line(out, depth, opts.indent_spaces,
                          std::string(var) + "->set_preview_shape(" + cpp_string_literal(*semantics.waveform_shape) + ");");
            break;
        case NativeWidgetKind::image_view:
            if (auto asset_id = first_asset_id(node)) {
                const auto expression = asset_uri_expression(manifest, *asset_id);
                if (!expression.empty())
                    emit_line(out, depth, opts.indent_spaces, std::string(var) + "->set_image_source(" + expression + ");");
            }
            if (auto sizing = imported_image_sizing_override(node)) {
                const auto flex_var = std::string(var) + "_image_flex";
                emit_line(out, depth, opts.indent_spaces,
                          "auto& " + flex_var + " = " + std::string(var) + "->flex();");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".preferred_width = " + float_expr(ctx, sizing->width) + ";");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".preferred_height = " + float_expr(ctx, sizing->height) + ";");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".dim_width = {" + float_expr(ctx, sizing->width) + ", pulp::view::DimensionUnit::px};");
                emit_line(out, depth, opts.indent_spaces,
                          flex_var + ".dim_height = {" + float_expr(ctx, sizing->height) + ", pulp::view::DimensionUnit::px};");
                if (sizing->left)
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_left(" + float_expr(ctx, *sizing->left) + ");");
                if (sizing->top)
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_top(" + float_expr(ctx, *sizing->top) + ");");
            }
            break;
        case NativeWidgetKind::svg_path:
            for (const char* key : {"path_data", "d"}) {
                if (auto path_data = attr(node, key)) {
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_path(" +
                                  cpp_string_literal(*path_data) + ");");
                    break;
                }
            }
            {
                bool emitted_viewbox = false;
                for (const char* key : {"svg_viewbox", "viewBox"}) {
                    if (auto viewbox = attr(node, key)) {
                        std::istringstream input(*viewbox);
                        float min_x = 0.0f, min_y = 0.0f, width = 0.0f, height = 0.0f;
                        if (input >> min_x >> min_y >> width >> height) {
                            emit_line(out, depth, opts.indent_spaces,
                                      std::string(var) + "->set_viewbox(" +
                                          float_expr(ctx, width) + ", " +
                                          float_expr(ctx, height) + ");");
                            emitted_viewbox = true;
                        }
                        break;
                    }
                }
                if (!emitted_viewbox && node.style.width && node.style.height) {
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) + "->set_viewbox(" +
                                  float_expr(ctx, *node.style.width) + ", " +
                                  float_expr(ctx, *node.style.height) + ");");
                }
                // Emitted only for `none`; `xMidYMid meet` is the widget
                // default and re-stating it would put a line in every exported
                // panel that says nothing.
                if (auto aspect = attr(node, "svg_preserve_aspect_ratio");
                    aspect && *aspect == "none") {
                    emit_line(out, depth, opts.indent_spaces,
                              std::string(var) +
                                  "->set_stretch_to_bounds(true);");
                }
            }
            // Path-only (SvgRect/SvgLine have no fill rule): the winding rule
            // decides which regions of a multi-subpath path are holes, and the
            // nonzero default fills a subtracted icon solid. Emitted only for
            // evenodd — nonzero is already the widget default.
            for (const char* key : {"svg_fill_rule", "fill-rule", "fillRule"}) {
                if (auto rule = attr(node, key)) {
                    if (*rule == "evenodd")
                        emit_line(out, depth, opts.indent_spaces,
                                  std::string(var) + "->set_fill_rule(pulp::canvas::FillRule::evenodd);");
                    break;
                }
            }
            emit_svg_paint(out, depth, ctx, var, node, true);
            break;
        case NativeWidgetKind::svg_rect:
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_rect(" +
                      float_expr(ctx, attr_float(node, "x").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "width").value_or(node.style.width.value_or(0.0f))) + ", " +
                      float_expr(ctx, attr_float(node, "height").value_or(node.style.height.value_or(0.0f))) + ");");
            emit_svg_paint(out, depth, ctx, var, node, true);
            break;
        case NativeWidgetKind::svg_line:
            emit_line(out, depth, opts.indent_spaces,
                      std::string(var) + "->set_line(" +
                      float_expr(ctx, attr_float(node, "x1").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y1").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "x2").value_or(0.0f)) + ", " +
                      float_expr(ctx, attr_float(node, "y2").value_or(0.0f)) + ");");
            emit_svg_paint(out, depth, ctx, var, node, false);
            break;
        default:
            break;
    }
}

// IR interactive-element kind → the generated DesignFrameElement::Kind token.
// Mirrors to_frame_elements() in design_import_native_common.cpp so the codegen
// and the runtime materializer lower the same overlay set.
const char* frame_element_kind_token(InteractiveElementKind kind) {
    switch (kind) {
        case InteractiveElementKind::knob:        return "knob";
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
    }
    return "knob";
}

// Emit a faithful_svg node as a DesignFrameView that renders the node's own
// Figma SVG export 1:1 (SkSVGDOM) with the typed interactive overlays composited
// on top — the same construction make_faithful_svg_frame() builds at runtime,
// but lowered to static C++. The SVG is embedded as chunked base64 (no >64K
// string literal) and decoded once at construction, matching the catalog
// generator (tools/import-design/make_catalog_component.py).
//
// Resolve a faithful node's SVG document, or an empty string when it has no
// asset id or the asset can't be resolved at codegen time.
std::string resolve_frame_svg(EmitContext& ctx, const IRNode& node) {
    if (!node.svg_asset_id) return {};
    const IRAssetRef* asset = ctx.manifest.resolve(*node.svg_asset_id);
    return asset ? resolve_svg_document(*asset) : std::string{};
}

// Emit `std::string <name>;` holding one frame's SVG, as chunked base64 joined
// and decoded once. Each call scopes its own kParts, so a multi-frame node can
// call this per frame without colliding.
void emit_frame_svg(std::ostringstream& out,
                    int depth,
                    EmitContext& ctx,
                    const std::string& name,
                    const std::string& svg) {
    const std::string b64 = runtime::base64_encode(svg);
    emit_line(out, depth, ctx.opts.indent_spaces, "std::string " + name + ";");
    emit_line(out, depth, ctx.opts.indent_spaces, "{");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "static const char* const kParts[] = {");
    constexpr std::size_t kChunk = 8000;
    for (std::size_t i = 0; i < b64.size(); i += kChunk) {
        const std::string piece = b64.substr(i, kChunk);
        const bool last = (i + kChunk >= b64.size());
        emit_line(out, depth + 2, ctx.opts.indent_spaces,
                  cpp_string_literal(piece) + (last ? "" : ","));
    }
    emit_line(out, depth + 1, ctx.opts.indent_spaces, "};");
    emit_line(out, depth + 1, ctx.opts.indent_spaces, "std::string b64;");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "for (const char* p : kParts) b64 += p;");
    emit_line(out, depth + 1, ctx.opts.indent_spaces,
              "if (auto bytes = pulp::runtime::base64_decode(b64))");
    emit_line(out, depth + 2, ctx.opts.indent_spaces,
              name + ".assign(bytes->begin(), bytes->end());");
    emit_line(out, depth, ctx.opts.indent_spaces, "}");
}

// Emit `std::vector<DesignFrameElement> <name>;` holding one frame's interactive
// overlays (knob / dropdown / text_field / tab_group / stepper / swap / ...).
void emit_frame_elements(std::ostringstream& out,
                         int depth,
                         EmitContext& ctx,
                         const std::string& name,
                         const IRNode& node) {
    emit_line(out, depth, ctx.opts.indent_spaces,
              "std::vector<pulp::view::DesignFrameElement> " + name + ";");
    for (const auto& e : node.interactive_elements) {
        emit_line(out, depth, ctx.opts.indent_spaces, "{");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "pulp::view::DesignFrameElement el;");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  std::string("el.kind = pulp::view::DesignFrameElement::Kind::") +
                  frame_element_kind_token(e.kind) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.cx = " + format_float(e.cx) + "; el.cy = " + format_float(e.cy) +
                  "; el.hit_radius = " + format_float(e.hit_radius) + ";");
        if (!e.svg_patch_d.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.needle_d = " + cpp_string_literal(e.svg_patch_d) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.value = " + format_float(e.default_value) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  "el.x = " + format_float(e.x) + "; el.y = " + format_float(e.y) +
                  "; el.w = " + format_float(e.w) + "; el.h = " + format_float(e.h) + ";");
        if (!e.placeholder.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.placeholder = " + cpp_string_literal(e.placeholder) + ";");
        if (!e.options.empty()) {
            std::string opts = "el.options = {";
            for (std::size_t i = 0; i < e.options.size(); ++i)
                opts += (i ? ", " : " ") + cpp_string_literal(e.options[i]);
            opts += " };";
            emit_line(out, depth + 1, ctx.opts.indent_spaces, opts);
        }
        if (e.selected_index != 0)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.selected_index = " + std::to_string(e.selected_index) + ";");
        if (!e.bg_color.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.bg_color = " + cpp_string_literal(e.bg_color) + ";");
        if (e.flash)
            emit_line(out, depth + 1, ctx.opts.indent_spaces, "el.flash = true;");
        // swap / action / xy_pad / value_label fields.
        if (e.target_frame != -1)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.target_frame = " + std::to_string(e.target_frame) + ";");
        if (!e.action.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.action = " + cpp_string_literal(e.action) + ";");
        if (!e.text.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.text = " + cpp_string_literal(e.text) + ";");
        if (e.value_left_align)
            emit_line(out, depth + 1, ctx.opts.indent_spaces, "el.value_left_align = true;");
        if (e.kind == InteractiveElementKind::xy_pad)
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.value_y = " + format_float(e.default_value_y) + ";");
        if (!e.factory_id.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.factory_id = " + cpp_string_literal(e.factory_id) + ";");
        if (!e.custom_props.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.custom_props = " + cpp_string_literal(e.custom_props) + ";");
        // Host-param binding key for a geometry-detected control.
        if (!e.param_key.empty())
            emit_line(out, depth + 1, ctx.opts.indent_spaces,
                      "el.param_key = " + cpp_string_literal(e.param_key) + ";");
        emit_line(out, depth + 1, ctx.opts.indent_spaces,
                  name + ".push_back(std::move(el));");
        emit_line(out, depth, ctx.opts.indent_spaces, "}");
    }
}

// True when any control on `node` or any of its alternate frames carries a
// host-param binding key.
bool frame_set_has_bound_control(const IRNode& node) {
    auto bound = [](const IRNode& n) {
        return std::any_of(n.interactive_elements.begin(), n.interactive_elements.end(),
                           [](const IRInteractiveElement& e) { return !e.param_key.empty(); });
    };
    if (bound(node)) return true;
    return std::any_of(node.alternate_frames.begin(), node.alternate_frames.end(), bound);
}

// Returns true when it emitted the faithful construction (assigning `var`).
// Returns false when the node is not faithful, has no svg_asset_id, or the asset
// can't be resolved at codegen time — the caller then falls back to the normal
// native-widget emit so the output always compiles and renders something.
bool emit_faithful_frame(std::ostringstream& out,
                         int depth,
                         EmitContext& ctx,
                         const std::string& var,
                         const IRNode& node) {
    if (node.render_mode != NodeRenderMode::faithful_svg || !node.svg_asset_id)
        return false;
    const std::string svg = resolve_frame_svg(ctx, node);
    if (svg.empty()) {
        if (ctx.opts.include_comments)
            emit_line(out, depth, ctx.opts.indent_spaces,
                      "// faithful_svg asset '" + *node.svg_asset_id +
                      "' unresolved at codegen time — falling back to native widgets");
        return false;
    }

    if (ctx.opts.include_comments)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  "// faithful_svg: render this node's own Figma SVG 1:1 via DesignFrameView");

    // Frame 0 = this node: the constructor's SVG + overlays.
    emit_frame_svg(out, depth, ctx, var + "_svg", svg);
    emit_frame_elements(out, depth, ctx, var + "_els", node);
    emit_line(out, depth, ctx.opts.indent_spaces,
              "auto " + var + " = std::make_unique<pulp::view::DesignFrameView>(std::move(" +
              var + "_svg), std::move(" + var + "_els));");

    // Frames 1..N = the alternate states, in capture order. A `swap` element
    // addresses frames POSITIONALLY, so every alternate must produce exactly one
    // add_frame call, in order: dropping or reordering one would silently
    // re-point every later swap target. An alternate whose SVG failed to resolve
    // is therefore still added (blank, but with its overlays and its index
    // intact) rather than skipped — apply_swap_target_verification and the
    // unresolved-asset diagnostics are what report the problem.
    for (std::size_t i = 0; i < node.alternate_frames.size(); ++i) {
        const IRNode& frame = node.alternate_frames[i];
        const std::string suffix = "_f" + std::to_string(i + 1);
        const std::string frame_svg = resolve_frame_svg(ctx, frame);
        if (frame_svg.empty() && ctx.opts.include_comments)
            emit_line(out, depth, ctx.opts.indent_spaces,
                      "// frame " + std::to_string(i + 1) +
                      " SVG unresolved at codegen time — added blank to keep swap "
                      "target indices stable");
        else if (ctx.opts.include_comments)
            emit_line(out, depth, ctx.opts.indent_spaces,
                      "// frame " + std::to_string(i + 1) + ": " +
                      (frame.name.empty() ? std::string("alternate state") : frame.name));
        emit_frame_svg(out, depth, ctx, var + "_svg" + suffix, frame_svg);
        emit_frame_elements(out, depth, ctx, var + "_els" + suffix, frame);
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->add_frame(std::move(" + var + "_svg" + suffix +
                  "), std::move(" + var + "_els" + suffix + "));");
    }

    // If any control carries a binding key, self-wire gestures to the host-param
    // surface — parity with the runtime materialize path (make_faithful_svg_frame).
    if (frame_set_has_bound_control(node))
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->route_changes_to_host_params(true);");
    return true;
}

void emit_node(std::ostringstream& out,
               EmitContext& ctx,
               const IRNode& node,
               const ResolvedNativeNode& resolved,
               std::string_view parent_var,
               int depth,
               std::optional<LayoutDirection> parent_direction,
               int& counter,
               PromotedChildHitPolicy self_hit_policy = PromotedChildHitPolicy::unchanged) {
    emit_line(out, depth, ctx.opts.indent_spaces, "{");
    ++depth;

    const std::string var = "node_" + std::to_string(counter++);
    if (ctx.opts.include_comments) {
        if (node.stable_anchor_id && !node.stable_anchor_id->empty()) {
            std::string comment = "// anchor: " + *node.stable_anchor_id;
            if (!node.name.empty()) comment += " - " + node.name;
            emit_line(out, depth, ctx.opts.indent_spaces, comment);
        } else if (!node.name.empty()) {
            emit_line(out, depth, ctx.opts.indent_spaces, "// " + node.name);
        }
    }
    // A faithful_svg node renders its own SVG via DesignFrameView; the SVG is the
    // whole subtree, so we skip the native widget body and child recursion below.
    const bool faithful = emit_faithful_frame(out, depth, ctx, var, node);
    if (!faithful)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  "auto " + var + " = " + widget_make_expr(node, resolved, ctx.manifest) + ";");
    emit_line(out, depth, ctx.opts.indent_spaces,
              var + "->set_id(" + cpp_string_literal(resolved.id) + ");");
    if (node.stable_anchor_id && !node.stable_anchor_id->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_anchor_id(" + cpp_string_literal(*node.stable_anchor_id) + ");");
    const bool explicit_hit_test_disabled =
        attr(node, "pulpHitTestable") && !attr_bool(node, "pulpHitTestable");
    if (self_hit_policy == PromotedChildHitPolicy::disabled || explicit_hit_test_disabled)
        emit_line(out, depth, ctx.opts.indent_spaces, var + "->set_hit_testable(false);");
    if (self_hit_policy == PromotedChildHitPolicy::pass_through_self)
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_pointer_events(pulp::view::View::PointerEvents::box_none);");
    if (auto label = resolved.text; label && !label->empty())
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_access_label(" + cpp_string_literal(*label) + ");");

    emit_common_layout(out, depth, ctx, var, node, parent_direction);
    // Faithful frames carry their own visuals (the SVG) and interaction (the
    // overlays), so the native style/widget/child emit is skipped entirely.
    if (!faithful) {
        emit_visual_style(out, depth, ctx, var, node.style);
        emit_widget_specific(out, depth, ctx, var, node, resolved, ctx.manifest);
    }

    const auto count = faithful ? std::size_t{0}
                                : std::min(node.children.size(), resolved.children.size());
    const bool parent_owns_imported_child_hits =
        native_kind_owns_imported_child_hits(resolved.kind);
    // A promoted widget that keeps a reachable child must be re-opened first.
    // TextButton defaults to PointerEvents::box_only so a centred icon cannot
    // swallow its own click, and hit_test() then never descends — which would
    // make the box_none emitted below inert and silently drop the interactive
    // descendant. Emitted once here so it covers both the extracted-factory
    // path and the inline emit_node path.
    //
    // Skipped when this node is itself `pass_through_self`: its own box_none was
    // emitted above, and re-opening here would clobber it — the parent's policy
    // for this node outranks this node's policy for its children, which is the
    // order the runtime materializer already uses.
    if (parent_owns_imported_child_hits &&
        self_hit_policy != PromotedChildHitPolicy::pass_through_self) {
        for (std::size_t i = 0; i < count; ++i) {
            if (promoted_widget_child_hit_policy(node.children[i], resolved.children[i]) ==
                PromotedChildHitPolicy::disabled)
                continue;
            emit_line(out, depth, ctx.opts.indent_spaces,
                      var + "->set_pointer_events(pulp::view::View::PointerEvents::auto_);");
            break;
        }
    }

    for (std::size_t i = 0; i < count; ++i) {
        const auto& child = node.children[i];
        const auto child_hit_policy = parent_owns_imported_child_hits
            ? promoted_widget_child_hit_policy(child, resolved.children[i])
            : PromotedChildHitPolicy::unchanged;
        auto found = ctx.extracted.find(&child);
        if (found != ctx.extracted.end()) {
            if (child_hit_policy != PromotedChildHitPolicy::unchanged) {
                const std::string child_var = "child_" + std::to_string(counter++);
                emit_line(out, depth, ctx.opts.indent_spaces, "{");
                emit_line(out, depth + 1, ctx.opts.indent_spaces,
                          "auto " + child_var + " = " + found->second + "(asset_base_directory);");
                if (child_hit_policy == PromotedChildHitPolicy::disabled) {
                    emit_line(out, depth + 1, ctx.opts.indent_spaces,
                              child_var + "->set_hit_testable(false);");
                } else if (child_hit_policy == PromotedChildHitPolicy::pass_through_self) {
                    emit_line(out, depth + 1, ctx.opts.indent_spaces,
                              child_var + "->set_pointer_events(pulp::view::View::PointerEvents::box_none);");
                }
                emit_line(out, depth + 1, ctx.opts.indent_spaces,
                          var + "->add_child(std::move(" + child_var + "));");
                emit_line(out, depth, ctx.opts.indent_spaces, "}");
            } else {
                emit_line(out, depth, ctx.opts.indent_spaces,
                          var + "->add_child(" + found->second + "(asset_base_directory));");
            }
        } else {
            emit_node(out,
                      ctx,
                      child,
                      resolved.children[i],
                      var,
                      depth,
                      node.layout.direction,
                      counter,
                      child_hit_policy);
        }
    }

    if (!parent_var.empty()) {
        emit_line(out, depth, ctx.opts.indent_spaces,
                  std::string(parent_var) + "->add_child(std::move(" + var + "));");
    } else {
        // Imported geometry intentionally preserves fractional coordinates.
        // Every emitted factory returns a layout root, including extracted
        // component factories, so opt each tree out of Yoga's pixel rounding.
        emit_line(out, depth, ctx.opts.indent_spaces,
                  var + "->set_subpixel_layout(true);");
        emit_line(out, depth, ctx.opts.indent_spaces, "return " + var + ";");
    }

    --depth;
    emit_line(out, depth, ctx.opts.indent_spaces, "}");
}


}  // namespace

void emit_function(std::ostringstream& out,
                   EmitContext& ctx,
                   std::string_view function_name,
                   const IRNode& node,
                   const ResolvedNativeNode& resolved,
                   std::optional<LayoutDirection> parent_direction,
                   std::string_view comment) {
    if (ctx.opts.include_comments && !comment.empty())
        out << "// " << comment << "\n";
    out << "std::unique_ptr<pulp::view::View> " << function_name
        << "(const std::filesystem::path& asset_base_directory) {\n";
    int counter = 0;
    emit_node(out, ctx, node, resolved, "", 1, parent_direction, counter);
    out << "}\n\n";
}


}  // namespace pulp::view::cpp_codegen
