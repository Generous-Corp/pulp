// test_pointer_coordinate_mapping.cpp — root -> local coordinate conversion.
//
// Split out of test_pointer_dispatch.cpp (WAH-7). `point_to_local` is the one
// piece of pointer handling that is pure arithmetic over the view tree —
// ancestor offsets, ScrollView scroll, and ancestor scale — so it is worth
// isolating from anything that dispatches an event.
//
// These cases were previously inlined in the macOS hosts, so a regression could
// only be caught by clicking a real NSView.

// Right-click routing and root→local coordinate conversion.
//
// These were previously inlined in the macOS hosts, so a regression could only
// be caught by clicking a real NSView. The plugin host in particular had no
// right-button path at all, which left every in-DAW context menu dead.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/view/gesture.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include <string>
#include <vector>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;


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
