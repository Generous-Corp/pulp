#pragma once

/// @file design_codegen.hpp
/// Code generation from a DesignIR: native-bridge / web-compat JS emission,
/// baked-C++ view artifact export, and the shortcut string→KeyCode/mask
/// helpers shared by the generator and runtime callers.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_shortcuts.hpp>  // DetectedShortcut (CodeGenOptions::shortcuts)
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

    /// pulp #3191 — when true (default), recognised faders/meters are emitted
    /// with a value-driven skin DERIVED from the captured asset (track / fill /
    /// thumb colours for a fader; gradient stops for a meter) so they render
    /// like the captured Figma art while the thumb/level still move with their
    /// bound value. When false (`--fader-style=default` / `--meter-style=default`)
    /// they fall back to the plain native look. The derived style attributes
    /// are stamped onto the node by the import CLI's asset-resolution pass.
    bool skin_faders = true;
    bool skin_meters = true;

    /// pulp #2116 V2 — shortcuts pulled from the source by
    /// `extract_keyboard_shortcuts(...)` (Strategy A: synthetic keydown
    /// re-dispatch). The generator emits one `registerShortcut(...)` plus
    /// a matching `__pulpShortcutHandler_N` thunk per entry. The thunk
    /// calls `__dispatch__('__global__', 'keydown', {...})` so the
    /// original React handlers in the bundled JS still own the closure
    /// state — we just intercept the OS chord and re-fire the synthetic
    /// keydown into the engine. Empty vector = no shortcut emission.
    std::vector<DetectedShortcut> shortcuts;

    /// Optional fidelity-report sink. When non-null, codegen records a
    /// FidelityIssue for any image it cannot prove it sized faithfully — a
    /// bleed sprite whose emitted aspect diverges from its source PNG (skew),
    /// or one missing the pixel dims needed to preserve aspect at all. This is
    /// a reference-free self-consistency check (it compares the emitted
    /// geometry against the asset's own pixels), so it generalizes to any
    /// import without overfitting. Non-owning; the caller owns the vector.
    std::vector<struct FidelityIssue>* fidelity_report = nullptr;
};

/// A single import-fidelity self-check finding.
struct FidelityIssue {
    std::string node_id;    ///< sanitized bridge id of the offending node
    std::string node_name;  ///< source layer name (for human-readable reports)
    std::string kind;       ///< "skew" | "aspect-unverified"
    std::string detail;     ///< one-line explanation with the measured numbers
};

/// Reference-free check: did codegen size this image faithfully? Returns a
/// finding when a BLEED sprite (render_bounds or asset_bleed) was emitted at an
/// aspect that diverges from its source PNG (skew), or lacks the PNG dims
/// needed to preserve aspect (aspect-unverified). Ordinary (non-bleed) images
/// intentionally fill their declared box and are never flagged. Pure +
/// testable in isolation; `emitted_w/h` are the dimensions codegen emitted.
std::optional<FidelityIssue> check_image_sizing_fidelity(
    const IRNode& node, const std::string& node_id,
    float emitted_w, float emitted_h);

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
/// `kModCmd` (the platform-primary modifier) per the cross-platform
/// idiom captured in `extract_keyboard_shortcuts` (metaKey || ctrlKey
/// collapses to "meta"). Unknown strings are silently dropped.
int modifier_strings_to_mask(const std::vector<std::string>& mods);

} // namespace pulp::view
