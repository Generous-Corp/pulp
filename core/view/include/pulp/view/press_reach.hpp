#pragma once

/// @file press_reach.hpp
/// Ask what a press at a point actually reaches, and audit a whole view tree
/// for controls that advertise a press target no press can reach.
///
/// This is deliberately NOT rect arithmetic. A checker that compares a
/// control's own hit rect against its own painted rect can only see defects a
/// control commits against itself, and the expensive defect is the one a
/// control commits against its ANCESTORS: an intermediate node with a
/// zero-area box seals off a subtree of perfectly sized, perfectly wired
/// controls, and every one of them still passes a self-vs-self comparison.
///
/// `Rect::contains` is half-open, so a zero-area rect contains no point at
/// all. A 0x0 wrapper therefore admits nothing through `View::hit_test`, and
/// the only reason such a subtree ever appears to work is the symmetric ~500px
/// slack `hit_test` grants an `overflow: visible` child — slack measured from
/// the wrapper's own in-flow position, which can be nowhere near where the
/// control paints. The control looks alive in a screenshot, answers a
/// selector-addressed synthetic dispatch, and cannot be pressed.
///
/// So the question these verbs answer is the end-to-end one: run the real
/// `View::hit_test` at the point the control PAINTS, and report what came
/// back. Nothing here infers; everything here resolves.
///
/// The audit reports a census alongside its findings on purpose. A reach audit
/// that examined nothing is not a clean tree, and "0 unreachable" and "0
/// examined" are indistinguishable to a caller who only reads the finding
/// list. Check `PressReachAudit::examined` before believing `clean()`.

/// `View::simulate_context_click` lives in `view.hpp` beside the rest of the
/// simulate_* family, and is implemented here so the one policy stays in one
/// place. It exists because `simulate_click(pt, {.button = MouseButton::right})`
/// does NOT open a context menu, and nothing about its name says so: that
/// overload delivers a right-button press through the ordinary mouse channels,
/// while the context-menu channel is a separate dispatch the hosts perform from
/// `rightMouseDown:` and its siblings. A test author who reaches for the button
/// field, sees no menu, and concludes the feature is broken has been misled by
/// the API rather than by the code.

#include <pulp/view/view.hpp>

#include <optional>
#include <string>
#include <vector>

namespace pulp::view {

/// Which handler channel a press looks for, and therefore which resolution
/// rule applies. The two are genuinely different, and conflating them is how a
/// right-click audit silently passes.
enum class PressChannel {
    /// Left button. `View::on_click`, resolved with the DOM-style bubble walk
    /// `View::simulate_click` and every platform host perform: the deepest hit
    /// view, then up the parent chain to the nearest ancestor carrying one.
    click,
    /// Right button. `View::on_context_menu`, resolved with the same bubble
    /// `dispatch_context_menu` performs: nearest handler from the hit view up
    /// to the root, first listener wins. A decorative child on top of a wired
    /// wrapper is therefore forgiven — correctly, since the real dispatch
    /// forgives it too. What is NOT forgiven is a press that leaves the
    /// control's subtree entirely, because the bubble then walks somebody
    /// else's ancestors.
    context_menu,
};

/// Human-readable channel name, for reports and test failure messages.
const char* press_channel_name(PressChannel channel);

/// What a press at a point resolves to, WITHOUT delivering it.
///
/// Resolution only: no handler runs, no overlay is dismissed, no application
/// state moves. That is what makes it usable as a gate — a gate that fires
/// every control it inspects is an integration test, not a check.
struct PressReach {
    /// Deepest hit-testable view at the point, or nullptr when the point
    /// resolves to nothing at all. `nullptr` here and "a handler-less view
    /// swallowed it" are different diagnoses; keep them apart.
    View* hit = nullptr;
    /// The view whose handler would actually run for this channel, or nullptr
    /// when the channel is dead at this point.
    View* handler = nullptr;
    /// `root_pt` in `handler`'s local coordinates. Meaningless when `handler`
    /// is null.
    Point local{};
};

/// Resolve — without delivering — what a press at `root_pt` would reach.
///
/// `root_pt` is in `root`-local (window) coordinates, the same space the
/// platform hosts hand to `route_context_press` and `View::simulate_click`.
PressReach reach_of_press(View& root, Point root_pt, PressChannel channel = PressChannel::click);

/// `view`'s painted rect in `root`-local coordinates, or `std::nullopt` when
/// that cannot be established exactly.
///
/// Exactly, not approximately. The walk sums `bounds()` offsets and subtracts
/// every `ScrollView` ancestor's scroll — but a `set_scale` or explicit
/// transform matrix anywhere on the path makes that walk wrong, and a wrong
/// rect turns this whole instrument into a generator of confident nonsense.
/// So the result is VERIFIED against `point_to_local`, the same function the
/// hosts route real presses through, and the walk reports failure rather than
/// a guess when the round trip does not land inside `view`.
std::optional<Rect> rect_in_root(const View& view, View& root);

/// One control that advertises a press target but cannot be reached by a press
/// at the rect it paints.
struct UnreachablePressTarget {
    View* view = nullptr;
    /// The view's `id()`, which for a materialized or imported design is the
    /// authored element id — the name a reader can search for.
    std::string id;
    /// The rect the control paints, in root coordinates.
    Rect painted{};
    PressChannel channel = PressChannel::click;
    /// The point that was pressed — the painted rect's centre.
    Point probe{};
    /// What the press resolved to instead, or nullptr for nothing at all.
    View* reached = nullptr;
    std::string reached_id;
    /// Why this is a finding, in one sentence.
    std::string reason;
};

/// What an audit examined, so a caller can tell a clean tree from a blind one.
struct PressReachAudit {
    /// Controls that advertise a press handler AND were actually probed.
    int examined = 0;
    /// Advertised a handler but sits outside the root's visible area (a
    /// scrolled-away row). Not a defect; not evidence of health either.
    int skipped_offscreen = 0;
    /// Advertised a handler but is hidden, disabled, or opted out of hit
    /// testing. Authored state, not a defect.
    int skipped_not_interactive = 0;
    /// Advertised a handler but sits under a transform whose root-space rect
    /// could not be established exactly. NOT counted as reachable: an audit
    /// must never launder "I could not measure this" into "this is fine".
    int skipped_indeterminate = 0;
    std::vector<UnreachablePressTarget> unreachable;

    /// True when nothing was found unreachable. Meaningless on its own — a
    /// caller must also require `examined > 0`.
    bool clean() const {
        return unreachable.empty();
    }
};

/// Tuning for `audit_press_reach`. The defaults audit both channels over the
/// whole tree.
struct PressReachOptions {
    bool audit_click = true;
    bool audit_context_menu = true;
    /// Ignore a control whose painted rect is smaller than this in either
    /// axis. Zero audits everything, including the zero-area wrappers this
    /// instrument exists to catch, so raise it only with a reason.
    float min_dimension = 0.0f;
};

/// Walk `root` and press every control that advertises a press handler, at the
/// centre of the rect it paints.
///
/// A control counts as reachable when the real `View::hit_test` at that point
/// lands on the control itself or inside its subtree AND the channel resolves
/// to a live handler. The subtree rule is deliberate: a container whose own
/// child wins the press is not sealed off, and reporting it would bury the
/// real findings in nesting noise.
PressReachAudit audit_press_reach(View& root, const PressReachOptions& options = {});

/// Format an audit as a report a human reads in CI output. Always includes the
/// census, including when there are no findings.
std::string format_press_reach_audit(const PressReachAudit& audit);

} // namespace pulp::view
