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
