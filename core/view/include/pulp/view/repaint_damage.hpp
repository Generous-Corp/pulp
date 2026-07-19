#pragma once

#include <pulp/view/geometry.hpp>

// FU-2 / WI-17 — the partial-repaint damage model.
//
// `compute_effective_damage` is the pure, side-effect-free heart of partial
// repaint: given the requested dirty rect (in ROOT logical coordinates, exactly
// what `View::request_repaint(Rect)` produces) it returns either a bounded clip
// rect that is PIXEL-IDENTICAL to a full repaint, or a "repaint everything"
// escalation when any view in the tree would sample pixels the clip cannot
// preserve.
//
// It has no GPU, canvas, or platform dependency and is unit-tested directly
// (test/test_partial_repaint_equivalence.cpp). The mac GPU host is the only
// consumer today, gated behind PULP_PARTIAL_REPAINT=1.

namespace pulp::view {

class View;

/// The result of the damage decision.
///
///  - `full == true`  → repaint the whole surface (`bounds` is unspecified).
///  - `full == false` → repaint only `bounds` (root logical coords, already
///                       snapped OUT to the device-pixel grid), which is
///                       pixel-identical to a full repaint.
struct DamageDecision {
    bool full = true;
    Rect bounds{};
};

/// Decide whether `requested_root_rect` can be repainted as a bounded clip
/// (pixel-identical to a full repaint) or must escalate to a full repaint.
///
/// The rule is the pixel-identity constraint: clipping the repaint is identical
/// to a full repaint iff no draw whose output touches the clip SAMPLES pixels
/// whose value differs between "preserved previous frame" and "would-be-fully-
/// repainted frame". Point-wise ops (plain draws, opacity/blend layers,
/// colour-only filter chains, analytic box shadows) are safe. The hazards are
/// ops that sample at a distance and can straddle the clip boundary:
///   - a view with `backdrop_blur > 0` (samples the backdrop behind it);
///   - a view with `filter_blur > 0` or a filter chain containing blur/
///     drop-shadow (spreads its own content past its box);
///   - a view with a compositing effect that samples (`effect() &&
///     effect()->needs_layer()`);
///   - a view with a mask layer (mask geometry is layer-relative; a truncated
///     layer is at minimum suspicious);
///   - a view under a render transform or a child-paint-offset (scroll) —
///     its painted box cannot be derived from the plain ancestor-origin walk,
///     so its extent is unknown.
///
/// Each hazard's painted box (inflated by its sample radius) is tested against
/// the requested damage. If any hazard reaches the damage, the decision is
/// FULL. Otherwise the requested rect — snapped OUT to the `device_scale` pixel
/// grid to avoid AA seams at the clip edge — is returned as the bounded clip.
///
/// v1 policy is ESCALATE, not grow-to-fixpoint: a hazard that intersects the
/// damage returns full rather than unioning the hazard's extent and rescanning.
/// This is always pixel-safe (it only forgoes the optimization) and matches the
/// producer-side conservatism in `View::request_repaint(Rect)`.
DamageDecision compute_effective_damage(const View& root,
                                        const Rect& requested_root_rect,
                                        float device_scale = 1.0f);

}  // namespace pulp::view
