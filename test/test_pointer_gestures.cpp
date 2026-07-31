// test_pointer_gestures.cpp — gesture arbitration and the headless simulator.
//
// Split out of test_pointer_dispatch.cpp (WAH-7). Gesture yielding is a
// different subject from raw delivery: it decides whether a recognizer CLAIMS
// the stream, and the interesting cases are the transitions — a mere candidate
// must not yield, a mid-drag claim must close and clear the raw drag, and a
// cancelled release must terminate a time-driven claim.
//
// The simulator cases live here because what they assert is that
// `simulate_drag` delivers what the real hosts deliver, including stopping
// mid-loop once a recognizer claims.

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


TEST_CASE("simulate_drag drives the callback channels a scripted widget uses",
          "[view][input][drag]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child = std::make_unique<View>();
    View* knob = child.get();
    knob->set_bounds({100, 50, 120, 120});
    root.add_child(std::move(child));

    std::vector<MousePhase> phases;
    std::vector<Point> drags;
    knob->on_pointer_event = [&](const MouseEvent& e) { phases.push_back(e.phase); };
    knob->on_drag = [&](Point p) { drags.push_back(p); };

    REQUIRE(root.hit_test({160, 80}) == knob);
    root.simulate_drag({160, 80}, {160, 160}, /*steps=*/4);

    // One pointermove per step, each in the knob's own local space.
    REQUIRE(drags.size() == 4);
    CHECK_THAT(drags.back().y, WithinAbs(110.0f, 0.01f));   // 160 - 50

    // The modern channel carries the press and the release around them.
    REQUIRE(phases.size() >= 2);
    CHECK(phases.front() == MousePhase::press);
    CHECK(phases.back() == MousePhase::release);
}

// ── should_yield_to_gesture ──────────────────────────────────────────────
//
// The gating decision every host phase early-returns on. It was spelled out
// inline at six call sites; the standalone macOS host had it wrong (yielding on
// the CONSUMED flag rather than an actual claim) for a full release after the
// plugin host was fixed, which is why it lives in one tested place now.

namespace {

// Counts the press/drag/release a widget under a recognizer receives.
// (Distinct from DragSpy above, which logs channel ORDER rather than counts.)
struct PhaseCounter final : View {
    int downs = 0, drags = 0, ups = 0;
    void on_mouse_down(Point) override { ++downs; }
    void on_mouse_drag(Point) override { ++drags; }
    void on_mouse_up(Point) override { ++ups; }
};

MouseEvent phase_event(Point p, MousePhase phase) {
    MouseEvent e;
    e.position = p;
    e.window_position = p;
    e.button = MouseButton::left;
    e.is_down = phase != MousePhase::release;
    e.phase = phase;
    return e;
}

}  // namespace

TEST_CASE("should_yield_to_gesture does not yield without any recognizer",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    child->set_bounds({20, 20, 160, 160});
    root.add_child(std::move(child));

    CHECK_FALSE(should_yield_to_gesture(root, phase_event({40, 40}, MousePhase::press)));
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({80, 80}, MousePhase::drag)));
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({80, 80}, MousePhase::release)));
}

// The regression that matters: a recognizer that never recognizes. Its
// per-event dispatch returns true throughout — a candidate EXISTS — so a host
// gating on that value hands every event to the gesture and the widget under it
// becomes permanently undraggable while looking perfectly alive.
TEST_CASE("should_yield_to_gesture does not yield to a mere candidate",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({20, 20, 160, 160});
    root.add_child(std::move(child));
    spy->add_gesture_recognizer(std::make_unique<TapRecognizer>(2));

    // Precondition: the raw dispatch DOES report consumed here. Without this
    // the test would pass for the wrong reason — a recognizer that was never
    // a candidate at all yields false trivially.
    auto press = phase_event({40, 40}, MousePhase::press);
    REQUIRE(root.dispatch_gesture_pointer_event(press));

    CHECK_FALSE(should_yield_to_gesture(root, phase_event({60, 60}, MousePhase::drag)));
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({90, 90}, MousePhase::drag)));
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({90, 90}, MousePhase::release)));
}

TEST_CASE("should_yield_to_gesture yields once a recognizer claims",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(40.0f);
    spy->add_gesture_recognizer(std::move(pan));

    // Press and a short move stay under the slop: candidate, not a claim.
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({40, 40}, MousePhase::press)));
    CHECK_FALSE(should_yield_to_gesture(root, phase_event({50, 40}, MousePhase::drag)));
    // Crossing the slop is the claim edge — and it stays claimed afterward, so
    // the host keeps yielding for the rest of the gesture.
    CHECK(should_yield_to_gesture(root, phase_event({100, 40}, MousePhase::drag)));
    CHECK(should_yield_to_gesture(root, phase_event({140, 40}, MousePhase::drag)));
}

// Guards the seam against being "simplified" into a pure predicate: the arbiter
// only advances because every event is dispatched, including the events that
// return false. Skip those and nothing ever reaches the claim edge.
TEST_CASE("should_yield_to_gesture dispatches even when it does not yield",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(40.0f);
    int began = 0;
    pan->on_began = [&](GestureRecognizer&) { ++began; };
    spy->add_gesture_recognizer(std::move(pan));

    // Only the press and one sub-slop move go through the helper, and both
    // return false. The claim on the next move is only reachable if those two
    // still reached the arbiter.
    REQUIRE_FALSE(should_yield_to_gesture(root, phase_event({40, 40}, MousePhase::press)));
    REQUIRE_FALSE(should_yield_to_gesture(root, phase_event({50, 40}, MousePhase::drag)));
    REQUIRE(began == 0);

    CHECK(should_yield_to_gesture(root, phase_event({100, 40}, MousePhase::drag)));
    CHECK(began == 1);
}

TEST_CASE("host gesture gate keeps raw delivery for an unclaimed candidate",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));
    spy->add_gesture_recognizer(std::make_unique<TapRecognizer>(2));

    View* drag_target = spy;
    auto press = phase_event({40, 40}, MousePhase::press);
    REQUIRE_FALSE(yield_to_gesture_with_handoff(root, drag_target, press));
    REQUIRE(drag_target == spy);
    REQUIRE(deliver_mouse_down(root, drag_target, press.window_position,
                               kModNone));

    auto drag = phase_event({80, 80}, MousePhase::drag);
    REQUIRE_FALSE(yield_to_gesture_with_handoff(root, drag_target, drag));
    REQUIRE(drag_target == spy);
    deliver_mouse_drag(root, drag_target, drag.window_position, kModNone);

    auto release = phase_event({80, 80}, MousePhase::release);
    REQUIRE_FALSE(yield_to_gesture_with_handoff(root, drag_target, release));
    REQUIRE(drag_target == spy);
    deliver_mouse_up(root, drag_target, release.window_position, kModNone, 1,
                     MouseUpHost{});

    CHECK(spy->downs == 1);
    CHECK(spy->drags == 1);
    CHECK(spy->ups == 1);
}

TEST_CASE("host gesture gate closes and clears a raw drag on mid-drag claim",
          "[view][input][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(40.0f);
    spy->add_gesture_recognizer(std::move(pan));

    View* drag_target = spy;
    auto press = phase_event({40, 40}, MousePhase::press);
    REQUIRE_FALSE(yield_to_gesture_with_handoff(root, drag_target, press));
    REQUIRE(deliver_mouse_down(root, drag_target, press.window_position,
                               kModNone));

    auto short_drag = phase_event({50, 40}, MousePhase::drag);
    REQUIRE_FALSE(yield_to_gesture_with_handoff(root, drag_target,
                                                short_drag));
    deliver_mouse_drag(root, drag_target, short_drag.window_position, kModNone);

    auto claim = phase_event({100, 40}, MousePhase::drag);
    REQUIRE(yield_to_gesture_with_handoff(root, drag_target, claim));
    REQUIRE(drag_target == nullptr);
    CHECK(spy->downs == 1);
    CHECK(spy->drags == 1);
    CHECK(spy->ups == 1);

    auto release = phase_event({100, 40}, MousePhase::release);
    (void)yield_to_gesture_with_handoff(root, drag_target, release);
    CHECK(spy->ups == 1);
}

TEST_CASE("cancelled release terminates a time-driven gesture claim",
          "[view][input][gesture][cancel]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto long_press = std::make_unique<LongPressRecognizer>();
    long_press->set_min_duration(0.25);
    int began = 0;
    int cancelled = 0;
    long_press->on_began = [&](GestureRecognizer&) { ++began; };
    long_press->on_cancelled = [&](GestureRecognizer&) { ++cancelled; };
    spy->add_gesture_recognizer(std::move(long_press));

    auto press = phase_event({40, 40}, MousePhase::press);
    REQUIRE(root.dispatch_gesture_pointer_event(press, 1.0));
    root.advance_gesture_recognizers(1.5);  // claim without WM_MOUSEMOVE
    REQUIRE(began == 1);

    auto cancel = phase_event({40, 40}, MousePhase::release);
    cancel.is_cancelled = true;
    REQUIRE(root.dispatch_gesture_pointer_event(cancel, 1.6));
    CHECK(cancelled == 1);
    CHECK_FALSE(root.has_time_driven_gestures());
}

// ── simulate_drag yields mid-loop ────────────────────────────────────────
//
// The headless drag must stop delivering the moment a recognizer takes the
// pointer, the way the hosts do — otherwise a headless test can observe drag
// behavior no real host would ever produce. The claim here lands MID-loop
// rather than on the first move, which is the case a first-move probe misses.
TEST_CASE("simulate_drag stops delivering moves once a recognizer claims",
          "[view][input][drag][gesture]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* spy = child.get();
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto pan = std::make_unique<PanRecognizer>();
    pan->set_min_distance(40.0f);
    spy->add_gesture_recognizer(std::move(pan));

    // Ten 10px steps; the pan claims on the fourth, where travel first
    // reaches the 40px slop.
    root.simulate_drag({40, 40}, {140, 40}, /*steps=*/10);

    CHECK(spy->downs == 1);
    CHECK(spy->drags == 3);   // steps 1-3 only; the claim ends delivery
    CHECK(spy->ups == 1);     // the press bracket still closes exactly once
}

// ── The headless simulator delivers what the hosts deliver ──────────────────
//
// View::simulate_click / simulate_drag are the idiom every headless UI test
// uses. They route through the same deliver_mouse_down/drag/up verbs the macOS
// window and plugin hosts call, so a test written against them exercises the
// real dispatch. When they instead called only the virtual on_mouse_* hooks,
// the `on_pointer_event` / `on_drag` callbacks — the ones the JS bridge
// installs, and the only channel a scripted UI ever sees — were unreachable
// from any headless test, so a script-driven control could be completely dead
// to input with the suite green.
namespace {

// Records every channel a dispatch can arrive on.
class ChannelSpy : public View {
public:
    ChannelSpy() {
        on_pointer_event = [this](const MouseEvent& e) {
            if (e.phase == MousePhase::press) ++modern_press;
            else if (e.phase == MousePhase::drag) ++modern_drag;
            else if (e.phase == MousePhase::release) ++modern_release;
        };
        on_drag = [this](Point p) { ++js_drag; last_drag = p; };
    }
    void on_mouse_down(Point) override { ++legacy_down; }
    void on_mouse_drag(Point) override { ++legacy_drag; }
    void on_mouse_up(Point) override { ++legacy_up; }

    int modern_press = 0, modern_drag = 0, modern_release = 0;
    int legacy_down = 0, legacy_drag = 0, legacy_up = 0;
    int js_drag = 0;
    Point last_drag{};
};

}  // namespace

TEST_CASE("simulate_click delivers the modern channel, not only the legacy one",
          "[view][input][simulate]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<ChannelSpy>();
    ChannelSpy* spy = child.get();
    spy->set_bounds({20, 20, 100, 100});
    root.add_child(std::move(child));

    root.simulate_click({60, 60});

    CHECK(spy->modern_press == 1);
    CHECK(spy->modern_release == 1);
    CHECK(spy->legacy_down == 1);
    CHECK(spy->legacy_up == 1);
}

TEST_CASE("simulate_drag delivers every drag tick on the modern and JS channels",
          "[view][input][simulate]") {
    View root;
    root.set_bounds({0, 0, 400, 300});
    auto child = std::make_unique<ChannelSpy>();
    ChannelSpy* spy = child.get();
    spy->set_bounds({20, 20, 100, 100});
    root.add_child(std::move(child));

    root.simulate_drag({60, 100}, {60, 40}, 4);

    CHECK(spy->modern_press == 1);
    CHECK(spy->modern_drag == 4);
    CHECK(spy->modern_release == 1);
    CHECK(spy->legacy_drag == 4);
    // `on_drag` is the channel WidgetBridge::registerPointer wires to emit
    // `pointermove` into JS. Four ticks, each localized to the spy's own space
    // (the final one at the drag end, 60,40 → 40,20 inside a 20,20 child).
    CHECK(spy->js_drag == 4);
    CHECK_THAT(spy->last_drag.x, WithinAbs(40.0f, 1e-3f));
    CHECK_THAT(spy->last_drag.y, WithinAbs(20.0f, 1e-3f));
}

// ── The simulator can describe a pointer that is not a mouse ────────────────
//
// Every synthesized event used to leave `pointer_type` at mouse, `pressure` at
// 0.5, `modifiers` at 0, `button` at left and `pointer_id` at 0, and the press
// and release verbs took no device arguments at all — only the drag verb did.
// So "a touch and a mouse produce the same transaction" was unfalsifiable
// headlessly (the press, where widgets latch, always said mouse), and
// multi-touch was unreachable: the arbiter keys sessions on `pointer_id` and
// the pinch/rotate recognizers key their touch maps on it, but nothing could
// drive a second id.
namespace {

// Captures the MouseEvent each phase arrives with, on the modern channel.
struct DeviceSpy final : View {
    std::vector<MouseEvent> presses, drags, releases;
    int legacy_downs = 0, legacy_ups = 0;

    void on_mouse_event(const MouseEvent& e) override {
        if (e.phase == MousePhase::press) presses.push_back(e);
        else if (e.phase == MousePhase::drag) drags.push_back(e);
        else if (e.phase == MousePhase::release) releases.push_back(e);
    }
    void on_mouse_down(Point) override { ++legacy_downs; }
    void on_mouse_up(Point) override { ++legacy_ups; }
};

DeviceSpy& add_device_spy(View& root) {
    auto child = std::make_unique<DeviceSpy>();
    DeviceSpy* spy = child.get();
    spy->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));
    return *spy;
}

}  // namespace

TEST_CASE("simulate_click reports the given device on the press channel",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    View::SimulatedPointer touch;
    touch.type = PointerType::touch;
    touch.pressure = 0.75f;
    touch.pointer_id = 3;
    root.simulate_click({60, 60}, touch);

    REQUIRE(spy.presses.size() == 1);
    CHECK(spy.presses[0].pointer_type == PointerType::touch);
    CHECK(spy.presses[0].isTouch());
    CHECK_THAT(spy.presses[0].pressure, WithinAbs(0.75f, 1e-6f));
    CHECK(spy.presses[0].pointer_id == 3);
}

TEST_CASE("simulate_click reports the given device on the release channel",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    View::SimulatedPointer pen;
    pen.type = PointerType::pen;
    pen.pressure = 0.25f;
    pen.pointer_id = 2;
    root.simulate_click({60, 60}, pen);

    REQUIRE(spy.releases.size() == 1);
    CHECK(spy.releases[0].pointer_type == PointerType::pen);
    CHECK(spy.releases[0].isPen());
    CHECK_THAT(spy.releases[0].pressure, WithinAbs(0.25f, 1e-6f));
    CHECK(spy.releases[0].pointer_id == 2);
}

TEST_CASE("simulate_drag reports the given device on every phase",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    View::SimulatedPointer touch;
    touch.type = PointerType::touch;
    touch.pressure = 0.9f;
    touch.pointer_id = 5;
    root.simulate_drag({40, 40}, {120, 40}, /*steps=*/4, touch);

    REQUIRE(spy.presses.size() == 1);
    REQUIRE(spy.drags.size() == 4);
    REQUIRE(spy.releases.size() == 1);
    for (const MouseEvent* e : {&spy.presses[0], &spy.drags[0], &spy.drags[3],
                                &spy.releases[0]}) {
        CHECK(e->pointer_type == PointerType::touch);
        CHECK_THAT(e->pressure, WithinAbs(0.9f, 1e-6f));
        CHECK(e->pointer_id == 5);
    }
}

TEST_CASE("simulate_click carries modifiers and button through delivery",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    View::SimulatedPointer right;
    right.button = MouseButton::right;
    right.modifiers = kModShift | kModAlt;
    root.simulate_click({60, 60}, right);

    REQUIRE(spy.presses.size() == 1);
    CHECK(spy.presses[0].button == MouseButton::right);
    CHECK(spy.presses[0].isShiftDown());
    CHECK(spy.presses[0].isAltDown());
    REQUIRE(spy.releases.size() == 1);
    CHECK(spy.releases[0].button == MouseButton::right);
    CHECK(spy.releases[0].isShiftDown());

    // The legacy channels carry no button identity and stay primary-only, the
    // same rule deliver_mouse_down/up already applied to the hosts.
    CHECK(spy.legacy_downs == 0);
    CHECK(spy.legacy_ups == 0);
}

TEST_CASE("simulate_click without a device argument is an unmodified left mouse",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    root.simulate_click({60, 60});

    REQUIRE(spy.presses.size() == 1);
    REQUIRE(spy.releases.size() == 1);
    for (const MouseEvent* e : {&spy.presses[0], &spy.releases[0]}) {
        CHECK(e->pointer_type == PointerType::mouse);
        CHECK_THAT(e->pressure, WithinAbs(0.5f, 1e-6f));
        CHECK(e->button == MouseButton::left);
        CHECK(e->modifiers == kModNone);
        CHECK(e->pointer_id == 0);
    }
    CHECK(spy.legacy_downs == 1);
    CHECK(spy.legacy_ups == 1);
}

TEST_CASE("simulate_drag without a device argument is an unmodified left mouse",
          "[view][input][simulate][pointer-type]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    DeviceSpy& spy = add_device_spy(root);

    root.simulate_drag({40, 40}, {120, 40}, /*steps=*/3);

    REQUIRE(spy.drags.size() == 3);
    for (const MouseEvent& e : spy.drags) {
        CHECK(e.pointer_type == PointerType::mouse);
        CHECK_THAT(e.pressure, WithinAbs(0.5f, 1e-6f));
        CHECK(e.button == MouseButton::left);
        CHECK(e.pointer_id == 0);
    }
}

// ── Multi-touch is now reachable from the simulator ─────────────────────────
//
// The load-bearing pair. PinchRecognizer keys `touches_` on `pointer_id`, so it
// only pairs two fingers when they arrive under DIFFERENT ids. One finger is
// held down through the raw arbiter entry point; the other is driven entirely by
// `simulate_drag`. Before the simulator could name a pointer id, the second
// finger overwrote the first and no pinch could ever form headlessly — which the
// companion case below pins by driving the identical sequence at the default id.
namespace {

MouseEvent touch_event(Point p, MousePhase phase, int pointer_id) {
    MouseEvent e = phase_event(p, phase);
    e.pointer_type = PointerType::touch;
    e.pointer_id = pointer_id;
    return e;
}

struct PinchFixture {
    View root;
    PhaseCounter* surface = nullptr;
    PinchRecognizer* pinch = nullptr;
    int began = 0;

    PinchFixture() {
        root.set_bounds({0, 0, 200, 200});
        auto child = std::make_unique<PhaseCounter>();
        surface = child.get();
        surface->set_bounds({0, 0, 200, 200});
        root.add_child(std::move(child));

        auto recognizer = std::make_unique<PinchRecognizer>();
        pinch = recognizer.get();
        pinch->on_began = [this](GestureRecognizer&) { ++began; };
        surface->add_gesture_recognizer(std::move(recognizer));
    }
};

}  // namespace

TEST_CASE("a simulated drag under a second pointer id pinches against a held touch",
          "[view][input][simulate][multitouch]") {
    PinchFixture fx;

    // Finger A goes down at x=40 and stays down.
    auto first = touch_event({40, 100}, MousePhase::press, /*pointer_id=*/0);
    REQUIRE(fx.root.dispatch_gesture_pointer_event(first));
    REQUIRE(fx.began == 0);

    // Finger B is entirely simulated, spreading from x=60 to x=160.
    View::SimulatedPointer second;
    second.type = PointerType::touch;
    second.pointer_id = 1;
    fx.root.simulate_drag({60, 100}, {160, 100}, /*steps=*/10, second);

    CHECK(fx.began == 1);
    CHECK(fx.pinch->scale() > 1.0f);
}

// The control for the case above: identical sequence, identical geometry, only
// the simulated finger's id left at the default. Both touches land in the same
// map slot, the recognizer never holds a pair, and no pinch forms — which is
// exactly the state the simulator was stuck in before it could name an id.
TEST_CASE("a simulated drag reusing the primary pointer id cannot pinch",
          "[view][input][simulate][multitouch]") {
    PinchFixture fx;

    auto first = touch_event({40, 100}, MousePhase::press, /*pointer_id=*/0);
    REQUIRE(fx.root.dispatch_gesture_pointer_event(first));

    View::SimulatedPointer second;
    second.type = PointerType::touch;
    second.pointer_id = 0;
    fx.root.simulate_drag({60, 100}, {160, 100}, /*steps=*/10, second);

    CHECK(fx.began == 0);
}

// Recognizer-level proof that the id survives the whole path — arbiter session
// lookup included — rather than only being observable via a pinch's arithmetic.
namespace {

struct PointerIdRecorder final : GestureRecognizer {
    std::vector<int> ids;
    std::vector<PointerType> types;

protected:
    void on_pointer_event(const MouseEvent& event, const GestureContext&) override {
        ids.push_back(event.pointer_id);
        types.push_back(event.pointer_type);
    }
};

}  // namespace

TEST_CASE("a simulated drag hands its device to the recognizers on the chain",
          "[view][input][simulate][multitouch]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    auto child = std::make_unique<PhaseCounter>();
    auto* surface = child.get();
    surface->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    auto recorder = std::make_unique<PointerIdRecorder>();
    auto* recorded = recorder.get();
    surface->add_gesture_recognizer(std::move(recorder));

    View::SimulatedPointer pen;
    pen.type = PointerType::pen;
    pen.pointer_id = 9;
    root.simulate_drag({40, 40}, {120, 40}, /*steps=*/3, pen);

    // press + 3 moves + release.
    REQUIRE(recorded->ids.size() == 5);
    for (int id : recorded->ids) CHECK(id == 9);
    for (PointerType t : recorded->types) CHECK(t == PointerType::pen);
}
