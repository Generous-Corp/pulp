#include <catch2/catch_test_macros.hpp>

#include <pulp/view/continuous_frames.hpp>
#include <pulp/view/gesture.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

namespace {

MouseEvent pointer_event(Point p, MousePhase phase, double* t, double advance = 0.05) {
    *t += advance;
    MouseEvent event;
    event.position = p;
    event.window_position = p;
    event.button = MouseButton::left;
    event.is_down = phase != MousePhase::release;
    event.phase = phase;
    return event;
}

MouseEvent touch_event(Point p, MousePhase phase, int pointer_id,
                       double* t, double advance = 0.05) {
    MouseEvent event = pointer_event(p, phase, t, advance);
    event.pointer_id = pointer_id;
    event.pointer_type = PointerType::touch;
    return event;
}

View& add_child(View& parent, Rect bounds) {
    auto child = std::make_unique<View>();
    child->set_bounds(bounds);
    auto& ref = *child;
    parent.add_child(std::move(child));
    return ref;
}

}  // namespace

TEST_CASE("PanRecognizer claims capture after movement crosses slop",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(4.0f);
    int began = 0;
    int changed = 0;
    int ended = 0;
    pan->on_began = [&](GestureRecognizer&) { ++began; };
    pan->on_changed = [&](GestureRecognizer&) { ++changed; };
    pan->on_ended = [&](GestureRecognizer&) { ++ended; };
    auto& pan_ref = static_cast<PanRecognizer&>(
        child.add_gesture_recognizer(std::move(pan)));

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t));
    REQUIRE_FALSE(child.has_pointer_capture(0));

    auto move = pointer_event({40, 32}, MousePhase::drag, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(move, t));
    REQUIRE(child.has_pointer_capture(0));
    REQUIRE(began == 1);
    REQUIRE(pan_ref.translation().x == 10.0f);
    REQUIRE(pan_ref.translation().y == 2.0f);

    move = pointer_event({50, 34}, MousePhase::drag, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(move, t));
    REQUIRE(changed == 1);

    auto up = pointer_event({50, 34}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(ended == 1);
    REQUIRE_FALSE(child.has_pointer_capture(0));
}

TEST_CASE("TapRecognizer recognizes single and double taps",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    int single_taps = 0;
    auto single = std::make_unique<TapRecognizer>();
    single->on_ended = [&](GestureRecognizer&) { ++single_taps; };
    child.add_gesture_recognizer(std::move(single));

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    auto up = pointer_event({30, 30}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(single_taps == 1);

    child.clear_gesture_recognizers();
    int double_taps = 0;
    auto dbl = std::make_unique<TapRecognizer>(2);
    dbl->on_ended = [&](GestureRecognizer&) { ++double_taps; };
    child.add_gesture_recognizer(std::move(dbl));

    t = 1.0;
    down = pointer_event({30, 30}, MousePhase::press, &t);
    up = pointer_event({30, 30}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(double_taps == 0);

    down = pointer_event({30, 30}, MousePhase::press, &t, 0.10);
    up = pointer_event({30, 30}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(double_taps == 1);
}

TEST_CASE("LongPressRecognizer fails when pan movement wins first",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.50);
    long_press->set_max_movement(8.0f);
    int long_began = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++long_began; };
    child.add_gesture_recognizer(std::move(long_press));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(8.0f);
    int pan_began = 0;
    pan->on_began = [&](GestureRecognizer&) { ++pan_began; };
    child.add_gesture_recognizer(std::move(pan));

    double t = 0.0;
    auto down = pointer_event({40, 40}, MousePhase::press, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t));

    auto move = pointer_event({60, 40}, MousePhase::drag, &t, 0.10);
    REQUIRE(root.dispatch_gesture_pointer_event(move, t));
    REQUIRE(pan_began == 1);
    REQUIRE(long_began == 0);

    auto up = pointer_event({60, 40}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(long_began == 0);
}

TEST_CASE("LongPressRecognizer begins after hold when pointer stays inside slop",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.50);
    long_press->set_max_movement(8.0f);
    int long_began = 0;
    int long_ended = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++long_began; };
    long_press->on_ended = [&](GestureRecognizer&) { ++long_ended; };
    child.add_gesture_recognizer(std::move(long_press));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(8.0f);
    int pan_began = 0;
    pan->on_began = [&](GestureRecognizer&) { ++pan_began; };
    child.add_gesture_recognizer(std::move(pan));

    double t = 0.0;
    auto down = pointer_event({40, 40}, MousePhase::press, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t));
    REQUIRE(root.has_time_driven_gestures());
    REQUIRE(needs_continuous_frames(&root));

    root.advance_gesture_recognizers(0.60);
    REQUIRE(long_began == 1);
    REQUIRE(pan_began == 0);
    REQUIRE(child.has_pointer_capture(0));
    REQUIRE_FALSE(root.has_time_driven_gestures());

    auto up = pointer_event({42, 42}, MousePhase::release, &t, 0.60);
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(long_ended == 1);
    REQUIRE_FALSE(child.has_pointer_capture(0));
}

TEST_CASE("failed recognizers reset when a fresh pointer session starts",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.50);
    int long_began = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++long_began; };
    child.add_gesture_recognizer(std::move(long_press));

    double t = 0.0;
    auto down = pointer_event({40, 40}, MousePhase::press, &t);
    auto up = pointer_event({40, 40}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(long_began == 0);

    down = pointer_event({40, 40}, MousePhase::press, &t, 0.20);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t));
    root.advance_gesture_recognizers(t + 0.60);
    REQUIRE(long_began == 1);
}

TEST_CASE("default gesture frame advancement uses host time source",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.0);
    int long_began = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++long_began; };
    child.add_gesture_recognizer(std::move(long_press));

    double t = 0.0;
    auto down = pointer_event({40, 40}, MousePhase::press, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down));
    REQUIRE(root.has_time_driven_gestures());

    root.advance_gesture_recognizers();
    REQUIRE(long_began == 1);
    REQUIRE_FALSE(root.has_time_driven_gestures());
}

TEST_CASE("require_to_fail lets the required recognizer win same-event contention",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto first = std::make_unique<TapRecognizer>();
    auto second = std::make_unique<TapRecognizer>();
    int first_ended = 0;
    int second_ended = 0;
    first->on_ended = [&](GestureRecognizer&) { ++first_ended; };
    second->on_ended = [&](GestureRecognizer&) { ++second_ended; };

    auto& first_ref = child.add_gesture_recognizer(std::move(first));
    auto& second_ref = child.add_gesture_recognizer(std::move(second));
    first_ref.require_to_fail(second_ref);

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    auto up = pointer_event({30, 30}, MousePhase::release, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));

    REQUIRE(first_ended == 0);
    REQUIRE(second_ended == 1);
}

TEST_CASE("require_to_fail preserves callbacks while waiting for failure",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto tap = std::make_unique<TapRecognizer>();
    tap->set_max_press_duration(0.20);
    int tap_ended = 0;
    tap->on_ended = [&](GestureRecognizer&) { ++tap_ended; };

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.50);
    int long_began = 0;
    int long_ended = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++long_began; };
    long_press->on_ended = [&](GestureRecognizer&) { ++long_ended; };

    auto& tap_ref = child.add_gesture_recognizer(std::move(tap));
    auto& long_ref = child.add_gesture_recognizer(std::move(long_press));
    long_ref.require_to_fail(tap_ref);

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t));

    root.advance_gesture_recognizers(0.60);
    REQUIRE(long_began == 0);

    auto up = pointer_event({30, 30}, MousePhase::release, &t, 0.60);
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));
    REQUIRE(tap_ended == 0);
    REQUIRE(long_began == 1);
    REQUIRE(long_ended == 1);
}

TEST_CASE("allow_simultaneous_with lets two recognizers share a pointer stream",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto first = std::make_unique<PanRecognizer>();
    auto second = std::make_unique<PanRecognizer>();
    first->set_min_distance(4.0f);
    second->set_min_distance(4.0f);
    int first_began = 0;
    int second_began = 0;
    first->on_began = [&](GestureRecognizer&) { ++first_began; };
    second->on_began = [&](GestureRecognizer&) { ++second_began; };

    auto& first_ref = child.add_gesture_recognizer(std::move(first));
    auto& second_ref = child.add_gesture_recognizer(std::move(second));
    first_ref.allow_simultaneous_with(second_ref);

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    auto move = pointer_event({45, 30}, MousePhase::drag, &t);
    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(move, t));

    REQUIRE(first_began == 1);
    REQUIRE(second_began == 1);
}

TEST_CASE("SwipeRecognizer and FlingRecognizer recognize release velocity",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    View& child = add_child(root, {20, 20, 100, 100});

    auto swipe = std::make_unique<SwipeRecognizer>();
    swipe->set_min_distance(20.0f);
    swipe->set_min_velocity(200.0f);
    int swipe_ended = 0;
    swipe->on_ended = [&](GestureRecognizer&) { ++swipe_ended; };
    child.add_gesture_recognizer(std::move(swipe));

    auto fling = std::make_unique<FlingRecognizer>();
    fling->set_min_velocity(300.0f);
    int fling_ended = 0;
    fling->on_ended = [&](GestureRecognizer&) { ++fling_ended; };
    auto& fling_ref = static_cast<FlingRecognizer&>(
        child.add_gesture_recognizer(std::move(fling)));
    child.gesture_recognizer_at(0)->allow_simultaneous_with(fling_ref);

    double t = 0.0;
    auto down = pointer_event({30, 30}, MousePhase::press, &t);
    auto move = pointer_event({90, 30}, MousePhase::drag, &t, 0.05);
    auto up = pointer_event({110, 30}, MousePhase::release, &t, 0.05);

    REQUIRE(root.dispatch_gesture_pointer_event(down, t - 0.10));
    REQUIRE(root.dispatch_gesture_pointer_event(move, t - 0.05));
    REQUIRE(root.dispatch_gesture_pointer_event(up, t));

    REQUIRE(swipe_ended == 1);
    REQUIRE(fling_ended == 1);
    REQUIRE(fling_ref.velocity().x > 300.0f);
}

TEST_CASE("PinchRecognizer tracks two touch pointers and reports scale",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 240, 240});
    View& child = add_child(root, {20, 20, 180, 180});

    auto pinch = std::make_unique<PinchRecognizer>();
    pinch->set_min_scale_delta(0.01f);
    int began = 0;
    int changed = 0;
    int ended = 0;
    pinch->on_began = [&](GestureRecognizer&) { ++began; };
    pinch->on_changed = [&](GestureRecognizer&) { ++changed; };
    pinch->on_ended = [&](GestureRecognizer&) { ++ended; };
    auto& pinch_ref = static_cast<PinchRecognizer&>(
        child.add_gesture_recognizer(std::move(pinch)));

    double t = 0.0;
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({60, 100}, MousePhase::press, 1, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({140, 100}, MousePhase::press, 2, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({40, 100}, MousePhase::drag, 1, &t), t));

    REQUIRE(began == 1);
    REQUIRE(pinch_ref.scale() > 1.20f);
    REQUIRE(child.has_pointer_capture(1));

    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({160, 100}, MousePhase::drag, 2, &t), t));
    REQUIRE(changed == 1);
    REQUIRE(child.has_pointer_capture(2));

    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({40, 100}, MousePhase::release, 1, &t), t));
    REQUIRE(ended == 1);
    REQUIRE_FALSE(child.has_pointer_capture(1));
}

TEST_CASE("RotateRecognizer tracks two touch pointers and reports radians",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 240, 240});
    View& child = add_child(root, {20, 20, 180, 180});

    auto rotate = std::make_unique<RotateRecognizer>();
    rotate->set_min_rotation(0.01f);
    int began = 0;
    int ended = 0;
    rotate->on_began = [&](GestureRecognizer&) { ++began; };
    rotate->on_ended = [&](GestureRecognizer&) { ++ended; };
    auto& rotate_ref = static_cast<RotateRecognizer&>(
        child.add_gesture_recognizer(std::move(rotate)));

    double t = 0.0;
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({80, 100}, MousePhase::press, 1, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({140, 100}, MousePhase::press, 2, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({80, 80}, MousePhase::drag, 1, &t), t));

    REQUIRE(began == 1);
    REQUIRE(std::abs(rotate_ref.rotation()) > 0.20f);

    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({80, 80}, MousePhase::release, 1, &t), t));
    REQUIRE(ended == 1);
}

TEST_CASE("pan and pinch can recognize simultaneously across touch sessions",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 260, 260});
    View& child = add_child(root, {20, 20, 200, 200});

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(4.0f);
    int pan_began = 0;
    pan->on_began = [&](GestureRecognizer&) { ++pan_began; };
    auto& pan_ref = child.add_gesture_recognizer(std::move(pan));

    auto pinch = std::make_unique<PinchRecognizer>();
    pinch->set_min_scale_delta(0.01f);
    int pinch_began = 0;
    pinch->on_began = [&](GestureRecognizer&) { ++pinch_began; };
    auto& pinch_ref = child.add_gesture_recognizer(std::move(pinch));
    pan_ref.allow_simultaneous_with(pinch_ref);

    double t = 0.0;
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({70, 100}, MousePhase::press, 1, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({90, 100}, MousePhase::drag, 1, &t), t));
    REQUIRE(pan_began == 1);

    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({150, 100}, MousePhase::press, 2, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({40, 100}, MousePhase::drag, 1, &t), t));
    REQUIRE(pinch_began == 1);
    REQUIRE(child.has_pointer_capture(1));
    REQUIRE(child.has_pointer_capture(2));
}

TEST_CASE("nested parent scroll pan yields to child pan with require-to-fail",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 300, 300});
    View& scroll = add_child(root, {0, 0, 300, 300});
    View& child = add_child(scroll, {40, 40, 160, 160});

    auto child_pan = std::make_unique<PanRecognizer>();
    child_pan->set_min_distance(4.0f);
    int child_began = 0;
    child_pan->on_began = [&](GestureRecognizer&) { ++child_began; };
    auto& child_pan_ref = child.add_gesture_recognizer(std::move(child_pan));

    auto scroll_pan = std::make_unique<PanRecognizer>();
    scroll_pan->set_min_distance(4.0f);
    int scroll_began = 0;
    scroll_pan->on_began = [&](GestureRecognizer&) { ++scroll_began; };
    auto& scroll_pan_ref = scroll.add_gesture_recognizer(std::move(scroll_pan));
    scroll_pan_ref.require_to_fail(child_pan_ref);

    double t = 0.0;
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({80, 80}, MousePhase::press, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({110, 84}, MousePhase::drag, &t), t));

    REQUIRE(child_began == 1);
    REQUIRE(scroll_began == 0);
    REQUIRE(child.has_pointer_capture(0));
    REQUIRE_FALSE(scroll.has_pointer_capture(0));
}

TEST_CASE("touch UI proof combines XY pan, waveform pinch, and nested list pan",
          "[view][gesture]") {
    View root;
    root.set_bounds({0, 0, 360, 320});
    View& xy_pad = add_child(root, {16, 16, 120, 120});
    View& waveform = add_child(root, {152, 16, 180, 120});
    View& list = add_child(root, {16, 160, 316, 120});
    View& row = add_child(list, {0, 24, 316, 32});

    Point xy_translation{};
    auto xy_pan = std::make_unique<PanRecognizer>();
    xy_pan->set_min_distance(3.0f);
    xy_pan->on_changed = [&](GestureRecognizer& recognizer) {
        xy_translation = static_cast<PanRecognizer&>(recognizer).translation();
    };
    xy_pan->on_ended = xy_pan->on_changed;
    xy_pad.add_gesture_recognizer(std::move(xy_pan));

    float waveform_scale = 1.0f;
    auto pinch = std::make_unique<PinchRecognizer>();
    pinch->set_min_scale_delta(0.01f);
    pinch->on_changed = [&](GestureRecognizer& recognizer) {
        waveform_scale = static_cast<PinchRecognizer&>(recognizer).scale();
    };
    pinch->on_ended = pinch->on_changed;
    waveform.add_gesture_recognizer(std::move(pinch));

    auto row_pan = std::make_unique<PanRecognizer>();
    row_pan->set_min_distance(4.0f);
    int row_claims = 0;
    row_pan->on_began = [&](GestureRecognizer&) { ++row_claims; };
    auto& row_pan_ref = row.add_gesture_recognizer(std::move(row_pan));

    auto scroll_pan = std::make_unique<PanRecognizer>();
    scroll_pan->set_min_distance(4.0f);
    int scroll_claims = 0;
    scroll_pan->on_began = [&](GestureRecognizer&) { ++scroll_claims; };
    auto& scroll_pan_ref = list.add_gesture_recognizer(std::move(scroll_pan));
    scroll_pan_ref.require_to_fail(row_pan_ref);

    double t = 0.0;
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({40, 40}, MousePhase::press, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({72, 80}, MousePhase::drag, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({72, 80}, MousePhase::release, &t), t));
    REQUIRE(xy_translation.x == 32.0f);
    REQUIRE(xy_translation.y == 40.0f);

    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({190, 70}, MousePhase::press, 1, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({270, 70}, MousePhase::press, 2, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({170, 70}, MousePhase::drag, 1, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        touch_event({290, 70}, MousePhase::drag, 2, &t), t));
    REQUIRE(waveform_scale > 1.40f);

    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({48, 194}, MousePhase::press, &t), t));
    REQUIRE(root.dispatch_gesture_pointer_event(
        pointer_event({92, 198}, MousePhase::drag, &t), t));
    REQUIRE(row_claims == 1);
    REQUIRE(scroll_claims == 0);
}
