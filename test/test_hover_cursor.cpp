// Hover-cursor tracking: the cursor must follow the content, not only the
// pointer. Platform cursor APIs are edge-driven — they re-ask which cursor to
// show when the pointer moves — so the tracker exists to re-resolve from a
// frame path while the pointer is perfectly still.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/hover_cursor.hpp>
#include <pulp/view/view.hpp>

using pulp::view::HoverCursorTracker;
using pulp::view::Point;
using pulp::view::View;

namespace {

struct StubView : View {
    void paint(pulp::canvas::Canvas&) override {}
};

}  // namespace

TEST_CASE("hover_cursor_at resolves the hit view's style", "[view][cursor]") {
    StubView root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<StubView>();
    child->set_bounds({0, 0, 100, 100});
    child->set_cursor(View::CursorStyle::pointer);
    root.add_child(std::move(child));

    REQUIRE(pulp::view::hover_cursor_at(root, {50, 50}) == View::CursorStyle::pointer);
    REQUIRE(pulp::view::hover_cursor_at(root, {150, 150}) == View::CursorStyle::default_);
}

TEST_CASE("a stationary pointer sees the cursor change when the content under it changes",
          "[view][cursor]") {
    StubView root;
    root.set_bounds({0, 0, 200, 200});

    // Two siblings. Only `left` is under the pointer to begin with.
    auto left_owned = std::make_unique<StubView>();
    left_owned->set_bounds({0, 0, 100, 200});
    left_owned->set_cursor(View::CursorStyle::pointer);
    auto* left = left_owned.get();
    root.add_child(std::move(left_owned));

    auto right_owned = std::make_unique<StubView>();
    right_owned->set_bounds({100, 0, 100, 200});
    right_owned->set_cursor(View::CursorStyle::text);
    auto* right = right_owned.get();
    root.add_child(std::move(right_owned));

    HoverCursorTracker tracker;
    const Point stationary{50, 100};
    tracker.set_pointer(stationary);

    // First resolve publishes the style under the pointer.
    auto first = tracker.poll(root);
    REQUIRE(first.has_value());
    REQUIRE(*first == View::CursorStyle::pointer);

    // Polling again with nothing changed must not re-publish: the platform call
    // happens on change, not once per frame.
    REQUIRE_FALSE(tracker.poll(root).has_value());

    // A layout pass slides the other view under the SAME point. No mouse event,
    // no new coordinates — the pointer never moved.
    left->set_bounds({-100, 0, 100, 200});
    right->set_bounds({0, 0, 200, 200});

    auto second = tracker.poll(root);
    REQUIRE(second.has_value());
    REQUIRE(*second == View::CursorStyle::text);
    REQUIRE(tracker.published() == View::CursorStyle::text);
}

TEST_CASE("the tracker only resolves while a pointer is over the surface",
          "[view][cursor]") {
    StubView root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<StubView>();
    child->set_bounds({0, 0, 200, 200});
    child->set_cursor(View::CursorStyle::crosshair);
    root.add_child(std::move(child));

    HoverCursorTracker tracker;
    REQUIRE_FALSE(tracker.has_pointer());
    REQUIRE_FALSE(tracker.poll(root).has_value());

    tracker.set_pointer({10, 10});
    REQUIRE(tracker.poll(root).has_value());

    // Leaving drops the published style, so returning to an unchanged tree
    // still publishes rather than assuming the platform kept it.
    tracker.clear_pointer();
    REQUIRE_FALSE(tracker.poll(root).has_value());
    tracker.set_pointer({10, 10});
    auto again = tracker.poll(root);
    REQUIRE(again.has_value());
    REQUIRE(*again == View::CursorStyle::crosshair);
}

TEST_CASE("a host-resolved style routes through the same change gate",
          "[view][cursor]") {
    HoverCursorTracker tracker;
    // Hosts that layer an overlay/inspector override on top of the hit-test
    // answer resolve the final style themselves and publish through here.
    auto first = tracker.poll_resolved(View::CursorStyle::grab);
    REQUIRE(first.has_value());
    REQUIRE(*first == View::CursorStyle::grab);
    REQUIRE_FALSE(tracker.poll_resolved(View::CursorStyle::grab).has_value());
    REQUIRE(tracker.poll_resolved(View::CursorStyle::grabbing).has_value());
}
