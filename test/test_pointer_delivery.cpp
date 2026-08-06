// test_pointer_delivery.cpp — press, drag, release, wheel, and the reentrancy
// guards around them.
//
// Split out of test_pointer_dispatch.cpp (WAH-7). This is the largest of the
// four subjects and the one with the most invariants: each channel fires
// exactly once and in a fixed order (modern before legacy on press, legacy
// before modern on release), bubbling walks ancestors in their own coordinate
// space, a target that left the tree is inert, and a reentrant cancellation
// stops stale channels rather than letting them continue against freed state.

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

namespace {

// Records which channels a press/drag/release entered, and with which
// button. Lives here rather than in the focus suite because what it
// asserts is a DELIVERY property: an auxiliary button must reach the
// modern channel without entering the left-only legacy callbacks.
class ButtonIdentitySpy : public View {
public:
    void on_mouse_event(const MouseEvent& event) override {
        modern_buttons.push_back(event.button);
        if (modern_callback) modern_callback(event);
    }
    void on_mouse_down(Point) override { ++legacy_down; }
    void on_mouse_drag(Point) override { ++legacy_drag; }
    void on_mouse_up(Point) override { ++legacy_up; }

    std::vector<MouseButton> modern_buttons;
    int legacy_down = 0;
    int legacy_drag = 0;
    int legacy_up = 0;
    std::function<void(const MouseEvent&)> modern_callback;
};

}  // namespace



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

// ── Press + release delivery (deliver_mouse_down / deliver_mouse_up) ─────────
//
// The press/release routing (modern + legacy channels, the W3C pointer bubble,
// and the up verb's same-target click suppression) previously lived inline in
// BOTH macOS hosts and drifted — overlay routing and the deferred-click teardown
// existed only in the standalone path. It is now the portable
// deliver_mouse_down / deliver_mouse_up. The clearest pinned contract is the
// suppression rule the smoke could only assert indirectly: a press on A that
// releases over B is NOT a click.

namespace {

// Records the ordered channel stream for press and release, plus the click and
// the local coordinates each channel saw.
class PressSpy : public View {
public:
    PressSpy() {
        on_click = [this] { log.push_back("on_click"); };
    }
    void on_mouse_event(const MouseEvent& e) override {
        log.push_back(e.phase == MousePhase::press ? "event:press" : "event:release");
        events.push_back(e);
    }
    void on_mouse_down(Point p) override {
        log.push_back("on_mouse_down");
        last_down = p;
    }
    void on_mouse_up(Point p) override {
        log.push_back("on_mouse_up");
        last_up = p;
    }
    void on_mouse_cancel(Point p) override {
        log.push_back("on_mouse_cancel");
        last_up = p;
    }
    std::vector<std::string> log;
    std::vector<MouseEvent> events;
    Point last_down{}, last_up{};
};

// A MouseUpHost that records every fire_click callback (what the standalone host
// wires to its deferred/global-click path and the plugin host to a synchronous
// call). Invokes the handler so click bubbling can be asserted end to end.
struct RecordingClickHost {
    int fires = 0;
    std::string last_id;
    uint16_t last_mods = 0;
    pulp::view::MouseUpHost host() {
        pulp::view::MouseUpHost h;
        h.fire_click = [this](const std::function<void()>& handler,
                              const std::string& id, uint16_t mods) {
            ++fires;
            last_id = id;
            last_mods = mods;
            if (handler) handler();
        };
        return h;
    }
};

}  // namespace

TEST_CASE("deliver_mouse_down fires modern press then legacy down, in order",
          "[view][input][press]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    const bool alive = deliver_mouse_down(root, spy, {130, 70}, kModShift, /*click_count=*/2);

    REQUIRE(alive);
    REQUIRE(spy->log == std::vector<std::string>{"event:press", "on_mouse_down"});
    REQUIRE(spy->events.size() == 1);
    const MouseEvent& e = spy->events.front();
    CHECK(e.phase == MousePhase::press);
    CHECK(e.is_down);
    CHECK(e.isShiftDown());               // modifiers ride the modern channel
    CHECK(e.button == MouseButton::left);
    CHECK(e.click_count == 2);
    CHECK_THAT(e.position.x, WithinAbs(30.0f, 0.01f));   // 130 - 100
    CHECK_THAT(e.window_position.x, WithinAbs(130.0f, 0.01f));
    CHECK_THAT(spy->last_down.y, WithinAbs(20.0f, 0.01f));  // 70 - 50
}

TEST_CASE("shared press and release preserve stylus orientation",
          "[view][input][pointer][stylus]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 100, 100});
    root.add_child(std::move(child));

    std::vector<MouseEvent> events;
    spy->modern_callback = [&](const MouseEvent& event) {
        events.push_back(event);
    };
    const PointerAttributes pencil{
        .type = PointerType::pen,
        .pressure = 0.75f,
        .pointer_id = 4,
        .altitude_angle = 0.45f,
        .azimuth_angle = 1.25f,
    };

    REQUIRE(deliver_mouse_down(root, spy, {10, 10}, 0, 1, true,
                               MouseButton::left, {}, pencil));
    deliver_mouse_up(root, spy, {10, 10}, 0, 1, {}, MouseButton::left,
                     pencil);

    REQUIRE(events.size() == 2);
    for (const auto& event : events) {
        CHECK(event.pointer_type == PointerType::pen);
        CHECK(event.pointer_id == 4);
        CHECK_THAT(event.altitude_angle, WithinAbs(0.45f, 0.001f));
        CHECK_THAT(event.azimuth_angle, WithinAbs(1.25f, 0.001f));
    }
}

TEST_CASE("guarded mouse down stops stale channels after reentrant cancellation",
          "[view][input][press][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto wrapper = std::make_unique<View>();
    auto* wrapper_ptr = wrapper.get();
    wrapper->set_bounds({0, 0, 200, 300});
    int bubbled = 0;
    wrapper->on_pointer_event = [&](const MouseEvent&) { ++bubbled; };

    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 300});
    wrapper->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    auto nested = std::make_unique<View>();
    auto* nested_target = nested.get();
    nested->set_bounds({200, 0, 200, 300});
    root.add_child(std::move(nested));

    uint64_t generation = 1;
    const uint64_t outer_generation = generation;
    View* captured_target = spy;
    spy->modern_callback = [&](const MouseEvent&) {
        // Model a synchronous cancel followed by a later generation becoming
        // current before the stale outer press frame resumes.
        ++generation;
        captured_target = nested_target;
    };
    MouseDownHost host;
    host.should_continue = [&] { return generation == outer_generation; };

    const bool alive = deliver_mouse_down(root, spy, {20, 20}, kModNone, 1,
                                          true, MouseButton::left, host);
    if (!alive && generation == outer_generation)
        captured_target = nullptr;  // mirrors the Windows host ownership check

    CHECK_FALSE(alive);
    CHECK(spy->modern_buttons.size() == 1);
    CHECK(spy->legacy_down == 0);
    CHECK(bubbled == 0);
    CHECK(captured_target == nested_target);
    CHECK(wrapper_ptr->child_count() == 1);
}

TEST_CASE("pointerdown bubbling survives an ancestor deleting itself",
          "[view][input][press][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    int root_bubbles = 0;
    root.on_pointer_event = [&](const MouseEvent&) { ++root_bubbles; };

    auto wrapper = std::make_unique<View>();
    auto* wrapper_ptr = wrapper.get();
    wrapper->set_bounds({0, 0, 200, 200});
    wrapper->on_pointer_event = [&](const MouseEvent&) {
        auto removed = root.remove_child(wrapper_ptr);
        removed.reset();
    };

    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* target = child.get();
    child->set_bounds({0, 0, 200, 200});
    wrapper->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    CHECK_FALSE(deliver_mouse_down(root, target, {20, 20}, kModNone, 1,
                                   true));
    CHECK(root_bubbles == 0);
    CHECK(root.child_count() == 0);
}

TEST_CASE("pointerup bubbling stops after an ancestor deletes the target",
          "[view][input][release][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    int root_bubbles = 0;
    root.on_pointer_event = [&](const MouseEvent&) { ++root_bubbles; };

    auto wrapper = std::make_unique<View>();
    auto* wrapper_ptr = wrapper.get();
    wrapper->set_bounds({0, 0, 200, 200});
    wrapper->on_pointer_event = [&](const MouseEvent&) {
        auto removed = root.remove_child(wrapper_ptr);
        removed.reset();
    };

    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* target = child.get();
    child->set_bounds({0, 0, 200, 200});
    wrapper->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    deliver_mouse_up(root, target, {20, 20}, kModNone, 1, {});
    CHECK(root_bubbles == 0);
    CHECK(root.child_count() == 0);
}

TEST_CASE("guarded right press suppresses stale context-menu continuation",
          "[view][input][press][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* spy = child.get();
    spy->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    uint64_t generation = 1;
    const uint64_t original = generation;
    int context_menu_dispatches = 0;
    spy->modern_callback = [&](const MouseEvent&) { ++generation; };
    MouseDownHost host;
    host.should_continue = [&] { return generation == original; };

    const bool alive = deliver_mouse_down(root, spy, {20, 20}, kModNone, 1,
                                          true, MouseButton::right, host);
    if (generation == original)
        ++context_menu_dispatches;  // mirrors the Windows host continuation

    CHECK_FALSE(alive);
    CHECK(spy->modern_buttons.size() == 1);
    CHECK(context_menu_dispatches == 0);
}

TEST_CASE("deliver_mouse_down bubbles pointerdown to registerPointer ancestors",
          "[view][input][press]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto wrapper = std::make_unique<View>();
    View* wrap = wrapper.get();
    wrap->set_bounds({100, 50, 200, 200});
    int wrap_downs = 0;
    Point wrap_pt{};
    wrap->on_pointer_event = [&](const MouseEvent& e) {
        if (e.phase == MousePhase::press) { ++wrap_downs; wrap_pt = e.position; }
    };

    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({10, 10, 50, 50});          // (110,60) in root space
    wrap->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    SECTION("bubble on (default): the wrapper sees the pointerdown in its space") {
        REQUIRE(deliver_mouse_down(root, spy, {130, 70}, 0));
        CHECK(wrap_downs == 1);
        CHECK_THAT(wrap_pt.x, WithinAbs(30.0f, 0.01f));   // 130 - 100
    }

    SECTION("bubble off (overlay path): ancestors do NOT receive pointerdown") {
        REQUIRE(deliver_mouse_down(root, spy, {130, 70}, 0, /*click_count=*/1, /*bubble=*/false));
        CHECK(wrap_downs == 0);                            // preserved no-bubble behavior
        // The target itself still got both channels.
        CHECK(spy->log == std::vector<std::string>{"event:press", "on_mouse_down"});
    }
}

TEST_CASE("deliver_mouse_down is inert for a target that left the tree",
          "[view][input][press]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    auto detached = root.remove_child(spy);      // unmounted before the press
    const bool alive = deliver_mouse_down(root, spy, {130, 70}, 0);
    CHECK_FALSE(alive);
    CHECK(spy->log.empty());
}

TEST_CASE("deliver_mouse_up fires legacy up then modern release, then a click",
          "[view][input][release]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    spy->set_id("knob-1");
    root.add_child(std::move(child));

    RecordingClickHost rec;
    // Release lands on the same view the press captured → a click.
    deliver_mouse_up(root, spy, {130, 70}, kModCmd, /*click_count=*/1, rec.host());

    // Up is legacy-before-modern, then the on_click handler fires (bubbled).
    REQUIRE(spy->log == std::vector<std::string>{"on_mouse_up", "event:release", "on_click"});
    CHECK(rec.fires == 1);
    CHECK(rec.last_id == "knob-1");               // immediate hit's id, for global-click
    CHECK((rec.last_mods & kModCmd) != 0);
    REQUIRE(spy->events.size() == 1);
    CHECK(spy->events.front().phase == MousePhase::release);
    CHECK_FALSE(spy->events.front().is_down);
    CHECK_THAT(spy->last_up.x, WithinAbs(30.0f, 0.01f));
}

TEST_CASE("deliver_mouse_up suppresses the click when release lands off the press target",
          "[view][input][release]") {
    // The direct down-A / up-B assertion the single-point simulate_click cannot
    // express (the smoke pinned it only indirectly via simulate_drag). The host
    // rule is `released == drag_target`; releasing over B must not fire A's click.
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto a = std::make_unique<PressSpy>();
    PressSpy* spyA = a.get();
    spyA->set_bounds({0, 0, 150, 300});           // left half — press target
    root.add_child(std::move(a));

    auto b = std::make_unique<PressSpy>();
    PressSpy* spyB = b.get();
    spyB->set_bounds({200, 0, 150, 300});         // right half — release lands here
    root.add_child(std::move(b));

    RecordingClickHost rec;
    // Press captured A; the up point {260,150} hit-tests to B, not A.
    deliver_mouse_up(root, spyA, {260, 150}, 0, /*click_count=*/1, rec.host());

    // A still receives its up channels (the captured target always does)…
    REQUIRE(spyA->log == std::vector<std::string>{"on_mouse_up", "event:release"});
    // …but NO click is synthesized, because the release did not land on A.
    CHECK(rec.fires == 0);
    // B is a bystander: an off-target release is not a press/click for it.
    CHECK(spyB->log.empty());
}

TEST_CASE("deliver_mouse_up bubbles the click handler from the press target",
          "[view][input][release]") {
    // hit_test returns the deepest view, but the on_click handler often lives on
    // an ancestor (@pulp/react wraps `<button onClick>` text in a synthetic
    // Label). The click_handler passed to fire_click is resolved by walking up
    // from the PRESS target, and it is captured before delivery.
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto wrapper = std::make_unique<View>();
    View* wrap = wrapper.get();
    wrap->set_bounds({100, 50, 200, 200});
    int wrap_clicks = 0;
    wrap->on_click = [&] { ++wrap_clicks; };      // handler on the ancestor

    auto child = std::make_unique<PressSpy>();    // leaf: no on_click of its own
    PressSpy* spy = child.get();
    spy->on_click = nullptr;                       // ensure the leaf has none
    spy->set_bounds({10, 10, 120, 120});          // covers the up point
    wrap->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    RecordingClickHost rec;
    // Release over the leaf (its own press target) → click bubbles to wrap.
    deliver_mouse_up(root, spy, {150, 100}, 0, /*click_count=*/1, rec.host());

    CHECK(rec.fires == 1);
    CHECK(wrap_clicks == 1);                        // the ancestor's handler ran
}

TEST_CASE("deliver_mouse_up is inert for a target that left the tree",
          "[view][input][release]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    auto detached = root.remove_child(spy);
    RecordingClickHost rec;
    deliver_mouse_up(root, spy, {130, 70}, 0, /*click_count=*/1, rec.host());
    CHECK(spy->log.empty());
    CHECK(rec.fires == 0);
}

TEST_CASE("deliver_mouse_cancel shares terminal routing and never clicks",
          "[view][input][release][cancel]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto wrapper = std::make_unique<View>();
    View* wrap = wrapper.get();
    wrap->set_bounds({100, 50, 200, 200});
    int bubbles = 0;
    MouseEvent bubbled{};
    wrap->on_pointer_event = [&](const MouseEvent& event) {
        ++bubbles;
        bubbled = event;
    };

    auto child = std::make_unique<PressSpy>();
    PressSpy* spy = child.get();
    spy->set_bounds({10, 10, 120, 120});
    wrapper->add_child(std::move(child));
    root.add_child(std::move(wrapper));

    deliver_mouse_cancel(root, spy, {130, 70}, kModShift, 1,
                         MouseButton::left,
                         PointerAttributes{PointerType::touch, 0.25f, 7});

    REQUIRE(spy->log ==
            std::vector<std::string>{"on_mouse_cancel", "event:release"});
    REQUIRE(spy->events.size() == 1);
    CHECK(spy->events.front().is_cancelled);
    CHECK(spy->events.front().pointer_type == PointerType::touch);
    CHECK(spy->events.front().pointer_id == 7);
    CHECK(bubbles == 1);
    CHECK(bubbled.is_cancelled);
    CHECK_THAT(bubbled.position.x, WithinAbs(30.0f, 0.01f));
    CHECK(spy->log.back() != "on_click");
}

// ── Wheel routing (deliver_mouse_wheel) ──────────────────────────────────────
//
// The precedence (popup → empty-pane scroll → value widget → W3C wheel bubble →
// deepest hit) previously lived inline in BOTH macOS hosts and drifted. It is
// now the portable pulp::view::deliver_mouse_wheel — see pointer_dispatch.hpp.

namespace {

// Adjusts its VALUE on a wheel over it (knob/fader idiom): takes precedence
// over any enclosing scroll container.
class WheelValueSpy : public View {
public:
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float dy) override { ++hits; last_delta = dy; }
    int hits = 0;
    float last_delta = 0.0f;
};

// A scroll container: consumes the wheel and terminates the ancestor walk.
class WheelScrollSpy : public View {
public:
    bool wants_wheel_scroll() const override { return true; }
    void on_mouse_event(const MouseEvent& e) override {
        ++hits;
        last = e;
    }
    int hits = 0;
    MouseEvent last{};
};

class SelfDeletingWheelScroll : public View {
public:
    explicit SelfDeletingWheelScroll(View& root) : root_(root) {}
    bool wants_wheel_scroll() const override { return true; }
    void on_mouse_event(const MouseEvent&) override {
        auto removed = root_.remove_child(this);
        removed.reset();
    }
private:
    View& root_;
};

class EmptyBackgroundSelfDeletingWheelScroll
    : public SelfDeletingWheelScroll {
public:
    using SelfDeletingWheelScroll::SelfDeletingWheelScroll;
    View* hit_test(Point) override { return nullptr; }
};

// Registers a JS-style pointer handler; the W3C wheel bubble delivers the wheel
// event to it (it self-filters on is_wheel, like registerWheel does).
class WheelPointerSpy : public View {
public:
    WheelPointerSpy() {
        on_pointer_event = [this](const MouseEvent& e) {
            if (e.is_wheel) {
                ++wheel_events;
                last = e;
            }
        };
    }
    int wheel_events = 0;
    MouseEvent last{};
};

// Counts terminal wheel dispatches so the request_repaint contract is asserted.
pulp::view::WheelHost counting_host(int& counter) {
    pulp::view::WheelHost h;
    h.request_repaint = [&counter] { ++counter; };
    return h;
}

}  // namespace

TEST_CASE("deliver_mouse_wheel steps a value widget under the cursor",
          "[view][input][wheel]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<WheelValueSpy>();
    WheelValueSpy* spy = child.get();
    spy->set_bounds({100, 50, 120, 80});
    root.add_child(std::move(child));

    int repaints = 0;
    deliver_mouse_wheel(root, {130, 70}, /*dx=*/0.0f, /*dy=*/3.0f, counting_host(repaints));

    // The value widget consumed the wheel (its on_wheel got the Pulp-convention
    // delta_y) and the host was asked to repaint exactly once.
    CHECK(spy->hits == 1);
    CHECK_THAT(spy->last_delta, WithinAbs(3.0f, 0.01f));
    CHECK(repaints == 1);
}

TEST_CASE("deliver_mouse_wheel routes to a wheel-scroll ancestor and stops",
          "[view][input][wheel]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto scroller = std::make_unique<WheelScrollSpy>();
    WheelScrollSpy* spy = scroller.get();
    spy->set_bounds({0, 0, 400, 300});
    root.add_child(std::move(scroller));

    int repaints = 0;
    deliver_mouse_wheel(root, {130, 70}, /*dx=*/0.0f, /*dy=*/-2.0f, counting_host(repaints));

    REQUIRE(spy->hits == 1);
    CHECK(spy->last.is_wheel);
    CHECK_THAT(spy->last.scroll_delta_y, WithinAbs(-2.0f, 0.01f));
    CHECK(repaints == 1);
}

TEST_CASE("wheel layout is skipped when the scrolling view deletes itself",
          "[view][input][wheel][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto scroller = std::make_unique<SelfDeletingWheelScroll>(root);
    scroller->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(scroller));

    int repaints = 0;
    deliver_mouse_wheel(root, {10, 10}, 0.0f, 1.0f,
                        counting_host(repaints));

    CHECK(root.child_count() == 0);
    CHECK(repaints == 1);
}

TEST_CASE("empty-background wheel skips layout after its scroller deletes itself",
          "[view][input][wheel][reentrancy]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_pointer_events(View::PointerEvents::box_none);
    auto scroller =
        std::make_unique<EmptyBackgroundSelfDeletingWheelScroll>(root);
    scroller->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(scroller));

    int repaints = 0;
    deliver_mouse_wheel(root, {10, 10}, 0.0f, 1.0f,
                        counting_host(repaints));

    CHECK(root.child_count() == 0);
    CHECK(repaints == 1);
}

TEST_CASE("deliver_mouse_wheel bubbles to a pointer ancestor, then the deepest hit",
          "[view][input][wheel]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto wrapper = std::make_unique<WheelPointerSpy>();
    WheelPointerSpy* wrap = wrapper.get();
    wrap->set_bounds({50, 40, 300, 200});

    auto leaf = std::make_unique<View>();
    View* leafp = leaf.get();
    leafp->set_bounds({10, 10, 100, 100});   // hit target inside the wrapper
    wrap->add_child(std::move(leaf));
    root.add_child(std::move(wrapper));

    int repaints = 0;
    // Point (70,60) is inside leaf; neither leaf nor root handle the wheel, so
    // it bubbles up to the pointer-registered wrapper.
    deliver_mouse_wheel(root, {70, 60}, /*dx=*/0.0f, /*dy=*/1.0f, counting_host(repaints));

    CHECK(wrap->wheel_events == 1);          // W3C wheel bubble reached the wrapper
    CHECK(wrap->last.is_wheel);
    CHECK(repaints == 1);                     // one terminal dispatch to the deepest hit
}

TEST_CASE("deliver_mouse_wheel scrolls an open ComboBox popup ahead of the tree",
          "[view][input][wheel][combo]") {
    // The dropdown paints as an overlay with no view backing, so a plain
    // hit_test lands on the sibling underneath. The popup bypass must route the
    // wheel to the open menu — mirrors the mac host's active_popup_ path.
    View root;
    root.set_bounds({0, 0, 200, 100});        // short window → menu clamps + scrolls
    auto owned = std::make_unique<ComboBox>();
    ComboBox* combo = owned.get();
    combo->set_bounds({0, 0, 120, 24});
    std::vector<std::string> items;
    for (int i = 0; i < 12; ++i) items.push_back("Item" + std::to_string(i));
    combo->set_items(items);
    combo->set_selected(0);
    root.add_child(std::move(owned));

    MouseEvent open_click;
    open_click.position = {60, 12};
    open_click.is_down = true;
    combo->on_mouse_event(open_click);        // opens the menu, sets active_popup_
    REQUIRE(combo->is_open());

    float x = 0, y = 0, w = 0, h = 0;
    REQUIRE(combo->dropdown_window_rect(x, y, w, h));

    int repaints = 0;
    // Wheel down over the middle of the open menu several times.
    for (int k = 0; k < 6; ++k) {
        deliver_mouse_wheel(root, {x + w / 2, y + h / 2}, 0.0f, 1.0f, counting_host(repaints));
        REQUIRE(combo->is_open());            // the wheel must not close the menu
    }
    CHECK(repaints == 6);                      // every tick routed to the popup + repainted

    // The scroll revealed clipped items: clicking the first visible row now
    // selects an item beyond the first page.
    combo->dropdown_window_rect(x, y, w, h);
    MouseEvent pick;
    pick.position = {60, y + 12.0f};
    pick.is_down = true;
    combo->on_mouse_event(pick);
    CHECK(combo->selected() > 0);
}

TEST_CASE("deliver_mouse_wheel with an empty host repaint hook is a no-op-safe",
          "[view][input][wheel]") {
    // The plugin host passes an empty WheelHost (its frame pump handles repaint).
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<WheelValueSpy>();
    WheelValueSpy* spy = child.get();
    spy->set_bounds({0, 0, 400, 300});
    root.add_child(std::move(child));

    deliver_mouse_wheel(root, {10, 10}, 0.0f, 1.0f, /*host=*/{});
    CHECK(spy->hits == 1);                     // routing still happens; no repaint hook to call
}

// A scripted widget subscribes to pointer input through the `on_pointer_event`
// and `on_drag` callbacks (registerPointer installs both). simulate_drag used to
// drive only the point-only virtuals, so those two stayed silent: a headless
// drag test could assert its handlers were installed and still never exercise
// them, and every JS knob read as inert under simulation while working in a

TEST_CASE("auxiliary buttons retain identity without entering left-only legacy callbacks",
          "[view][input]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<ButtonIdentitySpy>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    REQUIRE(deliver_mouse_down(root, spy, {20, 20}, kModNone, 1, true,
                               MouseButton::right));
    deliver_mouse_drag(root, spy, {30, 30}, kModNone, 1,
                       PointerType::mouse, 0.5f, MouseButton::right);
    deliver_mouse_up(root, spy, {30, 30}, kModNone, 1, MouseUpHost{},
                     MouseButton::right);

    const std::vector<MouseButton> expected{
        MouseButton::right, MouseButton::right, MouseButton::right};
    REQUIRE(spy->modern_buttons == expected);
    REQUIRE(spy->legacy_down == 0);
    REQUIRE(spy->legacy_drag == 0);
    REQUIRE(spy->legacy_up == 0);
}
