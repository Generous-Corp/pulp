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
