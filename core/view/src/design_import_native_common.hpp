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

ResolvedNativeNode resolve_design_ir_native(const DesignIR& ir,
                                            const IRAssetManifest& manifest);

ResolvedNativeNode resolve_design_ir_native_json(std::string_view frozen_design_ir_json,
                                                 const IRAssetManifest& manifest);

} // namespace pulp::view
