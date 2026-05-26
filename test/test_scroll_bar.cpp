// ScrollBar widget tests (pulp #6.3 macos plugin-authoring plan).
//
// Covers the four acceptance scenarios from the planning doc:
//   - drag the thumb,
//   - arrow keys (up/down/left/right) step by arrow_step,
//   - page up / page down step by page_step,
//   - home / end snap to min / max,
//   - track click pages toward the cursor,
//   - thumb sizing is proportional to page_size / range,
//   - on_change fires only on real value transitions.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/view/scroll_bar.hpp>
#include <pulp/view/input_events.hpp>

using namespace pulp::view;

namespace {

KeyEvent down(KeyCode k) {
    KeyEvent ev;
    ev.key = k;
    ev.is_down = true;
    return ev;
}

MouseEvent press(Point p) {
    MouseEvent ev;
    ev.position = p;
    ev.is_down = true;
    return ev;
}

MouseEvent release(Point p) {
    MouseEvent ev;
    ev.position = p;
    ev.is_down = false;
    return ev;
}

} // namespace

TEST_CASE("ScrollBar: defaults are usable out of the box", "[scroll-bar]") {
    ScrollBar sb;
    REQUIRE(sb.orientation() == ScrollBar::Orientation::vertical);
    REQUIRE(sb.min_value() == Catch::Approx(0.0f));
    REQUIRE(sb.max_value() == Catch::Approx(1.0f));
    REQUIRE(sb.value() == Catch::Approx(0.0f));
    REQUIRE(sb.focusable());
}

TEST_CASE("ScrollBar: set_range clamps existing value", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_range(0, 100);
    sb.set_value(75);
    REQUIRE(sb.value() == Catch::Approx(75.0f));
    // Tighten the range — value should clamp into the new max.
    sb.set_range(0, 50);
    REQUIRE(sb.value() == Catch::Approx(50.0f));
}

TEST_CASE("ScrollBar: arrow keys step by arrow_step", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_range(0, 10);
    sb.set_arrow_step(1.0f);

    REQUIRE(sb.on_key_event(down(KeyCode::down)));
    REQUIRE(sb.value() == Catch::Approx(1.0f));
    REQUIRE(sb.on_key_event(down(KeyCode::right)));
    REQUIRE(sb.value() == Catch::Approx(2.0f));
    REQUIRE(sb.on_key_event(down(KeyCode::up)));
    REQUIRE(sb.value() == Catch::Approx(1.0f));
    REQUIRE(sb.on_key_event(down(KeyCode::left)));
    REQUIRE(sb.value() == Catch::Approx(0.0f));
    // Already at min — additional left should clamp, not underflow.
    REQUIRE(sb.on_key_event(down(KeyCode::left)));
    REQUIRE(sb.value() == Catch::Approx(0.0f));
}

TEST_CASE("ScrollBar: page up/down step by page_step", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_range(0, 100);
    sb.set_page_step(25.0f);
    sb.set_value(50);

    REQUIRE(sb.on_key_event(down(KeyCode::page_down)));
    REQUIRE(sb.value() == Catch::Approx(75.0f));
    REQUIRE(sb.on_key_event(down(KeyCode::page_up)));
    REQUIRE(sb.value() == Catch::Approx(50.0f));
}

TEST_CASE("ScrollBar: home/end snap to bounds", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_range(10, 90);
    sb.set_value(50);

    REQUIRE(sb.on_key_event(down(KeyCode::home)));
    REQUIRE(sb.value() == Catch::Approx(10.0f));
    REQUIRE(sb.on_key_event(down(KeyCode::end_)));
    REQUIRE(sb.value() == Catch::Approx(90.0f));
}

TEST_CASE("ScrollBar: unrelated keys are not consumed", "[scroll-bar]") {
    ScrollBar sb;
    REQUIRE_FALSE(sb.on_key_event(down(KeyCode::a)));
    REQUIRE_FALSE(sb.on_key_event(down(KeyCode::escape)));
    // Key-up of a navigation key is not consumed either (only key-down acts).
    KeyEvent up_left;
    up_left.key = KeyCode::left;
    up_left.is_down = false;
    REQUIRE_FALSE(sb.on_key_event(up_left));
}

TEST_CASE("ScrollBar: drag updates value proportionally (vertical)",
          "[scroll-bar]") {
    ScrollBar sb;
    sb.set_bounds({0, 0, 16, 200});
    sb.set_range(0, 100);
    sb.set_page_size(20.0f);

    // Thumb starts at the top. Grab it and drag toward the bottom.
    const float thumb_lead = sb.thumb_pixel_offset() + /*arrow*/ 16.0f;
    sb.on_mouse_event(press({8, thumb_lead + 1}));
    // Drag down by 80 px — should move ~halfway down the usable track.
    sb.on_mouse_drag({8, thumb_lead + 1 + 80});
    REQUIRE(sb.value() > 30.0f);
    REQUIRE(sb.value() < 80.0f);
    sb.on_mouse_event(release({8, thumb_lead + 1 + 80}));
}

TEST_CASE("ScrollBar: drag (horizontal) tracks x-axis", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_orientation(ScrollBar::Orientation::horizontal);
    sb.set_bounds({0, 0, 200, 16});
    sb.set_range(0, 100);

    const float thumb_lead = sb.thumb_pixel_offset() + 16.0f;
    sb.on_mouse_event(press({thumb_lead + 1, 8}));
    sb.on_mouse_drag({thumb_lead + 80, 8});
    REQUIRE(sb.value() > 30.0f);
    REQUIRE(sb.value() < 80.0f);
}

TEST_CASE("ScrollBar: track click pages toward the cursor", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_bounds({0, 0, 16, 200});
    sb.set_range(0, 100);
    sb.set_page_step(20.0f);
    sb.set_value(50);  // thumb sits in the middle

    // Click far below the thumb → page forward.
    sb.on_mouse_event(press({8, 190}));
    REQUIRE(sb.value() == Catch::Approx(70.0f));
    sb.on_mouse_event(release({8, 190}));

    // Click far above the thumb → page backward.
    sb.on_mouse_event(press({8, 5}));
    REQUIRE(sb.value() == Catch::Approx(50.0f));
}

TEST_CASE("ScrollBar: thumb size scales with page_size / range",
          "[scroll-bar]") {
    ScrollBar sb;
    sb.set_bounds({0, 0, 16, 200});
    sb.set_range(0, 100);

    sb.set_page_size(10.0f);
    const float small = sb.thumb_pixel_length();
    sb.set_page_size(50.0f);
    const float big = sb.thumb_pixel_length();
    REQUIRE(big > small);
    // Floor at 16px so the thumb stays grabbable.
    REQUIRE(small >= 16.0f);
}

TEST_CASE("ScrollBar: on_change fires on real transitions only",
          "[scroll-bar]") {
    ScrollBar sb;
    sb.set_range(0, 10);
    int callbacks = 0;
    float last = -1.0f;
    sb.on_change = [&](float v) { ++callbacks; last = v; };

    REQUIRE(sb.set_value(5));    // changed → fires
    REQUIRE_FALSE(sb.set_value(5));  // no-op → no fire
    REQUIRE(callbacks == 1);
    REQUIRE(last == Catch::Approx(5.0f));

    // Clamping: requesting > max stores the clamped max, fires once.
    REQUIRE(sb.set_value(100));
    REQUIRE(callbacks == 2);
    REQUIRE(sb.value() == Catch::Approx(10.0f));
    REQUIRE_FALSE(sb.set_value(100));  // already clamped → no-op
    REQUIRE(callbacks == 2);
}

TEST_CASE("ScrollBar: empty range degrades gracefully", "[scroll-bar]") {
    ScrollBar sb;
    sb.set_bounds({0, 0, 16, 200});
    sb.set_range(5, 5);
    // Range collapsed — paint/thumb math must not divide by zero.
    REQUIRE(sb.thumb_pixel_length() > 0.0f);
    REQUIRE(sb.thumb_pixel_offset() == Catch::Approx(0.0f));
    // Drag should not crash and value stays put.
    sb.on_mouse_event(press({8, 100}));
    sb.on_mouse_drag({8, 150});
    REQUIRE(sb.value() == Catch::Approx(5.0f));
}
