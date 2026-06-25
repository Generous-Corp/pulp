#pragma once

/// @file design_codegen.hpp
/// Code generation from a DesignIR: native-bridge / web-compat JS emission,
/// baked-C++ view artifact export, and the shortcut string→KeyCode/mask
/// helpers shared by the generator and runtime callers.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_shortcuts.hpp>  // DetectedShortcut (CodeGenOptions::shortcuts)
#include <pulp/view/design_fidelity.hpp>   // FidelityIssue + run_fidelity_checks (sink)
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

// ── Code generator ──────────────────────────────────────────────────────

/// Code generation output mode.
enum class CodeGenMode {
    web_compat,        // document.createElement + el.style (web-compat layer)
    bridge_native_js,  // JS that calls createCol/createRow/createKnob + setFlex
                       // through the native widget bridge; not direct C++.
    native
        [[deprecated("Use CodeGenMode::bridge_native_js; CodeGenMode::native emits bridge JS, not direct C++.")]]
        = bridge_native_js
};

/// Options for code generation.
struct CodeGenOptions {
    CodeGenMode mode = CodeGenMode::bridge_native_js;  // Native-bridge JS by default (better Yoga compat)
    bool include_tokens = true;       // Generate token assignments
    bool include_comments = true;     // Generate inline comments
    bool preview_mode = false;        // Use minimal widget style (design preview)
    std::string root_variable = "root";
    int indent_spaces = 2;

    /// When true, audio knobs render as native vector silver/chrome widgets
    /// (WidgetRenderStyle::silver) instead of PNG sprite-strip images. Per-node
    /// override via `@sprite` or `@silver` suffix on the Figma layer name.
    bool use_silver_knobs = true;

    /// When true (default), recognised faders/meters are emitted with a
    /// value-driven skin derived from the captured asset (track / fill / thumb
    /// colours for a fader; gradient stops for a meter) so they render like the
    /// captured Figma art while the thumb/level still move with their bound
    /// value. When false (`--fader-style=default` / `--meter-style=default`)
    /// they fall back to the plain native look. The derived style attributes
    /// are stamped onto the node by the import CLI's asset-resolution pass.
    bool skin_faders = true;
    bool skin_meters = true;

    /// Shortcuts pulled from the source by `extract_keyboard_shortcuts(...)`.
    /// The generator emits one `registerShortcut(...)` plus a matching
    /// `__pulpShortcutHandler_N` thunk per entry. The thunk calls
    /// `__dispatch__('__global__', 'keydown', {...})` so the original React
    /// handlers in the bundled JS still own the closure state; native shortcut
    /// interception just re-fires the synthetic keydown into the engine. Empty
    /// vector = no shortcut emission.
    std::vector<DetectedShortcut> shortcuts;

    /// Optional fidelity-report sink. When non-null, codegen runs every
    /// registered reference-free fidelity self-check (see design_fidelity.hpp)
    /// against each element it emits and appends any FidelityIssue here. The
    /// import CLI surfaces these as `fidelity:` warnings and can fail on them
    /// under `--strict-fidelity`. Non-owning; the caller owns the vector.
    std::vector<FidelityIssue>* fidelity_report = nullptr;
};

/// Generate Pulp JS code from a DesignIR.
/// Native mode (default) uses createCol/createRow/createKnob + setFlex.
/// Web-compat mode uses document.createElement + el.style.
std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts = {});

struct CppExportOptions {
    std::string function_name = "build_imported_ui";
    std::string namespace_name;
    std::string header_filename = "imported_ui.hpp";
    std::string binding_function_name = "bind_imported_ui";
    bool include_comments = true;
    bool emit_named_tokens = true;
    bool emit_asset_constants = true;
    bool extract_named_components = true;
    bool emit_binding_context_helpers = true;
    int indent_spaces = 4;
};

struct CppExportResult {
    std::string header;
    std::string source;
    std::string binding_manifest;
};

/// Generate an ownable baked-C++ view artifact from a DesignIR. The result
/// contains a header/source pair: the header declares `function_name()` plus
/// `bake_asset_manifest()`, and the source constructs the view tree directly
/// with Pulp View widgets.
CppExportResult generate_pulp_cpp(const DesignIR& ir,
                                  const IRAssetManifest& manifest,
                                  const CppExportOptions& opts = {});

// ── Baked SwiftUI code generation ───────────────────────────────────────
//
// `--emit swiftui` lowers the same baked DesignIR to native SwiftUI — a
// fourth lowering alongside DOM/native-JS/baked-C++. Baked-only: SwiftUI is a
// compiled declarative target with no Pulp JS-runtime story. The current
// emitter covers stacks / Text / fixed frame-padding-background /
// knob+slider+toggle + a code-first PulpTheme partition + a minimal
// name-keyed binding.

struct SwiftExportOptions {
    /// Generated root `View` struct name. Sanitized to a Swift type name; an
    /// empty/invalid value falls back to "ImportedPulpView".
    std::string root_view_name = "ImportedPulpView";
    /// Generated theme enum/type name (code-first PulpTheme).
    std::string theme_type_name = "PulpTheme";
    bool include_comments = true;
    bool emit_theme = true;            ///< emit the PulpTheme.swift partition
    bool emit_binding_manifest = true; ///< emit the minimal SwiftUI binding manifest
    int indent_spaces = 4;
    /// Optional fidelity sink (parity with CodeGenOptions). SwiftUI-specific
    /// layout issues are not populated yet.
    std::vector<FidelityIssue>* fidelity_report = nullptr;
};

struct SwiftExportResult {
    std::string view_source;       ///< the generated SwiftUI View
    std::string theme_source;      ///< code-first PulpTheme (empty if !emit_theme)
    std::string binding_manifest;  ///< minimal SwiftUI binding manifest JSON
};

/// Generate a baked SwiftUI artifact from a DesignIR. Mirrors the C++ baker's
/// core emit loop: resolves native widget kinds, walks the node tree, and emits
/// a `<root_view_name>` View plus a `<theme_type_name>` token partition. The
/// generated View binds knob/slider/toggle controls to `PulpParameter`s via the
/// `PulpParameterResolving` protocol (exact `PulpParameter.name` match — there
/// is no stable string param key today).
SwiftExportResult generate_pulp_swift(const DesignIR& ir,
                                      const IRAssetManifest& manifest,
                                      const SwiftExportOptions& opts = {});

// ── Shortcut helpers (V2 — used by both the generator and any test/
//    runtime caller that needs to map DetectedShortcut → registerShortcut
//    args). String forms come from `extract_keyboard_shortcuts`; integer
//    forms feed `registerShortcut(key, modifiers, callback)` and the
//    runtime KeyCode enum.

/// Map a W3C `KeyboardEvent.key` (or `KeyboardEvent.code`) string to a
/// Pulp `KeyCode` integer. Returns 0 (KeyCode::unknown) for unrecognized
/// strings — caller should skip those shortcuts (with a warning) rather
/// than wire them with a bogus binding.
int key_string_to_keycode(const std::string& key);

/// Combine modifier strings (`"shift"`, `"ctrl"`, `"alt"`, `"meta"`)
/// into the bitmask `registerShortcut` consumes. `"meta"` maps to
/// `kModCmd` (the platform-primary modifier). `"ctrl"` maps separately to
/// `kModCtrl`, including when `extract_keyboard_shortcuts` captures both
/// modifiers from the `metaKey || ctrlKey` idiom. Unknown strings are silently
/// dropped.
int modifier_strings_to_mask(const std::vector<std::string>& mods);

} // namespace pulp::view
