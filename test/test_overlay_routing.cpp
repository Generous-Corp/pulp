// pulp #1148 — generalized overlay-click routing.
//
// Pins the per-View `View::active_overlay_` mechanism that the platform
// window host (window_host_mac.mm and platform siblings) consults
// AFTER `ComboBox::active_popup_` and BEFORE the regular tree
// `hit_test`. The ComboBox path is pinned separately by
// test_combo_dropdown.cpp [issue-overlay] — these tests cover the
// generic mechanism React popovers use.
//
// Contract under test:
//   1. claim_overlay() / release_overlay() toggle the global slot
//      and never null another holder.
//   2. View destructor releases the slot if it currently holds it.
//   3. overlay_contains() bounds-tests in window/root coordinates by
//      walking the parent chain (matches the mac mouseDown arithmetic).
//   4. release_overlay() is idempotent — safe to call when nothing
//      claimed the slot.
//
// The mac mouseDown integration itself is exercised end-to-end by
// the ComboBox regression test; for the per-View path we keep the
// invariants pure C++ so the test runs on every CI lane.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/overlay_dismissal.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <memory>

using pulp::view::View;
using pulp::view::Point;

namespace {

class TestView : public View {
public:
    void paint(pulp::canvas::Canvas&) override {}
};

// Reset global state — other tests in this binary may leave it set.
struct OverlayGuard {
    OverlayGuard() { View::active_overlay_ = nullptr; }
    ~OverlayGuard() { View::active_overlay_ = nullptr; }
};

}  // namespace

TEST_CASE("View::active_overlay_ defaults to nullptr [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::claim_overlay sets active_overlay_ to this [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    TestView v;
    v.claim_overlay();
    REQUIRE(View::active_overlay_ == &v);
}

TEST_CASE("View::release_overlay clears only if this holds it [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    TestView a;
    TestView b;

    a.claim_overlay();
    REQUIRE(View::active_overlay_ == &a);

    // Releasing a non-holder must NOT null a third party's slot.
    b.release_overlay();
    REQUIRE(View::active_overlay_ == &a);

    a.release_overlay();
    REQUIRE(View::active_overlay_ == nullptr);

    // Idempotent — safe to call again.
    a.release_overlay();
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::claim_overlay swaps the holder [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    TestView a, b;

    a.claim_overlay();
    REQUIRE(View::active_overlay_ == &a);

    // Mounting a second overlay supersedes the first — matches the
    // ComboBox::open_dropdown semantics where opening a second popup
    // closes the first.
    b.claim_overlay();
    REQUIRE(View::active_overlay_ == &b);
}

TEST_CASE("View destructor releases the overlay slot [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    {
        TestView v;
        v.claim_overlay();
        REQUIRE(View::active_overlay_ == &v);
    }
    // Without the dtor release, this would dangle and the next
    // mouseDown would dereference freed memory.
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("removing and destroying an overlay clears its former root slot",
          "[view][overlay][lifetime]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0, 0, 200, 200});
    auto sibling = std::make_unique<TestView>();
    sibling->set_bounds({100, 100, 50, 50});
    root.add_child(std::move(sibling));
    auto owned = std::make_unique<TestView>();
    auto* overlay = owned.get();
    overlay->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(owned));
    overlay->claim_overlay();
    REQUIRE(root.existing_interaction()->active_overlay == overlay);

    auto retired = root.remove_child(overlay);
    retired.reset();

    REQUIRE(root.existing_interaction()->active_overlay == nullptr);
    REQUIRE(route_press_to_active_overlay(root, {10, 10}).routing ==
            pulp::view::OverlayPressRouting::no_overlay);
}

TEST_CASE("View::overlay_contains uses absolute window coords [issue-1148]",
          "[view][overlay]") {
    OverlayGuard g;
    // Build a parent → child tree so the overlay's absolute origin is
    // offset from (0,0). The mac window-host walks the parent chain
    // identically; this asserts the helper matches that arithmetic.
    TestView parent;
    parent.set_bounds({100.0f, 50.0f, 400.0f, 400.0f});

    auto child_owned = std::make_unique<TestView>();
    auto* child = child_owned.get();
    child->set_bounds({20.0f, 30.0f, 80.0f, 60.0f});  // local to parent
    parent.add_child(std::move(child_owned));

    // Overlay's absolute window-rect is:
    //   x: 100 + 20 = 120
    //   y: 50  + 30 = 80
    //   w: 80, h: 60
    REQUIRE(child->overlay_contains({120.0f, 80.0f}));    // top-left corner
    REQUIRE(child->overlay_contains({199.0f, 139.0f}));   // bottom-right inside
    REQUIRE(child->overlay_contains({160.0f, 110.0f}));   // center

    REQUIRE_FALSE(child->overlay_contains({119.0f, 80.0f}));   // just left
    REQUIRE_FALSE(child->overlay_contains({120.0f, 79.0f}));   // just above
    REQUIRE_FALSE(child->overlay_contains({201.0f, 110.0f})); // right of
    REQUIRE_FALSE(child->overlay_contains({160.0f, 141.0f})); // below
}

// ── pulp #1320 — overlay_contains walks overflow:visible children ──────────
//
// Background (Spectr bands picker, v0.68.0 audit): a popover panel is built
// as `<View position="absolute"; right: 0>` inside a short trigger button's
// `position: relative` parent. The popover paints LEFTWARD beyond its
// parent's bounds. Before #1320, `overlay_contains` only tested the
// claiming View's own bounds, so clicks on the leftward cells of the
// popover failed `overlay_contains` and fell through to siblings.
//
// CSS `overflow:visible` semantics: a child painting outside its parent
// is still visible AND clickable. The fix expands `overlay_contains` to
// include the bounding box of all overflow:visible descendants (the
// painted union), matching the web rule.

TEST_CASE("View::overlay_contains includes overflow:visible child painted "
          "outside parent bounds [issue-1320]",
          "[view][overlay][1320]") {
    OverlayGuard g;
    // Simulate the Spectr bands picker layout:
    //   parent (relative wrapper):  bounds (200, 33, 60, 22)  ← short button
    //   child  (absolute popover):  bounds (-120, 28, 180, 30) ← extends LEFT
    //
    // The popover's window-coord rect is:
    //   x: 200 + (-120) = 80
    //   y: 33  + 28     = 61
    //   w: 180, h: 30
    //
    // A click at (100, 70) is INSIDE the popover but OUTSIDE the parent
    // (parent's right edge is 260, but 100 < 200 so the click is left of
    // the parent's left edge). The painted union of the parent plus
    // overflow:visible children must cover that pixel.
    TestView parent;
    parent.set_bounds({200.0f, 33.0f, 60.0f, 22.0f});
    parent.set_overflow(View::Overflow::visible);

    auto popover_owned = std::make_unique<TestView>();
    auto* popover = popover_owned.get();
    popover->set_bounds({-120.0f, 28.0f, 180.0f, 30.0f});
    popover->set_overflow(View::Overflow::visible);
    parent.add_child(std::move(popover_owned));

    parent.claim_overlay();
    REQUIRE(View::active_overlay_ == &parent);

    // Click on the leftward popover cell (the bug's "32" / "40" cells).
    REQUIRE(parent.overlay_contains({100.0f, 70.0f}));
    // Click in the middle of the popover.
    REQUIRE(parent.overlay_contains({170.0f, 70.0f}));
    // Click on the rightward portion (still inside popover, also inside parent).
    REQUIRE(parent.overlay_contains({250.0f, 70.0f}));
    // Click on the parent's own button area (regression — must still pass).
    REQUIRE(parent.overlay_contains({230.0f, 40.0f}));
    // Click clearly outside both parent AND popover — must still be false.
    REQUIRE_FALSE(parent.overlay_contains({50.0f, 70.0f}));    // left of popover
    REQUIRE_FALSE(parent.overlay_contains({270.0f, 70.0f}));   // right of popover
    REQUIRE_FALSE(parent.overlay_contains({170.0f, 100.0f})); // below popover
    REQUIRE_FALSE(parent.overlay_contains({170.0f, 30.0f})); // above (popover y=61, parent y=33; 30 is above both)
}

TEST_CASE("View::overlay_contains stops walking at overflow:hidden child "
          "[issue-1320]",
          "[view][overlay][1320]") {
    OverlayGuard g;
    // overflow:hidden clips its descendants — they don't contribute
    // painted pixels above us. Mirrors CSS rule.
    TestView parent;
    parent.set_bounds({100.0f, 100.0f, 50.0f, 50.0f});
    parent.set_overflow(View::Overflow::visible);

    auto clipped_owned = std::make_unique<TestView>();
    auto* clipped = clipped_owned.get();
    clipped->set_bounds({0.0f, 0.0f, 50.0f, 50.0f});
    clipped->set_overflow(View::Overflow::hidden);
    parent.add_child(std::move(clipped_owned));

    // Even if clipped has a wild child, we should NOT count it.
    auto wild_owned = std::make_unique<TestView>();
    auto* wild = wild_owned.get();
    wild->set_bounds({-1000.0f, -1000.0f, 100.0f, 100.0f});
    wild->set_overflow(View::Overflow::visible);
    clipped->add_child(std::move(wild_owned));
    (void)wild;

    parent.claim_overlay();
    // Inside parent — true.
    REQUIRE(parent.overlay_contains({120.0f, 120.0f}));
    // Inside the wild grandchild's painted rect — but it's clipped by
    // the overflow:hidden middle layer, so it must NOT contribute.
    REQUIRE_FALSE(parent.overlay_contains({-500.0f, -500.0f}));
}

TEST_CASE("View::overlay_contains: own overflow:hidden disables expansion "
          "[issue-1320]",
          "[view][overlay][1320]") {
    OverlayGuard g;
    // If the overlay View itself has overflow:hidden, its own painted
    // rect is the limit — children CAN'T paint outside it. Don't expand.
    TestView parent;
    parent.set_bounds({100.0f, 100.0f, 50.0f, 50.0f});
    parent.set_overflow(View::Overflow::hidden);

    auto child_owned = std::make_unique<TestView>();
    auto* child = child_owned.get();
    child->set_bounds({-100.0f, -100.0f, 50.0f, 50.0f});  // way outside
    parent.add_child(std::move(child_owned));
    (void)child;

    parent.claim_overlay();
    REQUIRE(parent.overlay_contains({120.0f, 120.0f}));     // inside parent
    REQUIRE_FALSE(parent.overlay_contains({0.0f, 0.0f})); // child's pixel — clipped
}

TEST_CASE("View overlay routing: ComboBox::active_popup_ is independent "
          "[issue-1148]",
          "[view][overlay][regression]") {
    // Belt-and-braces: the new generalized slot must not share storage
    // with ComboBox's existing active_popup_. The mac mouseDown order
    // checks ComboBox first, then this slot — if they collided the
    // ComboBox regression test would still pass while breaking React
    // popovers.
    OverlayGuard g;
    TestView v;
    v.claim_overlay();
    REQUIRE(View::active_overlay_ == &v);
    // ComboBox state is in its own static; not touched by claim_overlay.
}

// ── pulp #1361 — auto-dismissal (ESC + outside-click) ─────────────────────
//
// Before #1361, `active_overlay_` had no auto-dismissal: only React
// unmount or the View destructor cleared the slot. ESC and outside-click
// both failed silently. The fix adds:
//   - `View::dismiss_active_overlay()` — releases the slot AND fires
//     `on_overlay_dismissed` so React state can sync.
//   - Mac window host wires this into the ESC keypath and the
//     outside-click path (the latter previously called `release_overlay()`
//     which didn't fire the callback).
//
// These tests pin the View-level invariant. Mac-host integration is
// covered indirectly — we don't spin up an NSView in unit tests.

TEST_CASE("View::dismiss_active_overlay clears the slot [issue-1361]",
          "[view][overlay][1361]") {
    OverlayGuard g;
    TestView v;
    v.claim_overlay();
    REQUIRE(View::active_overlay_ == &v);

    View::dismiss_active_overlay();
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::dismiss_active_overlay fires on_overlay_dismissed callback "
          "[issue-1361]",
          "[view][overlay][1361]") {
    OverlayGuard g;
    TestView v;
    int dismiss_calls = 0;
    v.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };

    v.claim_overlay();
    View::dismiss_active_overlay();
    REQUIRE(dismiss_calls == 1);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::dismiss_active_overlay is a no-op when nothing is claimed "
          "[issue-1361]",
          "[view][overlay][1361]") {
    OverlayGuard g;
    TestView v;
    int dismiss_calls = 0;
    v.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };

    // Slot is empty — must not fire callback or crash.
    View::dismiss_active_overlay();
    REQUIRE(dismiss_calls == 0);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::release_overlay does NOT fire on_overlay_dismissed "
          "[issue-1361]",
          "[view][overlay][1361]") {
    // release_overlay() is the JSX-unmount / destructor path. React already
    // knows the popover is closing — firing on_overlay_dismissed there
    // would loop back into the JSX setOpen(false) handler unnecessarily.
    OverlayGuard g;
    TestView v;
    int dismiss_calls = 0;
    v.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };

    v.claim_overlay();
    v.release_overlay();
    REQUIRE(dismiss_calls == 0);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View destructor does NOT fire on_overlay_dismissed [issue-1361]",
          "[view][overlay][1361]") {
    // Same rationale as release_overlay — destructor is unmount-time, the
    // JS side already torn down the React component. Plus firing a JS
    // callback that touches the (now-destroyed) View would dangle.
    OverlayGuard g;
    int dismiss_calls = 0;
    {
        TestView v;
        v.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };
        v.claim_overlay();
    }  // dtor runs
    REQUIRE(dismiss_calls == 0);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("ESC dismisses active_overlay without disturbing other holders "
          "[issue-1361]",
          "[view][overlay][1361]") {
    // Sanity: the ESC path always calls dismiss_active_overlay() on the
    // current holder. A different View's claim must remain untouched
    // when not in the slot at the time of dismissal.
    OverlayGuard g;
    TestView a, b;
    int a_calls = 0, b_calls = 0;
    a.on_overlay_dismissed = [&a_calls]() { ++a_calls; };
    b.on_overlay_dismissed = [&b_calls]() { ++b_calls; };

    a.claim_overlay();
    REQUIRE(View::active_overlay_ == &a);

    // Simulate the ESC path.
    View::dismiss_active_overlay();
    REQUIRE(a_calls == 1);
    REQUIRE(b_calls == 0);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("Outside-click dismissal: dismiss_active_overlay is the "
          "release-and-notify path [issue-1361]",
          "[view][overlay][1361]") {
    // The mac mouseDown outside-click branch (window_host_mac.mm) used to
    // call `release_overlay()`, which silently cleared the slot without
    // notifying React. The fix routes that branch through
    // `dismiss_active_overlay()` — this test pins the contract:
    // the call SHOULD fire the dismiss callback so JS state can sync.
    OverlayGuard g;
    TestView popover;
    popover.set_bounds({100.0f, 100.0f, 200.0f, 150.0f});

    int dismiss_calls = 0;
    popover.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };
    popover.claim_overlay();

    // Verify the click really IS outside (mirrors the host's
    // overlay_contains() guard before calling dismiss_active_overlay).
    Point outside_pt{50.0f, 50.0f};
    REQUIRE_FALSE(popover.overlay_contains(outside_pt));

    // Host's outside-click branch calls dismiss_active_overlay().
    View::dismiss_active_overlay();
    REQUIRE(dismiss_calls == 1);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("Inside-click does NOT dismiss the overlay [issue-1361]",
          "[view][overlay][1361]") {
    // The host's mouseDown branch only calls dismiss_active_overlay()
    // when the click falls OUTSIDE the overlay. Inside clicks route
    // into the overlay's hit_test subtree and the slot stays claimed.
    // This test pins overlay_contains() returning true keeps the slot
    // intact — no call site invokes dismiss in that branch.
    OverlayGuard g;
    TestView popover;
    popover.set_bounds({100.0f, 100.0f, 200.0f, 150.0f});

    int dismiss_calls = 0;
    popover.on_overlay_dismissed = [&dismiss_calls]() { ++dismiss_calls; };
    popover.claim_overlay();

    // Click inside the popover bounds.
    Point inside_pt{200.0f, 175.0f};
    REQUIRE(popover.overlay_contains(inside_pt));

    // No dismiss call — the slot stays claimed for the next click.
    REQUIRE(View::active_overlay_ == &popover);
    REQUIRE(dismiss_calls == 0);
}

TEST_CASE("Pre-existing release_overlay path still works without callback "
          "[issue-1361][regression]",
          "[view][overlay][1361][regression]") {
    // Regression: legacy callers (the bridge's releaseOverlay JS function,
    // plus the View destructor) must still clear the slot via release_overlay
    // — that path is independent of the new dismiss callback.
    OverlayGuard g;
    TestView v;
    // No on_overlay_dismissed callback assigned at all.
    v.claim_overlay();
    REQUIRE(View::active_overlay_ == &v);
    v.release_overlay();
    REQUIRE(View::active_overlay_ == nullptr);
}

// ── Host press routing: route_press_to_active_overlay ──────────────────────
//
// The dismissal MECHANISM above was already fenced, but nothing fenced whether
// a platform host actually consults it on a press. The two mechanisms — the
// native ComboBox popup and this generalized slot — are wired per host, and
// they drifted: the standalone macOS host consulted both while the DAW plugin
// hosts consulted only the ComboBox one. A React / imported-design popover
// therefore stayed open forever when the user clicked outside it inside a
// plugin editor, while the identical UI dismissed correctly standalone.
//
// The decision now lives in one portable verb so it is testable without a
// window, and `tools/scripts/overlay_dismissal_wiring_guard.py` fences the
// hosts so one cannot be wired without the other.

TEST_CASE("route_press_to_active_overlay: no claim leaves the press alone",
          "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {150.0f, 150.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::no_overlay);
    REQUIRE(press.target == nullptr);
}

TEST_CASE("route_press_to_active_overlay: press inside routes into the overlay",
          "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {150.0f, 150.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::routed);
    REQUIRE(press.target == overlay);
    // Routing into an overlay must never dismiss it.
    REQUIRE(View::active_overlay_ == overlay);
}

// The regression that encodes the reported bug: open a dropdown, tap outside,
// it must close. Before the plugin hosts consulted this verb, the press simply
// never reached the dismissal mechanism and the dropdown stayed open.
TEST_CASE("route_press_to_active_overlay: press outside dismisses the overlay",
          "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    int dismissed_calls = 0;
    overlay->on_overlay_dismissed = [&] { ++dismissed_calls; };

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {500.0f, 500.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::dismissed);
    REQUIRE(press.target == nullptr);
    REQUIRE_FALSE(press.consume_press);
    REQUIRE(View::active_overlay_ == nullptr);
    // Routed through dismiss_active_overlay(), not a bare release_overlay(),
    // so React state can flip setOpen(false).
    REQUIRE(dismissed_calls == 1);
}

TEST_CASE("route_press_to_active_overlay preserves outside-click consumption",
          "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(true);

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {500.0f, 500.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::dismissed);
    REQUIRE(press.target == nullptr);
    REQUIRE(press.consume_press);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("View::simulate_click consumes a dismissing overlay press before the "
          "underlying control", "[view][overlay][pointer][consumption]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto underlying_owned = std::make_unique<TestView>();
    auto* underlying = underlying_owned.get();
    underlying->set_bounds({400.0f, 400.0f, 200.0f, 120.0f});
    int underlying_clicks = 0;
    underlying->on_click = [&] { ++underlying_clicks; };
    root.add_child(std::move(underlying_owned));

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    int dismissed_calls = 0;
    overlay->on_overlay_dismissed = [&] { ++dismissed_calls; };
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(true);

    root.simulate_click({500.0f, 500.0f});

    REQUIRE(dismissed_calls == 1);
    REQUIRE(underlying_clicks == 0);
    REQUIRE(View::active_overlay_ == nullptr);

    // Positive control: click-through remains available only when a caller
    // explicitly chooses the legacy non-consuming overlay policy.
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(false);
    root.simulate_click({500.0f, 500.0f});
    REQUIRE(dismissed_calls == 2);
    REQUIRE(underlying_clicks == 1);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("route_press_to_active_overlay: guards rejecting the point do not "
          "dismiss", "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    // Inside the overlay's rect, but the overlay opts out of hit-testing: it
    // is still mounted, just not interactive here, so the caller falls through
    // to the regular hit test WITHOUT closing it.
    overlay->set_hit_testable(false);

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {150.0f, 150.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::not_hittable);
    REQUIRE(press.target == nullptr);
    REQUIRE(View::active_overlay_ == overlay);
}

TEST_CASE("route_press_to_active_overlay: another tree's overlay is isolated",
          "[view][overlay][pointer]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    TestView other_root;
    other_root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto foreign_owned = std::make_unique<TestView>();
    auto* foreign = foreign_owned.get();
    foreign->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    other_root.add_child(std::move(foreign_owned));
    foreign->claim_overlay();

    const auto press = pulp::view::route_press_to_active_overlay(
        root, {150.0f, 150.0f});

    REQUIRE(press.routing == pulp::view::OverlayPressRouting::no_overlay);
    REQUIRE(View::active_overlay_ == foreign);
    REQUIRE(other_root.interaction().active_overlay == foreign);
}

// ── Dismiss-callback lifetime ────────────────────────────────────────────
//
// `on_overlay_dismissed` is a std::function whose storage lives inside the
// dismissed View. A React consumer flipping setOpen(false) unmounts the
// popover synchronously from inside that very callback, so the dismissing
// code must invoke a COPY: calling operator() on the member in place leaves
// std::function executing out of storage the callback just freed.
//
// `dismiss_claimed_overlay()` has always copied. The scope-taking
// `dismiss_active_overlay(View&)` did not, and it is the variant the DAW
// plugin hosts reach through `route_press_to_active_overlay`.
//
// The read after the unmount is what makes this a real use-after-free rather
// than a structural assertion: `sentinel` is a by-value capture, so it lives
// in the closure the destroyed View owned. Under a normal build this passes
// either way; under `-DPULP_SANITIZER=address` it reports
// heap-use-after-free without the copy.
TEST_CASE("dismiss_active_overlay(scope) survives a callback that unmounts the "
          "overlay", "[view][overlay][pointer][lifetime]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    int observed = 0;
    int calls = 0;
    int sentinel = 0x5A5A;
    overlay->on_overlay_dismissed = [&observed, &calls, &root, overlay,
                                     sentinel] {
        ++calls;
        // Unmount: the returned unique_ptr dies at the end of this full
        // expression, destroying the View that owns this closure.
        root.remove_child(overlay);
        observed = sentinel;  // reads the closure's own (now freed) storage
    };

    View::dismiss_active_overlay(root);

    REQUIRE(calls == 1);
    REQUIRE(observed == sentinel);
    REQUIRE(View::active_overlay_ == nullptr);
    REQUIRE(root.child_count() == 0);
}

// ── Context (right-button) press routing ─────────────────────────────────
//
// The right button reaches the underlay through a different host entry point
// than the left, and it did not honor `OverlayPressTarget::consume_press`:
// the dismissal happened, then the press fell through to `hit_test` and
// opened a context menu on the control beneath the popover. One right-click
// both closed the popover and mutated what was under it.
TEST_CASE("route_context_press consumes a dismissing overlay press before the "
          "underlying context menu",
          "[view][overlay][pointer][consumption]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto underlying_owned = std::make_unique<TestView>();
    auto* underlying = underlying_owned.get();
    underlying->set_bounds({400.0f, 400.0f, 200.0f, 120.0f});
    int underlying_menus = 0;
    underlying->on_context_menu = [&](pulp::view::Point) { ++underlying_menus; };
    root.add_child(std::move(underlying_owned));

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    int dismissed_calls = 0;
    overlay->on_overlay_dismissed = [&] { ++dismissed_calls; };
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(true);

    const auto consumed = pulp::view::route_context_press(root,
                                                          {500.0f, 500.0f});

    REQUIRE(dismissed_calls == 1);
    REQUIRE(consumed.overlay_dismissed);
    REQUIRE_FALSE(consumed.handled);
    REQUIRE(underlying_menus == 0);
    REQUIRE(View::active_overlay_ == nullptr);

    // Positive control, same instrument and same target: with the consumption
    // policy off, the identical press DOES reach the underlay's context menu.
    // Without it, `underlying_menus == 0` above would be ambiguous — it could
    // equally mean the menu was never reachable from this fixture at all.
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(false);
    const auto through = pulp::view::route_context_press(root,
                                                         {500.0f, 500.0f});

    REQUIRE(dismissed_calls == 2);
    REQUIRE(through.overlay_dismissed);
    REQUIRE(through.handled);
    REQUIRE(underlying_menus == 1);
    REQUIRE(View::active_overlay_ == nullptr);
}

TEST_CASE("route_context_press with no overlay dispatches to the hit target",
          "[view][overlay][pointer][consumption]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto target_owned = std::make_unique<TestView>();
    auto* target = target_owned.get();
    target->set_bounds({400.0f, 400.0f, 200.0f, 120.0f});
    int menus = 0;
    target->on_context_menu = [&](pulp::view::Point) { ++menus; };
    root.add_child(std::move(target_owned));

    const auto result = pulp::view::route_context_press(root, {500.0f, 500.0f});

    REQUIRE(result.handled);
    REQUIRE_FALSE(result.overlay_dismissed);
    REQUIRE(menus == 1);
}

TEST_CASE("route_context_press routes an inside press into the overlay subtree",
          "[view][overlay][pointer][consumption]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    int overlay_menus = 0;
    overlay->on_context_menu = [&](pulp::view::Point) { ++overlay_menus; };
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    overlay->set_overlay_consumes_outside_click(true);

    const auto result = pulp::view::route_context_press(root, {150.0f, 150.0f});

    REQUIRE(result.handled);
    REQUIRE_FALSE(result.overlay_dismissed);
    REQUIRE(overlay_menus == 1);
    // An inside press must not dismiss.
    REQUIRE(View::active_overlay_ == overlay);
}

// ── Host escape routing: route_escape_to_active_overlay ────────────────────
//
// Pressing Escape is a per-host obligation the same way a press is, and it
// drifted the same way: the standalone macOS host hand-rolled a three-step
// policy inline while both DAW plugin hosts and the web host had no Escape
// path at all. A plugin host hands the keyboard back to the DAW whenever
// nothing in its tree holds focus — the ordinary state while a popover is
// open — so a `<View overlay>` popover inside a plugin editor could not be
// closed from the keyboard. These pin the one shared policy the hosts call.

TEST_CASE("route_escape_to_active_overlay: nothing open reports none",
          "[view][overlay][escape]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    // A parented child, so this is a real tree with its own interaction slot
    // rather than a detached widget resolving to the process-global fallback.
    auto child = std::make_unique<TestView>();
    child->set_bounds({10.0f, 10.0f, 40.0f, 40.0f});
    root.add_child(std::move(child));

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::none);
    // A keystroke that finds nothing must not allocate interaction state onto
    // a tree that never claimed any.
    REQUIRE(root.existing_interaction() == nullptr);
}

TEST_CASE("route_escape_to_active_overlay dismisses a claimed overlay",
          "[view][overlay][escape]") {
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    int dismissed_calls = 0;
    overlay->on_overlay_dismissed = [&] { ++dismissed_calls; };

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::overlay);
    REQUIRE(root.interaction().active_overlay == nullptr);
    // Routed through the dismissal path, not a bare release, so React state
    // can flip setOpen(false).
    REQUIRE(dismissed_calls == 1);
}

TEST_CASE("route_escape_to_active_overlay only acts on its own tree",
          "[view][overlay][escape]") {
    // Two Pulp editors in one host process (the shared AUHostingService case).
    // Escape in editor B must not close editor A's popover, which is exactly
    // what reading the process-global shim mirror would do — A claimed most
    // recently, so the mirror names A's overlay.
    OverlayGuard g;
    TestView root_a, root_b;
    root_a.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    root_b.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root_a.add_child(std::move(overlay_owned));
    overlay->claim_overlay();
    REQUIRE(View::active_overlay_ == overlay);

    int dismissed_calls = 0;
    overlay->on_overlay_dismissed = [&] { ++dismissed_calls; };

    REQUIRE(pulp::view::route_escape_to_active_overlay(root_b) ==
            pulp::view::OverlayEscapeResult::none);
    REQUIRE(root_a.interaction().active_overlay == overlay);
    REQUIRE(dismissed_calls == 0);

    REQUIRE(pulp::view::route_escape_to_active_overlay(root_a) ==
            pulp::view::OverlayEscapeResult::overlay);
    REQUIRE(dismissed_calls == 1);
}

TEST_CASE("route_escape_to_active_overlay closes a modal before the overlay",
          "[view][overlay][escape]") {
    // A modal traps interaction, so nothing behind it may act on the key. It
    // is found by tree walk rather than by focus: a modal that never took
    // focus still owns the screen, and in a plugin host the DAW may hold
    // focus entirely.
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    auto modal_owned = std::make_unique<pulp::view::ModalOverlay>();
    auto* modal = modal_owned.get();
    modal->set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    int modal_dismissals = 0;
    modal->on_dismiss = [&] { ++modal_dismissals; };
    root.add_child(std::move(modal_owned));

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::modal);
    REQUIRE(modal_dismissals == 1);
    // The overlay behind the modal is untouched.
    REQUIRE(root.interaction().active_overlay == overlay);
}

TEST_CASE("route_escape_to_active_overlay skips a hidden modal",
          "[view][overlay][escape]") {
    // A modal that is not on screen must not eat the key, or a popover opened
    // after a modal was hidden becomes undismissable.
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto modal_owned = std::make_unique<pulp::view::ModalOverlay>();
    auto* modal = modal_owned.get();
    modal->set_bounds({0.0f, 0.0f, 800.0f, 600.0f});
    int modal_dismissals = 0;
    modal->on_dismiss = [&] { ++modal_dismissals; };
    root.add_child(std::move(modal_owned));
    modal->set_visible(false);

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({100.0f, 100.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::overlay);
    REQUIRE(modal_dismissals == 0);
    REQUIRE(root.interaction().active_overlay == nullptr);
}

TEST_CASE("route_escape_to_active_overlay closes an open ComboBox dropdown",
          "[view][overlay][escape]") {
    // ComboBox::on_key_event fires only while the combo owns focus, which a
    // sibling React popover routinely steals; without this host-level
    // fallback the still-visible dropdown wedges open with no keyboard
    // escape route.
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto combo_owned = std::make_unique<pulp::view::ComboBox>();
    auto* combo = combo_owned.get();
    combo->set_bounds({10.0f, 10.0f, 160.0f, 24.0f});
    combo->set_items({"One", "Two", "Three"});
    root.add_child(std::move(combo_owned));

    pulp::view::MouseEvent open_click;
    open_click.position = {60.0f, 12.0f};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::combo_popup);
    REQUIRE_FALSE(combo->is_open());
}

TEST_CASE("route_escape_to_active_overlay closes a dropdown before an overlay",
          "[view][overlay][escape]") {
    // Ordering regression: with both open, one Escape closes the dropdown and
    // leaves the popover, so a second Escape is needed to close that. Closing
    // both at once would surprise a user backing out of a nested menu.
    OverlayGuard g;
    TestView root;
    root.set_bounds({0.0f, 0.0f, 800.0f, 600.0f});

    auto combo_owned = std::make_unique<pulp::view::ComboBox>();
    auto* combo = combo_owned.get();
    combo->set_bounds({10.0f, 10.0f, 160.0f, 24.0f});
    combo->set_items({"One", "Two", "Three"});
    root.add_child(std::move(combo_owned));

    auto overlay_owned = std::make_unique<TestView>();
    auto* overlay = overlay_owned.get();
    overlay->set_bounds({300.0f, 300.0f, 200.0f, 120.0f});
    root.add_child(std::move(overlay_owned));
    overlay->claim_overlay();

    pulp::view::MouseEvent open_click;
    open_click.position = {60.0f, 12.0f};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);
    REQUIRE(combo->is_open());

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::combo_popup);
    REQUIRE(root.interaction().active_overlay == overlay);

    REQUIRE(pulp::view::route_escape_to_active_overlay(root) ==
            pulp::view::OverlayEscapeResult::overlay);
    REQUIRE(root.interaction().active_overlay == nullptr);
}
