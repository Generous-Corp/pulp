#pragma once

#include <pulp/view/design_import.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

enum class NativeWidgetKind {
    view,
    label,
    text_button,
    text_editor,
    checkbox,
    toggle_button,
    knob,
    fader,
    meter,
    xy_pad,
    waveform,
    spectrum,
    image_view,
    canvas,
    svg_path,
    svg_rect,
    svg_line
};

const char* native_widget_kind_name(NativeWidgetKind kind);

struct ResolvedNativeNode {
    NativeWidgetKind kind = NativeWidgetKind::view;
    std::string id;
    std::optional<std::string> text;
    std::vector<ResolvedNativeNode> children;
    std::vector<ImportDiagnostic> diagnostics;
};

enum class ImportedFaderThumbShape {
    rectangle,
    circle
};

struct ImportedWidgetSemantics {
    std::string text;
    std::optional<std::string> text_placeholder;
    std::optional<std::string> text_value;

    bool checked = false;
    bool toggle_on = false;
    std::optional<std::string> toggle_on_background_color;
    std::optional<std::string> toggle_off_background_color;
    std::optional<std::string> toggle_on_text_color;
    std::optional<std::string> toggle_off_text_color;
    std::optional<std::string> toggle_on_border_color;
    std::optional<std::string> toggle_off_border_color;
    std::optional<float> toggle_corner_radius;
    std::optional<float> toggle_font_size;

    float normalized_value = 0.5f;
    float normalized_default = 0.5f;
    float peak_value = 0.5f;
    bool horizontal = false;
    std::optional<std::string> widget_schema;
    bool show_internal_label = true;

    std::optional<ImportedFaderThumbShape> fader_thumb_shape;
    std::optional<float> fader_thumb_width;
    std::optional<float> fader_thumb_height;
    std::optional<float> fader_thumb_corner_radius;

    float x_value = 0.5f;
    float y_value = 0.5f;
    std::optional<std::string> x_label;
    std::optional<std::string> y_label;

    std::optional<std::string> waveform_shape;
};

ImportedWidgetSemantics imported_widget_semantics(const IRNode& node,
                                                  const ResolvedNativeNode& resolved);

ResolvedNativeNode resolve_design_ir_native(const DesignIR& ir,
                                            const IRAssetManifest& manifest);

ResolvedNativeNode resolve_design_ir_native_json(std::string_view frozen_design_ir_json,
                                                 const IRAssetManifest& manifest);

} // namespace pulp::view
