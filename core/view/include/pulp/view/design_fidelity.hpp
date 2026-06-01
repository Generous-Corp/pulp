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

#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// A single import-fidelity self-check finding.
struct FidelityIssue {
    std::string node_id;    ///< sanitized bridge id of the offending node
    std::string node_name;  ///< source layer name (for human-readable reports)
    std::string kind;       ///< "skew" | "aspect-unverified" | "gross-size"
    std::string detail;     ///< one-line explanation with the measured numbers
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

// ── Registry + entry point ───────────────────────────────────────────────────

/// Run every registered fidelity check against `ctx`, appending findings to
/// `sink`. This is the ONLY entry point codegen calls. The registry below is
/// the single place to add a new invariant.
void run_fidelity_checks(const FidelityContext& ctx, std::vector<FidelityIssue>& sink);

}  // namespace pulp::view
