#pragma once

/// @file design_import.hpp
/// Import design files from external tools (Figma, Stitch, v0, Pencil) into
/// Pulp's web-compat JS format. Supports a normalized intermediate
/// representation (IR) and W3C Design Tokens.
///
/// This is a back-compat umbrella header. The declarations are split across
/// focused headers (included below in dependency order):
///   * design_ir.hpp        — IR model + canonical JSON / asset entry points
///   * design_tokens.hpp    — W3C tokens + Figma/Stitch token sync
///   * design_sources.hpp   — source adapters (Figma/Stitch/v0/Pencil/Claude,
///                            DESIGN.md, Claude/React runtime bundles)
///   * design_shortcuts.hpp — keyboard-shortcut import + default shortcuts
///   * design_codegen.hpp   — JS / baked-C++ code generation
///
/// The native-binding materialization surface (build_native_view_tree and the
/// NativeImport* binding descriptors / context) still lives directly in this
/// umbrella; it has not been split out yet.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_tokens.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/design_shortcuts.hpp>
#include <pulp/view/design_codegen.hpp>

#include <memory>
#include <string_view>
#include <vector>

namespace pulp::view {

class View;
class Knob;
class Fader;
class Meter;
class ToggleButton;
class TextButton;
class TextEditor;
class XYPad;
class WaveformView;

struct NativeMaterializeOptions {
    bool apply_token_theme = true;
    bool preview_mode = false;
    std::vector<ImportDiagnostic>* diagnostics_out = nullptr;
};

enum class NativeDesignProviderMode {
    baked_cpp_only,
    provider_with_fallback,
    provider_strict
};

struct NativeDesignProviderProvenance {
    std::string provider_id;
    std::string provider_version;
    std::string source_design_hash;
};

struct NativeDesignProviderRequest {
    const DesignIR* source_ir = nullptr;
    const IRAssetManifest* source_manifest = nullptr;
    std::string canonical_source_design_ir_json;
    std::string source_design_hash;
    bool strict_mode = false;
};

struct NativeDesignProviderOutput {
    bool ok = true;
    std::string canonical_design_ir_json;
    IRAssetManifest asset_manifest;
    std::vector<ImportDiagnostic> diagnostics;
    NativeDesignProviderProvenance provenance;
    std::string failure_reason;
};

class NativeDesignProvider {
public:
    virtual ~NativeDesignProvider() = default;
    virtual std::string provider_id() const = 0;
    virtual std::string provider_version() const { return {}; }
    virtual NativeDesignProviderOutput import_design(const NativeDesignProviderRequest& request) = 0;
};

class NativeDesignProviderRegistry {
public:
    bool register_provider(std::shared_ptr<NativeDesignProvider> provider);
    NativeDesignProvider* find_provider(std::string_view provider_id) const;
    std::vector<std::string> provider_ids() const;

private:
    std::vector<std::shared_ptr<NativeDesignProvider>> providers_;
};

struct NativeDesignProviderValidationOptions {
    std::string expected_provider_id;
    std::string source_design_hash;
    bool require_canonical_json = true;
    bool reject_duplicate_source_anchors = true;
};

struct NativeDesignProviderValidationResult {
    bool ok = false;
    DesignIR design_ir;
    IRAssetManifest asset_manifest;
    std::string canonical_design_ir_json;
    std::vector<ImportDiagnostic> diagnostics;
    NativeDesignProviderProvenance provenance;
};

NativeDesignProviderValidationResult validate_native_design_provider_output(
    const NativeDesignProviderOutput& output,
    const NativeDesignProviderValidationOptions& options = {});

struct NativeDesignProviderAttempt {
    bool provider_attempted = false;
    bool strict_mode = false;
    bool fallback_used = false;
    std::string provider_id;
    std::string provider_version;
    std::string source_design_hash;
    std::string failure_reason;
    std::vector<ImportDiagnostic> diagnostics;
};

struct NativeDesignProviderMaterializeOptions {
    NativeDesignProviderMode mode = NativeDesignProviderMode::baked_cpp_only;
    std::string provider_id;
    const NativeDesignProviderRegistry* registry = nullptr;
    std::string source_design_hash;
    NativeDesignProviderAttempt* attempt_out = nullptr;
    bool reject_duplicate_source_anchors = true;
};

struct NativeImportBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view binding_module;
    std::string_view binding_param;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportXYPadBindingDescriptor {
    std::string_view route_id;
    std::string_view x_param_key;
    std::string_view y_param_key;
    std::string_view x_binding_module;
    std::string_view x_binding_param;
    std::string_view y_binding_module;
    std::string_view y_binding_param;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportMeterBindingDescriptor {
    std::string_view route_id;
    std::string_view meter_source;
    std::string_view channel;
    std::string_view value_key;
    std::string_view event_contract;
};

struct NativeImportChoiceBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view choice_value;
    std::string_view choice_label;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportWaveformBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view shape;
    std::string_view event_contract;
};

struct NativeImportTextBindingDescriptor {
    std::string_view route_id;
    std::string_view value_key;
    std::string_view initial_value;
    std::string_view placeholder;
    std::string_view event_contract;
    std::string_view focus_contract;
};

struct NativeImportHostActionDescriptor {
    std::string_view route_id;
    std::string_view action;
    std::string_view label;
    std::string_view payload_contract;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

class NativeImportBindingContext {
public:
    virtual ~NativeImportBindingContext() = default;
    virtual void bind_knob(Knob& knob, const NativeImportBindingDescriptor& descriptor) {
        (void)knob;
        (void)descriptor;
    }
    virtual void bind_fader(Fader& fader, const NativeImportBindingDescriptor& descriptor) {
        (void)fader;
        (void)descriptor;
    }
    virtual void bind_meter(Meter& meter, const NativeImportMeterBindingDescriptor& descriptor) {
        (void)meter;
        (void)descriptor;
    }
    virtual void bind_toggle_button(ToggleButton& button, const NativeImportBindingDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
    virtual void bind_choice_button(ToggleButton& button, const NativeImportChoiceBindingDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
    virtual void bind_xy_pad(XYPad& pad, const NativeImportXYPadBindingDescriptor& descriptor) {
        (void)pad;
        (void)descriptor;
    }
    virtual void bind_waveform_display(WaveformView& waveform,
                                       const NativeImportWaveformBindingDescriptor& descriptor) {
        (void)waveform;
        (void)descriptor;
    }
    virtual void bind_text_editor(TextEditor& editor, const NativeImportTextBindingDescriptor& descriptor) {
        (void)editor;
        (void)descriptor;
    }
    virtual void bind_host_action(TextButton& button, const NativeImportHostActionDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
};

std::unique_ptr<View> build_native_view_tree(
    const DesignIR& ir,
    const IRAssetManifest& manifest,
    const NativeMaterializeOptions& options = {});

std::unique_ptr<View> build_native_view_tree_with_provider(
    const DesignIR& ir,
    const IRAssetManifest& manifest,
    const NativeMaterializeOptions& materialize_options,
    const NativeDesignProviderMaterializeOptions& provider_options);

/// Source-agnostic IR normalization for vector SHAPE PRIMITIVES. Walks the tree
/// and, for each rect/rectangle/line/ellipse/circle/polygon/star node that
/// would otherwise be dropped to an empty frame — no `path_data`, no children,
/// no visible fill, no rasterized asset, not an audio widget — synthesizes an
/// SVG path `d` (plus `svg_viewbox` and, when the node carries a border, an
/// `svg_stroke`/`svg_stroke_width`; `svg_fill` is forced to "none" so the
/// SvgPathWidget's default opaque-black fill never paints a phantom box) from
/// the node's geometry. Codegen then lowers it to a native SvgPath via
/// createSvgPath+setSvgPath instead of silently dropping the shape. Idempotent:
/// nodes that already carry `path_data`, render some other way, or are not
/// synthesizable primitives are left untouched. Geometry is derived from IR
/// fields only (width/height, corner radii, and optional pointCount/innerRadius
/// attributes) — never a layer name or source quirk.
void synthesize_primitive_paths(IRNode& root);

} // namespace pulp::view
