#include <pulp/view/overlay_dismissal.hpp>

#include <pulp/view/modal.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>

namespace pulp::view {
namespace {

// Pointer-compare only, so it is safe to call with a `needle` that may point at
// freed memory. `~View` clears the slot it holds, so a stale holder should be
// impossible; this stays a guard rather than an assumption because the cost of
// being wrong is a use-after-free on the press path.
bool overlay_still_in_tree(View* needle, View* root) {
    if (!needle || !root) return false;
    if (needle == root) return true;
    for (std::size_t i = 0; i < root->child_count(); ++i)
        if (overlay_still_in_tree(needle, root->child_at(i))) return true;
    return false;
}

// Topmost = last child wins, depth first, mirroring paint order. Invisible
// subtrees are skipped: a hidden modal is not on screen and must not eat keys.
ModalOverlay* topmost_modal(View* root) {
    if (!root || !root->visible()) return nullptr;
    for (std::size_t i = root->child_count(); i > 0; --i)
        if (auto* modal = topmost_modal(root->child_at(i - 1))) return modal;
    return dynamic_cast<ModalOverlay*>(root);
}

// A framework default, not per-editor state: one configuration for the
// process, written at application setup and only read on the press path.
OverlayDismissalPolicy& mutable_policy() {
    static OverlayDismissalPolicy policy;
    return policy;
}

// Whether the press lands on a control that OPENS an overlay, in which case it
// means "switch menus" rather than "dismiss". The trigger itself or any
// ancestor of the hit view may carry the mark, because an imported or scripted
// dropdown is usually a wrapper around the element that actually wins the hit
// test.
bool press_hits_overlay_trigger(View& root, Point root_pt) {
    if (!overlay_dismissal_policy().trigger_press_passes_through) return false;
    for (View* v = root.hit_test(root_pt); v != nullptr; v = v->parent()) {
        if (v->overlay_trigger()) return true;
        if (v == &root) break;
    }
    return false;
}

}  // namespace

const OverlayDismissalPolicy& overlay_dismissal_policy() {
    return mutable_policy();
}

void set_overlay_dismissal_policy(const OverlayDismissalPolicy& policy) {
    mutable_policy() = policy;
}

OverlayPressTarget route_press_to_active_overlay(View& root, Point root_pt) {
    // Walk the overlay stack from the top. An entry that contains the press
    // wins; an entry that does not is dismissed, revealing the one below it.
    // That single loop is what makes a nest of menus behave: pressing a parent
    // menu item while its submenu is open closes only the submenu and lands on
    // the item, instead of either closing the whole nest or losing the press
    // to whatever sibling pixel sits under the submenu.
    bool dismissed_any = false;
    bool consume_press = false;
    // Whether the press lands outside EVERY open overlay, read before any
    // dismissal callback can change the tree. Only then is a press on a
    // nest one "outside the menu" rather than "on the menu under a submenu".
    bool outside_every_overlay = true;
    if (auto* state = root.existing_interaction()) {
        for (View* open : state->overlay_stack)
            if (open != nullptr && overlay_still_in_tree(open, &root) &&
                open->overlay_contains(root_pt)) {
                outside_every_overlay = false;
                break;
            }
    }
    for (;;) {
        auto* state = root.existing_interaction();
        auto* overlay = state ? state->active_overlay : nullptr;
        if (!overlay) break;

        // The root-owned slot guarantees this overlay belongs to this editor.
        // Keep the tree check as a stale-slot safety guard: a detached holder
        // is treated like an outside press and dismissed only from this
        // root's slot.
        if (overlay_still_in_tree(overlay, &root) &&
            overlay->overlay_contains(root_pt)) {
            // Hit-test inside the overlay's own subtree so nested buttons and
            // labels still receive the press. Only route when this resolves to
            // a real view: a null result means the overlay's guards rejected
            // the point, and force-dispatching to the overlay anyway would
            // bypass them. Do not dismiss in that case — the overlay is still
            // mounted.
            if (auto* sub =
                    overlay->hit_test(point_to_local(root_pt, overlay, &root)))
                return {OverlayPressRouting::routed, sub};
            if (dismissed_any)
                return {OverlayPressRouting::dismissed, nullptr, consume_press};
            return {OverlayPressRouting::not_hittable, nullptr};
        }

        // Outside this overlay: auto-release so "dismiss on outside click"
        // works without every JSX caller registering a global click listener.
        // Go through the dismissal path rather than the bare release_overlay()
        // so React state can flip setOpen(false) via on_overlay_dismissed; a
        // bare release leaves the component believing it is still open.
        //
        // Read the consumption decision BEFORE dismissing. A dismissal
        // callback is arbitrary application code that routinely unmounts the
        // popover and reflows what is underneath, so anything asked afterwards
        // answers about a different tree than the one the user pressed on.
        const bool entry_consumes = overlay->overlay_consumes_outside_click() &&
                                    !press_hits_overlay_trigger(root, root_pt);
        // A submenu nested on the overlay below it is one surface with that
        // overlay. When the press is outside the whole nest, stopping here
        // closed only the submenu and left the menu it belonged to open, so
        // the walk continues to the overlay it nests on. A press that lands
        // ON a lower overlay keeps the rule below untouched: a dialog nested
        // on a panel still spends a press on that panel on its own close.
        const auto& stack = state->overlay_stack;
        const View* below = stack.size() >= 2 ? stack[stack.size() - 2] : nullptr;
        const bool defers_to_below = outside_every_overlay && overlay->overlay_nests_on(below);
        consume_press = consume_press || entry_consumes;
        View::dismiss_active_overlay(root);
        dismissed_any = true;

        // An overlay that consumes its outside press spends that press on the
        // close; nothing below it — not even a parent menu — may also act on
        // it. Without this the one press would close a modal AND operate the
        // menu behind it. The one exception is a press outside the whole nest
        // (above): every level closes, and the consumption carried so far
        // still holds, so nothing behind the nest receives it.
        if (entry_consumes && !defers_to_below)
            break;

        // No-progress guard: a dismissal callback may re-claim the same view,
        // and a loop that kept asking would never terminate.
        auto* after = root.existing_interaction();
        if (after != nullptr && after->active_overlay == overlay) break;
    }
    if (!dismissed_any) return {};
    return {OverlayPressRouting::dismissed, nullptr, consume_press};
}

ContextPressResult route_context_press(View& root, Point root_pt) {
    const auto overlay_press = route_press_to_active_overlay(root, root_pt);
    if (overlay_press.consume_press) {
        // The overlay opted to consume the pointer sequence that dismissed it.
        // Stop before the underlay hit-test: otherwise this one right-click
        // both closes the popover and opens a context menu on the control
        // beneath it, which is the same click-through violation the left
        // button already fences off.
        return {false, true};
    }
    auto* target = overlay_press.routing == OverlayPressRouting::routed
                       ? overlay_press.target
                       : root.hit_test(root_pt);
    return {dispatch_context_menu(root, target, root_pt),
            overlay_press.routing == OverlayPressRouting::dismissed};
}

OverlayEscapeResult route_escape_to_active_overlay(View& root,
                                                   std::uint16_t modifiers,
                                                   bool is_repeat) {
    if (auto* modal = topmost_modal(&root)) {
        KeyEvent ke;
        ke.key = KeyCode::escape;
        ke.modifiers = modifiers;
        ke.is_down = true;
        ke.is_repeat = is_repeat;
        if (modal->on_key_event(ke)) return OverlayEscapeResult::modal;
    }

    if (ComboBox::active_popup_in(root)) {
        ComboBox::close_active_popup(root);
        return OverlayEscapeResult::combo_popup;
    }

    // `existing_interaction()` rather than `interaction()`: a tree that never
    // claimed anything must not have a state block allocated onto it by a
    // keystroke that will find nothing.
    auto* state = root.existing_interaction();
    if (auto* overlay = state ? state->active_overlay : nullptr) {
        overlay->dismiss_claimed_overlay();
        return OverlayEscapeResult::overlay;
    }

    return OverlayEscapeResult::none;
}

bool root_overlay_owns_keyboard(View& root) {
    // Slot reads first, modal walk last: the two slot reads are O(1) and the
    // walk is O(tree) with a dynamic_cast per node. This runs at press and
    // focus-sync frequency rather than Escape frequency, which is the same
    // order as the host's own hit test on the same press, but there is no
    // reason to pay it when a cheaper answer already said yes.
    auto* state = root.existing_interaction();
    auto* overlay = state ? state->active_overlay : nullptr;
    if (overlay != nullptr && overlay->overlay_consumes_outside_click())
        return true;
    if (ComboBox::active_popup_in(root)) return true;
    return topmost_modal(&root) != nullptr;
}

}  // namespace pulp::view
