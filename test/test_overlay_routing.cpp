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
#include <pulp/view/view.hpp>

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
    // the parent's left edge). Pre-fix: overlay_contains returns false.
    // Post-fix: overlay_contains returns true because the painted union
    // of the parent + overflow:visible children covers that pixel.
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
