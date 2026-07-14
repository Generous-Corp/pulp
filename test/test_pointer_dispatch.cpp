// Right-click routing and root→local coordinate conversion.
//
// These were previously inlined in the macOS hosts, so a regression could only
// be caught by clicking a real NSView. The plugin host in particular had no
// right-button path at all, which left every in-DAW context menu dead.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

// A view that records where a context-menu click landed, in its own local space.
class ContextSpy : public View {
public:
    ContextSpy() {
        on_context_menu = [this](Point p) {
            ++hits;
            last = p;
        };
    }
    int hits = 0;
    Point last{};
};

}  // namespace

TEST_CASE("dispatch_context_menu routes a right-click to the view under it", "[view][input]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<ContextSpy>();
    ContextSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    SECTION("inside the child: handler fires with child-local coordinates") {
        REQUIRE(dispatch_context_menu(root, {130, 70}));
        REQUIRE(spy->hits == 1);
        CHECK_THAT(spy->last.x, WithinAbs(30.0, 1e-4));  // 130 - 100
        CHECK_THAT(spy->last.y, WithinAbs(20.0, 1e-4));  // 70  - 50
    }

    SECTION("outside the child: no handler on the root, so nothing is consumed") {
        REQUIRE_FALSE(dispatch_context_menu(root, {10, 10}));
        REQUIRE(spy->hits == 0);
    }
}

TEST_CASE("dispatch_context_menu reports not-consumed without a handler", "[view][input]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    // No on_context_menu anywhere: the host must fall through to its own menu.
    REQUIRE_FALSE(dispatch_context_menu(root, {50, 50}));
}

TEST_CASE("point_to_local peels ancestor offsets", "[view][input]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto mid = std::make_unique<View>();
    View* midp = mid.get();
    midp->set_bounds({50, 30, 300, 300});
    root.add_child(std::move(mid));

    auto leaf = std::make_unique<View>();
    View* leafp = leaf.get();
    leafp->set_bounds({20, 10, 100, 100});
    midp->add_child(std::move(leaf));

    const Point local = point_to_local({100, 70}, leafp, &root);
    CHECK_THAT(local.x, WithinAbs(30.0, 1e-4));  // 100 - 50 - 20
    CHECK_THAT(local.y, WithinAbs(30.0, 1e-4));  // 70  - 30 - 10
}

TEST_CASE("point_to_local undoes a scrolled ScrollView", "[view][input]") {
    // A ScrollView paints children shifted by -scroll and its hit_test adds
    // +scroll back; the local coordinate must agree with the target hit_test found.
    ScrollView root;
    root.set_bounds({0, 0, 200, 200});
    // set_scroll clamps against content_size, which is otherwise zero here.
    root.set_content_size({200, 500});

    auto child = std::make_unique<View>();
    View* childp = child.get();
    childp->set_bounds({0, 100, 200, 400});
    root.add_child(std::move(child));

    const Point unscrolled = point_to_local({10, 120}, childp, &root);
    CHECK_THAT(unscrolled.y, WithinAbs(20.0, 1e-4));  // 120 - 100

    // Scroll down, then click the same content point. Its window-space y moves up
    // by the scroll offset, but the child-local y must be unchanged. Read the
    // applied offset back rather than assuming it, since set_scroll clamps to
    // the content extent.
    root.set_scroll(0.0f, 60.0f);
    const float sy = root.scroll_y();
    REQUIRE(sy > 0.0f);
    const Point scrolled = point_to_local({10, 120 - sy}, childp, &root);
    CHECK_THAT(scrolled.y, WithinAbs(20.0, 1e-4));
}

TEST_CASE("point_to_local peels a non-root ScrollView ancestor", "[view][input]") {
    // A ScrollView nested below the root paints its children shifted by -scroll;
    // the inverse walk must add its scroll offset back inside the ancestor loop,
    // scaled by the chain above it. This is distinct from the root-ScrollView
    // branch, which is handled before the loop.
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto mid = std::make_unique<ScrollView>();
    ScrollView* midp = mid.get();
    midp->set_bounds({50, 30, 200, 200});
    // set_scroll clamps against content_size, which is otherwise zero here.
    midp->set_content_size({400, 500});
    root.add_child(std::move(mid));

    auto child = std::make_unique<View>();
    View* childp = child.get();
    childp->set_bounds({0, 100, 200, 400});
    midp->add_child(std::move(child));

    // Read the applied offsets back rather than assuming them: set_scroll clamps
    // to (content - bounds), here 200 in x and 300 in y, so 40/60 both apply.
    midp->set_scroll(40.0f, 60.0f);
    const float sx = midp->scroll_x();
    const float sy = midp->scroll_y();
    REQUIRE(sx > 0.0f);
    REQUIRE(sy > 0.0f);

    const Point local = point_to_local({10, 120}, childp, &root);
    // scale chain is 1.0 throughout, so no final divide.
    CHECK_THAT(local.x, WithinAbs(10.0 - 50.0 + sx, 1e-4));         // px - mid.x + scroll_x - child.x(0)
    CHECK_THAT(local.y, WithinAbs(120.0 - 30.0 - 100.0 + sy, 1e-4)); // py - mid.y - child.y + scroll_y
}

TEST_CASE("point_to_local divides out a scaled ancestor", "[view][input]") {
    // When an ancestor has set_scale != 1.0, its descendants are painted magnified.
    // The forward model is:
    //   visual = mid.bounds*1 + leaf.bounds*mid.scale + target_local*mid.scale
    // so the inverse peels mid.bounds at chain=1, leaf.bounds at chain=mid.scale,
    // then divides the residual by the accumulated chain.
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto mid = std::make_unique<View>();
    View* midp = mid.get();
    midp->set_bounds({50, 30, 300, 300});
    midp->set_scale(2.0f);
    root.add_child(std::move(mid));

    auto leaf = std::make_unique<View>();
    View* leafp = leaf.get();
    leafp->set_bounds({20, 10, 100, 100});
    midp->add_child(std::move(leaf));

    // visual for target_local (20,20): (50,30) + (20,10)*2 + (20,20)*2 = (130,90).
    const Point local = point_to_local({130, 90}, leafp, &root);
    CHECK_THAT(local.x, WithinAbs(20.0, 1e-4));  // (130 - 50 - 20*2) / 2
    CHECK_THAT(local.y, WithinAbs(20.0, 1e-4));  // (90  - 30 - 10*2) / 2
}

// ── Drag delivery on the modern event channel (deliver_mouse_drag) ───────────
//
// Before this existed, both macOS hosts delivered `press` and `release` on BOTH
// the modern `on_mouse_event` channel and the legacy `on_mouse_*` callbacks, but
// delivered `drag` on the LEGACY channel only — and the legacy drag callback
// carries nothing but a Point. A view could therefore see modifier keys on the
// press and on the release of a gesture, but never DURING it, so "hold Shift to
// fine-adjust while dragging" — the most common plug-in knob idiom there is —
// could not be written at all.

namespace {

// Records every channel a pointer phase arrives on, in arrival order, so both
// the exactly-once property and the modern-before-legacy ordering are asserted.
class DragSpy : public View {
public:
    DragSpy() {
        on_drag = [this](Point p) {
            log.push_back("on_drag");
            last_on_drag = p;
        };
    }

    void on_mouse_event(const MouseEvent& e) override {
        log.push_back("on_mouse_event");
        events.push_back(e);
    }
    void on_mouse_drag(Point p) override {
        log.push_back("on_mouse_drag");
        last_legacy = p;
    }

    std::vector<std::string> log;
    std::vector<MouseEvent> events;
    Point last_legacy{};
    Point last_on_drag{};
};

}  // namespace

TEST_CASE("deliver_mouse_drag carries modifiers on the modern channel", "[view][input][drag]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<DragSpy>();
    DragSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    deliver_mouse_drag(root, spy, {130, 70}, kModShift | kModCmd, /*click_count=*/1);

    REQUIRE(spy->events.size() == 1);
    const MouseEvent& e = spy->events.front();
    CHECK(e.phase == MousePhase::drag);
    CHECK(e.is_down);                       // the button is still held mid-drag
    CHECK(e.isShiftDown());                 // <-- the wire: modifiers DURING a drag
    CHECK((e.modifiers & kModCmd) != 0);
    CHECK(e.button == MouseButton::left);
    CHECK(e.click_count == 1);
    // Local to the spy, window-space preserved for the host.
    CHECK_THAT(e.position.x, WithinAbs(30.0f, 0.01f));
    CHECK_THAT(e.position.y, WithinAbs(20.0f, 0.01f));
    CHECK_THAT(e.window_position.x, WithinAbs(130.0f, 0.01f));
}

TEST_CASE("deliver_mouse_drag fires each channel exactly once, modern first",
          "[view][input][drag]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<DragSpy>();
    DragSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    deliver_mouse_drag(root, spy, {130, 70}, 0);

    // Exactly once per channel, in the documented order (pointer_dispatch.hpp).
    REQUIRE(spy->log == std::vector<std::string>{"on_mouse_event", "on_mouse_drag", "on_drag"});
    // The legacy channels still receive the same local point they always did.
    CHECK_THAT(spy->last_legacy.x, WithinAbs(30.0f, 0.01f));
    CHECK_THAT(spy->last_on_drag.y, WithinAbs(20.0f, 0.01f));
}

TEST_CASE("deliver_mouse_drag bubbles on_drag to ancestors in their own space",
          "[view][input][drag]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto wrapper = std::make_unique<View>();
    View* wrap = wrapper.get();
    wrap->set_bounds({100, 50, 200, 200});
    int wrap_hits = 0;
    Point wrap_pt{};
    wrap->on_drag = [&](Point p) { ++wrap_hits; wrap_pt = p; };

    auto child = std::make_unique<DragSpy>();
    DragSpy* spy = child.get();
    spy->set_bounds({10, 10, 50, 50});      // (110,60) in root space
    wrap->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    deliver_mouse_drag(root, spy, {130, 70}, 0);

    // The inner presentational widget is the hit target; the drag handler lives
    // on the outer wrapper. Both get exactly one call, each in its own space.
    CHECK(wrap_hits == 1);
    CHECK_THAT(wrap_pt.x, WithinAbs(30.0f, 0.01f));   // 130 - 100
    CHECK_THAT(spy->last_on_drag.x, WithinAbs(20.0f, 0.01f));  // 130 - 100 - 10
}

TEST_CASE("deliver_mouse_drag is inert for a target that left the tree",
          "[view][input][drag]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<DragSpy>();
    DragSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    auto detached = root.remove_child(spy);   // unmounted mid-gesture
    deliver_mouse_drag(root, spy, {130, 70}, kModShift);
    CHECK(spy->log.empty());                  // no channel fires; no deref hazard
}
