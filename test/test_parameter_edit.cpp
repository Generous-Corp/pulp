// ParameterEdit + bind_parameter — host-automation gesture wiring.
//
// Verifies that UI parameter edits are bracketed in store gestures (so a
// DAW records and plays back the automation), that values are written, and
// that the in-flight display cache tracks the drag. Gestures are observed
// through StateStore::set_gesture_callbacks (the same hook the format
// adapters use to emit host Begin/EndParameterChangeGesture events).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/widgets.hpp>
#include <vector>

using namespace pulp;
using Catch::Matchers::WithinAbs;

namespace {

struct GestureLog {
    std::vector<state::ParamID> begins;
    std::vector<state::ParamID> ends;
    void attach(state::StateStore& s) {
        s.set_gesture_callbacks([this](state::ParamID id) { begins.push_back(id); },
                                [this](state::ParamID id) { ends.push_back(id); });
    }
};

void populate(state::StateStore& s) {
    s.add_parameter({.id = 1, .name = "X", .unit = "", .range = {-12.0f, 12.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = 2, .name = "Y", .unit = "", .range = {-12.0f, 12.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = 3, .name = "Toggle", .unit = "", .range = {0.0f, 1.0f, 0.0f, 1.0f}});
}

} // namespace

TEST_CASE("ParameterEdit brackets writes in host gestures", "[state][parameter-edit]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);

    state::ParameterEdit edit(store);
    REQUIRE_FALSE(edit.active());

    edit.begin({1, 2});
    REQUIRE(edit.active());
    REQUIRE(log.begins == std::vector<state::ParamID>{1, 2});
    REQUIRE(log.ends.empty());

    edit.set(1, 7.0f);
    edit.set(2, -3.0f);
    REQUIRE_THAT(store.get_value(1), WithinAbs(7.0f, 1e-6f));
    REQUIRE_THAT(store.get_value(2), WithinAbs(-3.0f, 1e-6f));

    edit.finish();
    REQUIRE_FALSE(edit.active());
    REQUIRE(log.ends == std::vector<state::ParamID>{1, 2});
}

TEST_CASE("ParameterEdit display cache tracks the in-flight value", "[state][parameter-edit]") {
    state::StateStore store;
    populate(store);
    state::ParameterEdit edit(store);
    // Outside a gesture: falls back to the live store value.
    REQUIRE_THAT(edit.display_value(1, 5.0f), WithinAbs(5.0f, 1e-6f));
    edit.begin({1});
    edit.set(1, 9.0f);
    // The cache shows the user's value even if the store is later changed
    // out from under the UI (host echo / per-block clobber).
    store.set_value(1, -2.0f);
    REQUIRE_THAT(edit.display_value(1, store.get_value(1)), WithinAbs(9.0f, 1e-6f));
    edit.finish();
    REQUIRE_THAT(edit.display_value(1, store.get_value(1)), WithinAbs(-2.0f, 1e-6f));
}

TEST_CASE("ParameterEdit ends an open gesture on destruction", "[state][parameter-edit]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    {
        state::ParameterEdit edit(store);
        edit.begin({1});
        edit.set(1, 4.0f);
        // no explicit finish()
    }
    REQUIRE(log.begins == std::vector<state::ParamID>{1});
    REQUIRE(log.ends == std::vector<state::ParamID>{1});
}

TEST_CASE("bind_parameter records XY pad automation", "[view][parameter-binding]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);

    view::XYPad pad;
    pad.set_bounds({0, 0, 200, 200});
    auto binding = view::bind_parameter(pad, store, 1, 2);

    // A drag: press → move → release. simulate_drag drives the widget's
    // on_mouse_down/drag/up, which fire gesture + change callbacks.
    pad.simulate_drag({20, 180}, {180, 20}, 8);

    // Both params opened and closed a gesture (in some order).
    REQUIRE(log.begins.size() == 2);
    REQUIRE(log.ends.size() == 2);
    // And the values moved toward the drag end (x high, y high since up = high).
    REQUIRE(store.get_normalized(1) > 0.5f);
    REQUIRE(store.get_normalized(2) > 0.5f);
}

TEST_CASE("bind_parameter records a toggle press as a one-shot gesture",
          "[view][parameter-binding]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);

    view::ToggleButton button;
    button.set_bounds({0, 0, 80, 40});
    auto binding = view::bind_parameter(button, store, 3);

    REQUIRE(store.get_value(3) < 0.5f);
    button.simulate_click({40, 20});
    REQUIRE(store.get_value(3) >= 0.5f);
    REQUIRE(log.begins == std::vector<state::ParamID>{3});
    REQUIRE(log.ends == std::vector<state::ParamID>{3});
}

TEST_CASE("bind_parameter records RangeSlider, Toggle, and Checkbox edits",
          "[view][parameter-binding]") {
    SECTION("RangeSlider drags a continuous gesture") {
        state::StateStore store;
        populate(store);
        GestureLog log;
        log.attach(store);
        view::RangeSlider slider;
        slider.set_bounds({0, 0, 200, 24});
        auto b = view::bind_parameter(slider, store, 1);
        // RangeSlider handles the rich on_mouse_event (its live host path).
        auto ev = [](float x, bool down) {
            view::MouseEvent e;
            e.position = {x, 12};
            e.is_down = down;
            return e;
        };
        slider.on_mouse_event(ev(10, true));   // press
        slider.on_mouse_drag({190, 12});       // move to the far end
        slider.on_mouse_event(ev(190, false)); // release
        REQUIRE(log.begins == std::vector<state::ParamID>{1});
        REQUIRE(log.ends == std::vector<state::ParamID>{1});
        REQUIRE(store.get_normalized(1) > 0.5f);
    }
    SECTION("Toggle flips as a one-shot gesture") {
        state::StateStore store;
        populate(store);
        GestureLog log;
        log.attach(store);
        view::Toggle toggle;
        toggle.set_bounds({0, 0, 48, 24});
        auto b = view::bind_parameter(toggle, store, 3);
        toggle.simulate_click({24, 12});
        REQUIRE(store.get_value(3) >= 0.5f);
        REQUIRE(log.begins == std::vector<state::ParamID>{3});
        REQUIRE(log.ends == std::vector<state::ParamID>{3});
    }
    SECTION("Checkbox flips as a one-shot gesture") {
        state::StateStore store;
        populate(store);
        GestureLog log;
        log.attach(store);
        view::Checkbox box;
        box.set_bounds({0, 0, 24, 24});
        auto b = view::bind_parameter(box, store, 3);
        box.simulate_click({12, 12});
        REQUIRE(store.get_value(3) >= 0.5f);
        REQUIRE(log.begins == std::vector<state::ParamID>{3});
        REQUIRE(log.ends == std::vector<state::ParamID>{3});
    }
}

TEST_CASE("bind_parameter reflects host automation playback back to the widget",
          "[view][parameter-binding]") {
    state::StateStore store;
    populate(store);
    view::Knob knob;
    auto binding = view::bind_parameter(knob, store, 1);
    // Host moves the parameter (automation playback / preset load).
    store.set_normalized(1, 0.75f);
    REQUIRE_THAT(knob.value(), WithinAbs(0.75f, 1e-4f));
}
