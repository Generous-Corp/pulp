// test_disabled_input_contract.cpp — the input contract of a disabled view.
//
// A disabled view, or one inside a disabled ancestor, receives no key, text,
// gesture, hover-enter, press, move, drag or double-click. It still receives
// the release and the pointer-exit that finish an interaction already in
// progress, so nothing is left stuck pressed. The cursor over it is the
// default cursor.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/gesture.hpp>
#include <pulp/view/hover_cursor.hpp>
#include <pulp/view/pointer_dispatch.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <memory>

using namespace pulp::view;

namespace {

// Counts every pointer-facing callback and keeps a pressed flag the way a
// button does: set on press, cleared on release.
class InputSpy : public View {
public:
    void on_mouse_down(Point) override { ++downs; pressed = true; }
    void on_mouse_drag(Point) override {
        ++drags;
        if (disable_after_drags > 0 && drags == disable_after_drags)
            set_enabled(false);
    }
    void on_mouse_up(Point) override { ++ups; pressed = false; }
    void on_mouse_enter() override { ++enters; }
    void on_mouse_leave() override { ++leaves; }
    bool on_key_event(const KeyEvent&) override { ++keys; return true; }
    void on_text_input(const TextInputEvent&) override { ++texts; }

    int downs = 0, drags = 0, ups = 0, enters = 0, leaves = 0, keys = 0, texts = 0;
    int disable_after_drags = 0;
    bool pressed = false;
};

struct Fixture {
    View root;
    View* group = nullptr;
    InputSpy* spy = nullptr;

    Fixture() {
        root.set_bounds({0, 0, 400, 300});
        auto g = std::make_unique<View>();
        group = g.get();
        group->set_bounds({0, 0, 400, 300});
        auto s = std::make_unique<InputSpy>();
        spy = s.get();
        spy->set_bounds({50, 50, 100, 100});
        spy->set_focusable(true);
        group->add_child(std::move(s));
        root.add_child(std::move(g));
    }
};

constexpr Point kInside{70, 70};

}  // namespace

TEST_CASE("disabling a widget mid-drag ends the drag with exactly one up",
          "[view][input][disabled]") {
    Fixture f;
    f.spy->disable_after_drags = 2;

    f.root.simulate_drag({60, 60}, {140, 60}, 8);

    CHECK(f.spy->downs == 1);
    CHECK(f.spy->drags == 2);  // the disabling move, and nothing after it
    CHECK(f.spy->ups == 1);
    CHECK_FALSE(f.spy->pressed);
}

TEST_CASE("a real fader disabled mid-drag is not left holding its gesture",
          "[view][input][disabled]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto fader = std::make_unique<Fader>();
    Fader* fp = fader.get();
    fp->set_bounds({10, 10, 40, 160});
    root.add_child(std::move(fader));

    View* target = root.hit_test({20, 100});
    REQUIRE(target == fp);
    REQUIRE(deliver_mouse_down(root, fp, {20, 100}, 0));
    REQUIRE(fp->is_gesture_active());

    fp->set_enabled(false);
    deliver_mouse_drag(root, fp, {20, 60}, 0);
    CHECK(fp->is_gesture_active());  // the drag was refused, not yet released

    deliver_mouse_up(root, fp, {20, 60}, 0, 1, MouseUpHost{});
    CHECK_FALSE(fp->is_gesture_active());
}

TEST_CASE("a disabled ancestor disables the whole subtree",
          "[view][input][disabled]") {
    Fixture f;
    f.group->set_enabled(false);

    CHECK_FALSE(f.spy->effectively_enabled());
    CHECK(f.spy->enabled());

    f.root.simulate_click(kInside);
    f.root.simulate_drag({60, 60}, {140, 60}, 4);
    CHECK(f.spy->downs == 0);
    CHECK(f.spy->drags == 0);
    CHECK(f.spy->ups == 0);

    // Delivery verbs fed a stale target refuse as well.
    CHECK_FALSE(deliver_mouse_down(f.root, f.spy, kInside, 0));
    CHECK(f.spy->downs == 0);
    CHECK_FALSE(deliver_key_event(*f.spy, KeyEvent{.key = KeyCode::a}));
    deliver_text_input(*f.spy, TextInputEvent{.text = "x"});
    CHECK(f.spy->keys == 0);
    CHECK(f.spy->texts == 0);
}

TEST_CASE("an enabled view still receives key and text input",
          "[view][input][disabled]") {
    Fixture f;
    CHECK(deliver_key_event(*f.spy, KeyEvent{.key = KeyCode::a}));
    CHECK(deliver_text_input(*f.spy, TextInputEvent{.text = "x"}));
    CHECK(f.spy->keys == 1);
    CHECK(f.spy->texts == 1);
}

TEST_CASE("a double-click on a disabled view delivers nothing",
          "[view][input][disabled]") {
    Fixture f;
    f.spy->set_enabled(false);
    CHECK_FALSE(deliver_mouse_down(f.root, f.spy, kInside, 0, /*click_count=*/2));
    CHECK(f.spy->downs == 0);
}

TEST_CASE("a disabled view gains no hover but still receives the exit",
          "[view][input][disabled]") {
    Fixture f;

    f.root.simulate_hover(kInside);
    REQUIRE(f.spy->enters == 1);
    REQUIRE(f.spy->is_hovered());

    f.spy->set_enabled(false);
    f.root.simulate_hover({300, 250});  // pointer leaves
    CHECK(f.spy->leaves == 1);
    CHECK_FALSE(f.spy->is_hovered());

    f.root.simulate_hover(kInside);  // pointer returns to a disabled view
    CHECK(f.spy->enters == 1);
    CHECK_FALSE(f.spy->is_hovered());
}

TEST_CASE("the cursor over a disabled view is the default cursor",
          "[view][input][disabled]") {
    Fixture f;
    f.spy->set_cursor(View::CursorStyle::pointer);
    f.group->set_cursor(View::CursorStyle::crosshair);

    CHECK(hover_cursor_at(f.root, kInside) == View::CursorStyle::pointer);

    f.spy->set_enabled(false);
    CHECK(hover_cursor_at(f.root, kInside) == View::CursorStyle::default_);

    // Whole disabled subtree, not only the leaf.
    f.spy->set_enabled(true);
    f.group->set_enabled(false);
    CHECK(hover_cursor_at(f.root, kInside) == View::CursorStyle::default_);
}

TEST_CASE("a recognizer on a view disabled mid-gesture is cancelled once",
          "[view][input][disabled][gesture]") {
    Fixture f;
    auto& rec = f.spy->add_gesture_recognizer(std::make_unique<PanRecognizer>());
    int cancelled = 0, changed_after_disable = 0;
    bool disabled = false;
    rec.on_cancelled = [&](GestureRecognizer&) { ++cancelled; };
    rec.on_changed = [&](GestureRecognizer&) {
        if (disabled) ++changed_after_disable;
    };

    auto event = [&](MousePhase phase, Point p) {
        MouseEvent e;
        e.position = p;
        e.window_position = p;
        e.is_down = phase != MousePhase::release;
        e.phase = phase;
        return e;
    };
    f.root.dispatch_gesture_pointer_event(event(MousePhase::press, {60, 60}), 0.0);
    f.root.dispatch_gesture_pointer_event(event(MousePhase::drag, {90, 60}), 0.01);
    f.root.dispatch_gesture_pointer_event(event(MousePhase::drag, {110, 60}), 0.02);
    REQUIRE(rec.state() == GestureState::changed);

    f.spy->set_enabled(false);
    disabled = true;
    f.root.dispatch_gesture_pointer_event(event(MousePhase::drag, {130, 60}), 0.03);
    f.root.dispatch_gesture_pointer_event(event(MousePhase::drag, {150, 60}), 0.04);
    f.root.dispatch_gesture_pointer_event(event(MousePhase::release, {150, 60}), 0.05);

    CHECK(cancelled == 1);
    CHECK(changed_after_disable == 0);
}
