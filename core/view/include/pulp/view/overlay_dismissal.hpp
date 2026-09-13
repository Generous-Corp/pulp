#pragma once

/// @file overlay_dismissal.hpp
/// The one implementation of Pulp's overlay-dismissal policy.
///
/// Pulp has two independent popup mechanisms that a platform host must keep in
/// step:
///
///   A. `ComboBox`'s open native dropdown, a paint-only overlay with no view
///      backing, reached through `ComboBox::active_popup_in` /
///      `close_active_popup` / `notify_global_click`.
///   B. The generalized overlay slot (`RootInteractionState::active_overlay`),
///      which `@pulp/react`'s `<View overlay>` prop — and therefore every
///      imported or materialized design's popover — claims via
///      `View::claim_overlay()`.
///
/// Deciding what a press or an Escape means for those two lives HERE, not in
/// each host. Hosts own only the native event plumbing and call one verb.
///
/// That split is not cosmetic. The policy was previously hand-rolled once per
/// host, and every copy drifted differently: one dismissed another editor's
/// popover because it read the process-global shim mirror instead of the
/// root-owned slot, one never honoured an overlay's outside-click consumption,
/// and the two DAW plugin hosts had no Escape path at all, so a popover opened
/// inside a plugin editor could not be closed from the keyboard. A single
/// implementation is also the only version that is headlessly testable —
/// per-host copies can only be exercised by running a real NSView / HWND /
/// browser.
///
/// `tools/scripts/overlay_dismissal_wiring_guard.py` enforces the arrangement:
/// a host that routes presses must call these verbs, and no host may reach
/// past them to the slot itself.

#include <pulp/view/view.hpp>

#include <cstdint>

namespace pulp::view {

/// What consulting the generalized overlay slot decided about a press.
enum class OverlayPressRouting {
    /// Nothing claimed the root's overlay slot; the caller proceeds normally.
    no_overlay,
    /// The press landed inside the overlay and resolved to a view in its
    /// subtree. Deliver to `OverlayPressTarget::target` and stop; the regular
    /// tree `hit_test` must not run, or an absolutely-positioned popover child
    /// loses the click to whatever sibling occupies that pixel.
    routed,
    /// The press landed inside the overlay's rect, but the overlay's own
    /// visible / enabled / hit-testable / pointer-events guards rejected it.
    /// The overlay stays claimed (it is still mounted, just not interactive
    /// here) and the caller falls through to the regular hit test.
    not_hittable,
    /// The press landed outside the overlay, so it was dismissed. The caller
    /// still falls through to the regular hit test, matching WebView's
    /// outside-click closes-and-clicks-through behavior.
    dismissed,
};

struct OverlayPressTarget {
    OverlayPressRouting routing = OverlayPressRouting::no_overlay;
    /// Non-null only when `routing == OverlayPressRouting::routed`.
    View* target = nullptr;
    /// True when an outside press dismissed an overlay that opted to consume
    /// that initiating pointer sequence. Hosts must stop before gesture and
    /// ordinary hit-test routing so the same press cannot mutate the underlay.
    ///
    /// False even for such an overlay when the press landed on an overlay
    /// TRIGGER and `OverlayDismissalPolicy::trigger_press_passes_through` is
    /// set: switching dropdowns is one press, not two.
    bool consume_press = false;
};

/// Tunable defaults for the dismissal policy. One configuration for the
/// process, deliberately: this is a framework default, not per-editor state,
/// and it is read-only on the press path.
struct OverlayDismissalPolicy {
    /// A press that dismisses an open overlay is delivered to the control
    /// under it when that control is an overlay TRIGGER
    /// (`View::overlay_trigger()`), so switching from one dropdown to a
    /// sibling costs one press rather than two — the behaviour of the macOS
    /// menu bar and of every multi-menu toolbar.
    ///
    /// Scoped to triggers on purpose. Passing every dismissing press through
    /// would mean clicking away from a menu also operates whatever control
    /// happens to sit under the click, which is a real hazard rather than a
    /// hypothetical one; an overlay that asked to consume its outside click
    /// still consumes it everywhere else.
    ///
    /// Set false to restore strict consume-everywhere dismissal.
    bool trigger_press_passes_through = true;
};

/// The active policy. Reading is cheap and allocation-free.
const OverlayDismissalPolicy& overlay_dismissal_policy();
/// Replace the active policy. Call from application setup, not mid-gesture.
void set_overlay_dismissal_policy(const OverlayDismissalPolicy& policy);

/// Consult `root`'s generalized overlay slot for a press at `root_pt`.
///
/// Scoped to `root`'s own interaction state, never the process-global shim
/// mirror: two Pulp editors in one host process (the documented shared
/// `AUHostingService` case) each own their overlay, and a press in one must
/// not dismiss the other's popover.
///
/// Call this AFTER the ComboBox popup routing — which stays exact-as-was, per
/// the regression in `test_combo_dropdown.cpp` — and BEFORE the regular tree
/// `hit_test`.
OverlayPressTarget route_press_to_active_overlay(View& root, Point root_pt);

/// What a context (right-button) press resolved to.
struct ContextPressResult {
    /// A view claimed the context menu, so the host must not fall through to
    /// the platform's own menu.
    bool handled = false;
    /// The press dismissed an open overlay, so the host must repaint even when
    /// nothing claimed a context menu.
    bool overlay_dismissed = false;
};

/// Route a context (right-button) press at `root_pt`, consulting the
/// generalized overlay slot first.
///
/// This is the right-button counterpart of the left-button overlay routing a
/// host performs around `route_press_to_active_overlay`, and it exists because
/// honoring `OverlayPressTarget::consume_press` is easy to omit: a host that
/// forgets it dismisses the overlay and THEN hit-tests the underlay, so one
/// right-click both closes the popover and opens a context menu on the control
/// underneath it. Keeping the decision here rather than in each host makes it
/// headlessly testable and gives every host one call to make.
ContextPressResult route_context_press(View& root, Point root_pt);

/// What an Escape keypress dismissed, in the order the policy tries them.
enum class OverlayEscapeResult {
    /// Nothing was open under `root`. The caller must handle Escape itself
    /// (blur a text field, hand the key back to the DAW, …).
    none,
    /// A `ModalOverlay` in the tree consumed Escape through its own handler.
    modal,
    /// An open native `ComboBox` dropdown was closed.
    combo_popup,
    /// The generalized overlay slot was dismissed, firing
    /// `on_overlay_dismissed` so React state can flip `setOpen(false)`.
    overlay,
};

/// Dismiss the topmost thing Escape should close under `root`.
///
/// Order, and why it is this order:
///
///   1. A `ModalOverlay` — it traps interaction, so nothing behind it may act
///      on the key. Found by tree walk rather than by focus, because a modal
///      that never took focus still owns the screen, and because in a plugin
///      host the DAW may hold focus entirely.
///   2. An open `ComboBox` dropdown. `ComboBox::on_key_event` fires only while
///      the combo owns focus, which a sibling React popover routinely steals;
///      without a host-level fallback the still-visible dropdown wedges open
///      with no keyboard escape route.
///   3. The generalized overlay slot. `ModalOverlay`, `ComboBox`, and
///      `CallOutBox` each own their Escape; a bare `<View overlay>` popover has
///      no widget-specific owner, so this is the only path that closes it.
///
/// `modifiers` and `is_repeat` shape the `KeyEvent` handed to a modal, so an
/// Escape chord a modal declines still falls through to the rest of the policy.
///
/// Every host must call this BEFORE any focus-gated key dispatch. A plugin host
/// returns early when nothing in its tree holds focus — which is exactly the
/// state an open popover leaves behind — so an Escape path placed after that
/// check can never run.
OverlayEscapeResult route_escape_to_active_overlay(View& root,
                                                   std::uint16_t modifiers = 0,
                                                   bool is_repeat = false);

/// Whether an overlay under `root` is the kind that should own the keyboard.
///
/// A plugin host needs this before the key ever arrives. An embedded editor
/// borrows the DAW keyboard only for an active bounded interaction, so with
/// nothing focused it is not first responder and never receives Escape at all
/// — which is precisely the state an open popover leaves behind.
///
/// Deliberately NARROWER than what `route_escape_to_active_overlay` acts on,
/// because holding a DAW's keyboard when nothing needs it is the more
/// expensive mistake: the user experiences it as transport and Musical Typing
/// going dead. Three things qualify, and a bare claim does not:
///
///   - A visible `ModalOverlay`. It traps interaction by definition.
///   - An open `ComboBox` dropdown. An open menu owns arrows, Enter, Escape.
///   - A claimed overlay that consumes its outside click. That flag is the
///     author's STATEMENT that the view is a popover — `@pulp/react`'s
///     `<View overlay>` prop and `data-overlay="true"` both set it. The
///     web-compat CSS-shape heuristic (`position:absolute` + a high
///     `z-index`) deliberately does not, because it is an INFERENCE that can
///     fire on a decorative absolutely-positioned box; such a box can hold
///     the slot for an editor's whole lifetime, and keeping the keyboard that
///     long would be indistinguishable from the plug-in stealing it.
bool root_overlay_owns_keyboard(View& root);

}  // namespace pulp::view
