// test_hosting_input_smoke.cpp — headless characterization of the view/input
// stack that the DAW/standalone window hosts drive.
//
// This is the regression anchor for two behavior-preserving refactors tracked
// under pulp #6223:
//
//   * S31 extracts the macOS mouse down/up dispatch — currently duplicated and
//     drift-prone across core/view/platform/mac/window_host_mac.mm and
//     plugin_view_host_mac.mm — into portable pulp::view verbs (the same shape
//     as the already-extracted deliver_mouse_drag / deliver_mouse_wheel in
//     pointer_dispatch.hpp). The cases below pin the *portable* primitives that
//     the extracted verb will compose: hit_test targeting, the
//     simulate_click / simulate_drag click-synthesis contract, and the W3C
//     on_click bubble bounded at the receiver.
//
//   * S11 moves the process-wide UI statics (View::focused_input_,
//     View::active_overlay_, ComboBox::active_popup_, and the overlay queue) to
//     a root-owned interaction struct reached via tree_root(), keeping the
//     static APIs as forwarding shims. The cases below pin the *single-instance*
//     focus / overlay / popup lifecycle those statics implement today — claim,
//     guarded release, blur-on-swap, destructor auto-clear, and popup
//     open/dismiss — so the shim refactor is provably non-regressing.
//
// Everything here is headless and window-free: it drives the real pulp::view
// input path in-process. Two independent View trees stand in for two hosted
// editors; the multi-instance ISOLATION assertion (two editors not sharing a
// focus/popup slot) is intentionally NOT here — it fails on main by design and
// ships with the S11 PR that fixes it.
//
// Where a contract lives only in the mac hosts today (not in a portable verb),
// that is called out in a comment so S31 knows to preserve the observable
// result when it lifts the code out. The clearest example: the host fires
// on_click only when the mouse-up hit target equals the mouse-down target
// (plugin_view_host_mac.mm: `released == *drag_target`). The portable
// simulate_click uses one point so cannot express down-A/up-B; the portable
// stand-in for "an up somewhere else is not a click" is simulate_drag, which
// synthesizes no click at all (asserted below).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/view.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/input_events.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// Records the ordered stream of pointer callbacks a widget receives so a test
// can assert the exact down → up → click sequence the portable simulate_*
// path drives, plus the local coordinates each callback saw.
class InputSpy : public View {
public:
    InputSpy() {
        on_click = [this] {
            ++clicks;
            log.emplace_back("click");
        };
    }
    void on_mouse_down(Point p) override {
        ++downs;
        last_down = p;
        log.emplace_back("down");
    }
    void on_mouse_up(Point p) override {
        ++ups;
        last_up = p;
        log.emplace_back("up");
    }
    void on_mouse_drag(Point p) override {
        ++drags;
        last_drag = p;
        log.emplace_back("drag");
    }
    bool wants_mouse_input() const override { return true; }

    int downs = 0, ups = 0, drags = 0, clicks = 0;
    Point last_down{}, last_up{}, last_drag{};
    std::vector<std::string> log;
};

// A focusable, text-accepting widget — the shape the host focus protocol
// targets. Nothing here claims focus on its own: focus assignment on click is a
// host responsibility today (the drift S31/S11 unify), so tests drive the
// focus primitives directly, exactly as the mac hosts do.
class FocusSpy : public View {
public:
    FocusSpy() { set_focusable(true); }
    bool accepts_text_input() const override { return true; }
};

// Reset the process-global interaction slots so Catch2's single-process run
// cannot leak state between cases. Mirrors the invariants the destructors and
// the host teardown maintain.
void reset_interaction_statics() {
    ComboBox::close_active_popup();      // clears ComboBox::active_popup_
    View::focused_input_ = nullptr;
    View::active_overlay_ = nullptr;
}

}  // namespace

// ── 1. Mouse down → up → synthesized click, in order ─────────────────────────
// Pins the portable single-path click contract simulate_click implements:
// hit_test the point, deliver on_mouse_down then on_mouse_up to the hit target
// in its LOCAL coordinates, then synthesize on_click bubbled up to the nearest
// ancestor handler. The extracted S31 verb must reproduce this observable
// sequence.
TEST_CASE("simulate_click delivers down, up, then a synthesized click in order",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<InputSpy>();
    InputSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    root.simulate_click({130, 70});

    REQUIRE(spy->downs == 1);
    REQUIRE(spy->ups == 1);
    REQUIRE(spy->clicks == 1);
    // Order is load-bearing: down, then up, then the click bubble.
    REQUIRE(spy->log == std::vector<std::string>{"down", "up", "click"});
    // Callbacks see the target's LOCAL coordinates (130-100, 70-50).
    CHECK_THAT(spy->last_down.x, WithinAbs(30.0, 1e-4));
    CHECK_THAT(spy->last_down.y, WithinAbs(20.0, 1e-4));
    CHECK_THAT(spy->last_up.x, WithinAbs(30.0, 1e-4));
    CHECK_THAT(spy->last_up.y, WithinAbs(20.0, 1e-4));
}

// ── 2. A drag that ends elsewhere is NOT a click ─────────────────────────────
// The portable stand-in for the host's "on_click only when up-target ==
// down-target" rule (plugin_view_host_mac.mm `released == *drag_target`).
// simulate_drag delivers down + drag ticks + up to the START target and
// synthesizes NO click — so a press-move-release gesture never fires on_click.
TEST_CASE("simulate_drag delivers down/drag/up but synthesizes no click",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<InputSpy>();
    InputSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    // Start inside the child; end well outside it.
    root.simulate_drag({130, 70}, {330, 250}, /*steps=*/5);

    REQUIRE(spy->downs == 1);
    REQUIRE(spy->drags == 5);
    REQUIRE(spy->ups == 1);
    REQUIRE(spy->clicks == 0);   // a drag is never a click
}

// ── 3. Click bubbles to the nearest ancestor handler, and only that one ───────
// hit_test returns the DEEPEST hit-testable view; simulate_click then walks up
// to the nearest ancestor with an on_click and fires exactly that handler. This
// is the W3C "click bubbles until handled" behavior; there is no separate
// stopPropagation flag at this layer — firing only the nearest handler IS the
// propagation contract.
TEST_CASE("click bubbles to the nearest ancestor handler only",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    int outer_clicks = 0;
    int mid_clicks = 0;

    View root;
    root.set_bounds({0, 0, 400, 300});
    root.on_click = [&] { ++outer_clicks; };

    auto mid = std::make_unique<View>();
    View* midp = mid.get();
    midp->set_bounds({50, 40, 200, 150});
    midp->on_click = [&] { ++mid_clicks; };
    root.add_child(std::move(mid));

    // A leaf with NO handler nested in mid; hit_test lands here, the click
    // bubbles up past it to `mid`.
    auto leaf = std::make_unique<View>();
    View* leafp = leaf.get();
    leafp->set_bounds({10, 10, 80, 60});
    midp->add_child(std::move(leaf));

    // Window point inside the leaf: root(50+10)+ .. → {70,60} lands in the leaf.
    root.simulate_click({70, 60});

    REQUIRE(mid_clicks == 1);     // nearest ancestor handler fires
    REQUIRE(outer_clicks == 0);   // the further ancestor does NOT also fire
    (void)leafp;
}

// ── 3b. The click bubble is bounded at the receiver ──────────────────────────
// simulate_click stops the on_click walk at the receiving view (inclusive),
// even when an ancestor OUTSIDE the receiver's subtree has a handler. This is
// the guard against synthetic clicks leaking across component boundaries when
// tooling drives an isolated subtree.
TEST_CASE("simulate_click does not bubble past the receiving view",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    int outer_clicks = 0;

    View root;
    root.set_bounds({0, 0, 400, 300});
    root.on_click = [&] { ++outer_clicks; };  // ancestor ABOVE the receiver

    auto mid = std::make_unique<View>();
    View* midp = mid.get();
    midp->set_bounds({50, 40, 200, 150});     // receiver — no handler
    root.add_child(std::move(mid));

    auto leaf = std::make_unique<View>();
    View* leafp = leaf.get();
    leafp->set_bounds({10, 10, 80, 60});      // no handler
    midp->add_child(std::move(leaf));

    // Drive the click on `mid` as the receiver; the point is mid-local so it
    // lands in the leaf. The bubble walks leaf → mid (the receiver) and stops,
    // never reaching root even though root has a handler.
    midp->simulate_click({20, 20});

    REQUIRE(outer_clicks == 0);
    (void)leafp;
}

// ── 4. Focus protocol: claim, guarded release, blur-on-swap ──────────────────
// Pins the process-global focus slot (View::focused_input_) and the visual
// focus flag (has_focus_) the hosts drive. S11 moves the slot to a root-owned
// struct behind these same call shapes; the observable results must not change.
TEST_CASE("input focus: claim sets the slot, swap blurs the previous holder",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    FocusSpy a;
    FocusSpy b;

    // Claiming focus for A points the global slot at A and raises its visual
    // focus (the host runs on_focus_changed(true) + claim_input_focus()).
    a.on_focus_changed(true);
    a.claim_input_focus();
    REQUIRE(View::focused_input_ == &a);
    REQUIRE(a.has_focus());

    // The host focus-swap idiom: blur the previous holder, then focus the new
    // one. After it, the slot points at B and A's visual focus is cleared.
    a.release_input_focus();
    a.on_focus_changed(false);
    b.on_focus_changed(true);
    b.claim_input_focus();
    REQUIRE(View::focused_input_ == &b);
    REQUIRE_FALSE(a.has_focus());
    REQUIRE(b.has_focus());
}

// ── 4b. release_input_focus only clears the slot when it holds it ────────────
// A non-holder calling release must NOT steal the slot away — the guard that
// stops one widget's teardown from blurring an unrelated focused widget.
TEST_CASE("release_input_focus is a no-op for a non-holder",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    FocusSpy a;
    FocusSpy b;

    a.claim_input_focus();
    REQUIRE(View::focused_input_ == &a);

    b.release_input_focus();               // B does not hold the slot
    REQUIRE(View::focused_input_ == &a);   // A still focused

    a.release_input_focus();               // the holder releases
    REQUIRE(View::focused_input_ == nullptr);
}

// ── 4c. Destroying the focused view auto-clears the slot (UAF guard) ──────────
// The exact contract that stops the host's keyDown path from dereferencing a
// freed widget after a React unmount. ~View() nulls focused_input_ when the
// dying view holds it.
TEST_CASE("destroying the focused view clears the global focus slot",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    {
        FocusSpy focused;
        focused.claim_input_focus();
        REQUIRE(View::focused_input_ == &focused);
    }  // focused destroyed here

    REQUIRE(View::focused_input_ == nullptr);
}

// ── 5. Overlay slot lifecycle: claim, dismiss-with-callback, auto-clear ───────
// The generalized overlay path (View::active_overlay_) the host consults before
// its normal hit_test so a popover over a sibling receives the click. Pins
// claim, the dismiss path (clears the slot BEFORE firing on_overlay_dismissed
// so a replacement popover claimed inside the callback survives), and the
// destructor auto-clear.
TEST_CASE("overlay slot: claim, dismiss fires the callback after clearing",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    View overlay;
    int dismissed = 0;
    View* slot_at_callback = reinterpret_cast<View*>(0x1);
    overlay.on_overlay_dismissed = [&] {
        ++dismissed;
        // The slot is cleared BEFORE the callback runs.
        slot_at_callback = View::active_overlay_;
    };

    overlay.claim_overlay();
    REQUIRE(View::active_overlay_ == &overlay);

    View::dismiss_active_overlay();
    REQUIRE(dismissed == 1);
    REQUIRE(slot_at_callback == nullptr);      // cleared before the callback
    REQUIRE(View::active_overlay_ == nullptr);

    // Dismissing again with no active overlay is a safe no-op.
    View::dismiss_active_overlay();
    REQUIRE(dismissed == 1);
}

TEST_CASE("destroying the overlay holder clears the overlay slot",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    {
        View overlay;
        overlay.claim_overlay();
        REQUIRE(View::active_overlay_ == &overlay);
    }  // overlay destroyed here

    REQUIRE(View::active_overlay_ == nullptr);
}

// ── 6. ComboBox popup lifecycle via the real click path ──────────────────────
// A click on a closed combo opens its dropdown and registers it as the single
// active popup; a click reported outside the popup (the host calls
// notify_global_click on every mouse-down) dismisses it. Opening a second combo
// closes the first — there is exactly one active popup process-wide, the slot
// S11 relocates to the root.
namespace {
// Deliver a left mouse-down to a widget through the real on_mouse_event path,
// the way the host does on press.
void press(View& v, Point local) {
    MouseEvent e;
    e.position = local;
    e.window_position = local;
    e.button = MouseButton::left;
    e.is_down = true;
    e.phase = MousePhase::press;
    v.on_mouse_event(e);
}
}  // namespace

TEST_CASE("ComboBox opens on click and registers as the single active popup",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    ComboBox combo;
    combo.set_bounds({0, 0, 160, 28});
    combo.set_items({"One", "Two", "Three", "Four"});

    REQUIRE_FALSE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == nullptr);

    // Click the header (closed → open).
    press(combo, {10, 14});
    REQUIRE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == &combo);

    // The host reports a click landing outside the popup subtree; the combo
    // dismisses. notify_global_click closes the active popup when the target is
    // not the popup or a descendant of it.
    View elsewhere;
    ComboBox::notify_global_click(&elsewhere);
    REQUIRE_FALSE(combo.is_open());
    REQUIRE(ComboBox::active_popup_ == nullptr);
}

TEST_CASE("opening a second ComboBox closes the first",
          "[view][input][hosting-smoke]") {
    reset_interaction_statics();

    ComboBox a;
    a.set_bounds({0, 0, 160, 28});
    a.set_items({"A1", "A2"});

    ComboBox b;
    b.set_bounds({0, 40, 160, 28});
    b.set_items({"B1", "B2"});

    press(a, {10, 14});
    REQUIRE(a.is_open());
    REQUIRE(ComboBox::active_popup_ == &a);

    // Opening B (via its own click) must close A — one active popup
    // process-wide. open_dropdown() closes any other open dropdown first.
    press(b, {10, 14});
    REQUIRE(b.is_open());
    REQUIRE_FALSE(a.is_open());
    REQUIRE(ComboBox::active_popup_ == &b);

    reset_interaction_statics();  // leave the slot clean for the next case
}
