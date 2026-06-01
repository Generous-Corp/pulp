// core/view/include/pulp/view/design_fidelity.hpp
//
// Reference-free import-fidelity self-checks. Each invariant is a small pure
// function registered in one table; codegen captures the geometry it already
// computes and calls run_fidelity_checks() at each branch. Adding an invariant
// = one function + one registry line here — design_codegen.cpp does not grow.
//
// Every check reads only normalized DesignIR geometry + the emitted dimensions,
// never a source quirk, so all sources (figma, figma-plugin, pencil, stitch,
// v0, claude) are validated by the same checks.
#pragma once

#include <pulp/view/design_ir.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// A single import-fidelity self-check finding.
struct FidelityIssue {
    std::string node_id;    ///< sanitized bridge id of the offending node
    std::string node_name;  ///< source layer name (for human-readable reports)
    std::string kind;       ///< "skew" | "aspect-unverified" | "gross-size"
                            ///<   | "widget-size" | "widget-undersized" | "text-vcenter"
                            ///<   | "dropped-vector"
    std::string detail;     ///< one-line explanation with the measured numbers
    bool informational = false;  ///< advisory only (e.g. a below-native-minimum
                                 ///< clamp-up); --strict-fidelity must NOT fail on it
};

/// What codegen branch produced the emitted geometry — lets a check apply only
/// where it is meaningful without re-deriving the element kind.
enum class FidelityElement { image, container, widget, text };

/// Everything a check needs: the node, its sanitized bridge id, the dimensions
/// codegen emitted for it, and which branch produced them.
struct FidelityContext {
    const IRNode& node;
    std::string node_id;
    float emitted_w = 0.0f;
    float emitted_h = 0.0f;
    FidelityElement element = FidelityElement::container;
};

// ── Individual invariants (pure; testable in isolation) ──────────────────────

/// No-skew: a BLEED sprite (render_bounds or asset_bleed) must be emitted at an
/// aspect matching its source PNG; missing PNG dims → aspect-unverified.
std::optional<FidelityIssue> check_image_sizing_fidelity(const FidelityContext&);

/// Gross-size: a node the user pinned on BOTH axes (SizingMode::fixed) must not
/// be emitted >3x its source box; hug/fill/absolute/display:none self-skip.
std::optional<FidelityIssue> check_gross_size_divergence(const FidelityContext&);

/// Widget-intrinsic-size: a recognized audio widget whose emitted box diverges
/// >1.5x from its source intrinsic size (shape_width/height attrs, else style
/// box). A source below the widget's native usable minimum is reported as
/// informational "widget-undersized" (codegen clamps up); a real divergence is
/// "widget-size". Heuristic detections with no source dims self-skip.
std::optional<FidelityIssue> check_widget_intrinsic_size(const FidelityContext&);

/// Text-vertical-centering: a SINGLE-LINE text given a reserved vertical slot
/// (emitted box taller than one line, below the multi-line threshold) must be
/// emitted vertically centered. A top-aligned single-line label in a tall slot
/// is "text-vcenter". Multi-line, no-slack, and missing-metric cases self-skip.
std::optional<FidelityIssue> check_text_vertical_centering(const FidelityContext&);

// ── Registry + entry point ───────────────────────────────────────────────────

/// Run every registered fidelity check against `ctx`, appending findings to
/// `sink`. This is the ONLY entry point codegen calls. The registry below is
/// the single place to add a new invariant.
void run_fidelity_checks(const FidelityContext& ctx, std::vector<FidelityIssue>& sink);

// ── Tree-level invariants (need subtree / coverage context) ──────────────────

/// Vector-renderability: every visible vector/path-like node (svg path/rect/
/// line, ellipse, polygon, …) above an area threshold must map to a real
/// renderable primitive — a rasterized asset, a native widget, child paint, or
/// a visible fill — OR already carry an explicit unsupported diagnostic. A bare
/// vector that codegen would drop to an empty `createRow` frame (no asset, no
/// children, no fill — its stroke/path/border art silently vanishes) is
/// reported as "dropped-vector".
///
/// This is a TREE pass, not a per-element registry check, for two reasons the
/// single-node FidelityContext cannot serve: (1) the dropped node hits codegen's
/// generic-frame fall-through, which has no run_fidelity_checks call site; and
/// (2) the false-positive gate "a vector child consumed into a parent widget is
/// not dropped" needs subtree context. The walk mirrors generate_native_node's
/// recursion exactly — it descends EXCEPT into the terminal image/widget/text
/// branches — so a knob's consumed stroke-ellipse is never falsely flagged.
///
/// `diagnostics` is `DesignIR::diagnostics`: a node already carrying a
/// render-affecting import diagnostic (matched by stable_anchor_id or structural
/// path) is suppressed so the silent-drop finding never double-reports a drop
/// the importer already surfaced. `node_id_of` maps a node to the bridge id
/// codegen emitted for it (codegen passes a lookup over its real id map; tests
/// pass an identity-ish stub).
void check_vector_renderability(
    const IRNode& root,
    const std::vector<ImportDiagnostic>& diagnostics,
    const std::function<std::string(const IRNode&)>& node_id_of,
    std::vector<FidelityIssue>& sink);

/// Number of findings that should fail `--strict-fidelity`. Informational
/// findings (advisory clamp-ups etc.) are surfaced as warnings but never gate
/// the import, so the CLI decides its exit code from this count, not the raw
/// finding count.
std::size_t count_strict_fidelity_failures(const std::vector<FidelityIssue>& issues);

}  // namespace pulp::view
