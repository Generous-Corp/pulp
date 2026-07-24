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
    combo_box,
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

// On-screen box (and optional absolute offset) for an imported image node,
// derived from the PNG's natural size, its art-core rect, or its bleed aspect.
// Shared by the runtime materializer and the C++ codegen so both size an
// imported image identically. Returns nullopt when no override applies.
struct ImportedImageSizing {
    float width = 0.0f;
    float height = 0.0f;
    std::optional<float> left;
    std::optional<float> top;
};

std::optional<ImportedImageSizing> imported_image_sizing_override(const IRNode& node);

// Hit-ownership contract shared by the runtime materializer and the C++ codegen.
// A single definition here keeps the two lowerers from drifting on which widget
// kinds are interactive, which own their promoted children's hits, and how a
// promoted child's hit-testing is rewritten.
bool is_interactive_native_kind(NativeWidgetKind kind);
bool native_kind_owns_imported_child_hits(NativeWidgetKind kind);
bool subtree_contains_interactive_hit_target(const IRNode& node,
                                             const ResolvedNativeNode& resolved);

enum class PromotedChildHitPolicy {
    unchanged,
    disabled,
    pass_through_self,
};

PromotedChildHitPolicy promoted_widget_child_hit_policy(const IRNode& child,
                                                        const ResolvedNativeNode& resolved_child);

// Canonical pre-resolution normalization shared by every native lane. Returns
// a copy so callers never mutate the imported IR they were handed.
DesignIR prepare_native_design_ir(const DesignIR& ir);

ResolvedNativeNode resolve_design_ir_native(const DesignIR& ir,
                                            const IRAssetManifest& manifest);

ResolvedNativeNode resolve_design_ir_native_json(std::string_view frozen_design_ir_json,
                                                 const IRAssetManifest& manifest);

// Returns the SVG document text for a faithful_svg node's asset, or empty if it
// can't be resolved. Handles `data:image/svg+xml[;base64],…` payloads and
// on-disk files (local_path or a `file://` original_uri). Shared by the runtime
// materializer (make_faithful_svg_frame) and the C++ codegen, so both lower a
// faithful_svg node from the identical resolved bytes. Host-side / codegen-time
// only — does file I/O; never the audio/render thread.
std::string resolve_svg_document(const IRAssetRef& asset);

} // namespace pulp::view
