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

TEST_CASE("bind_parameter follows host automation playback after pump",
          "[view][parameter-binding]") {
    // The generic SDK path: the host writes the store (automation playback /
    // a host-side edit), and pumping the store on the UI thread (the editor
    // idle pump) propagates it to the bound widget so it visibly moves —
    // exactly as if the user had dragged it.
    state::StateStore store;
    populate(store);
    view::Knob knob;
    auto binding = view::bind_parameter(knob, store, 1);
    const float at_default = knob.value();                  // normalized(0) = 0.5
    REQUIRE_THAT(at_default, WithinAbs(0.5f, 1e-5f));

    store.set_value(1, 6.0f);                               // host automation
    store.pump_listeners();                                  // editor idle pump (UI thread)
    REQUIRE_THAT(knob.value(), WithinAbs(store.get_normalized(1), 1e-5f));
    REQUIRE(knob.value() > at_default);                      // the widget moved to track it
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

// ── Bind once: automation, playback and modulated display ───────────────────

TEST_CASE("A bound knob records a host gesture and follows playback with no other code",
          "[view][parameter-binding]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    view::Knob knob;
    knob.set_bounds({0, 0, 40, 40});
    auto binding = view::bind_parameter(knob, store, 1);

    // A drag is one recorded gesture with the value written inside it.
    knob.simulate_drag({20, 30}, {20, 10});
    REQUIRE(log.begins == std::vector<state::ParamID>{1});
    REQUIRE(log.ends == std::vector<state::ParamID>{1});
    REQUIRE(store.get_value(1) > 0.0f);

    // Playback moves the knob.
    store.set_value(1, -9.0f);
    store.pump_listeners();
    REQUIRE_THAT(knob.value(), WithinAbs(store.get_normalized(1), 1e-5f));
}

TEST_CASE("Bound discrete controls record one gesture per change and follow playback",
          "[view][parameter-binding]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);

    view::Toggle toggle;
    auto tb = view::bind_parameter(toggle, store, 3);
    REQUIRE(toggle.on_toggle);
    toggle.on_toggle(true);
    REQUIRE(log.begins == std::vector<state::ParamID>{3});
    REQUIRE(log.ends == std::vector<state::ParamID>{3});
    REQUIRE(store.get_value(3) == 1.0f);
    store.set_value(3, 0.0f);
    store.pump_listeners();
    REQUIRE_FALSE(toggle.is_on());

    view::ToggleButton button;
    auto bb = view::bind_parameter(button, store, 3);
    REQUIRE(button.on_toggle);
    button.on_toggle(true);
    REQUIRE(log.begins.size() == 2);
    REQUIRE(log.ends.size() == 2);
    store.set_value(3, 0.0f);
    store.pump_listeners();
    REQUIRE_FALSE(button.is_on());
}

TEST_CASE("Host modulation of a bound knob draws its played value with no plugin code",
          "[view][parameter-binding][modulation]") {
    // A CLAP host's parameter modulation reaches the store as the mod offset
    // (the adapter's CLAP_EVENT_PARAM_MOD path); the bound knob shows it.
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    view::Knob knob;
    auto binding = view::bind_parameter(knob, store, 1);
    REQUIRE_FALSE(knob.has_modulated_value());   // nothing modulates it yet

    store.set_mod_offset(1, 6.0f);               // what the CLAP adapter writes
    store.pump_listeners();
    REQUIRE(knob.has_modulated_value());
    REQUIRE_THAT(knob.modulated_display_value(), WithinAbs((6.0f + 12.0f) / 24.0f, 1e-5f));
    // Display only: the base, the store value and the host lane are untouched.
    REQUIRE_THAT(knob.value(), WithinAbs(0.5f, 1e-5f));
    REQUIRE(store.get_value(1) == 0.0f);
    REQUIRE(log.begins.empty());

    store.set_mod_offset(1, 0.0f);
    store.pump_listeners();
    REQUIRE_FALSE(knob.has_modulated_value());
}

TEST_CASE("A plugin's own modulation shows only when it publishes one",
          "[view][parameter-binding][modulation]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    view::Fader fader;
    auto binding = view::bind_parameter(fader, store, 2);

    // Negative: a plugin that never opts in shows no modulation UI at all.
    for (int i = 0; i < 3; ++i) store.pump_listeners();
    REQUIRE_FALSE(fader.has_modulated_value());

    // Opt in: the plugin publishes what its internal LFO plays (plain units).
    store.set_display_modulation(2, 6.0f);
    store.pump_listeners();
    REQUIRE(fader.has_modulated_value());
    REQUIRE_THAT(fader.modulated_display_value(), WithinAbs(0.75f, 1e-5f));
    REQUIRE(store.get_value(2) == 0.0f);         // never a write
    REQUIRE(log.begins.empty());                 // never a host gesture

    // A published value wins over a host offset; clearing hands back to it.
    store.set_mod_offset(2, -6.0f);
    store.pump_listeners();
    REQUIRE_THAT(fader.modulated_display_value(), WithinAbs(0.75f, 1e-5f));
    store.clear_display_modulation(2);
    store.pump_listeners();
    REQUIRE_THAT(fader.modulated_display_value(), WithinAbs(0.25f, 1e-5f));
}

TEST_CASE("Dropping the binding stops the modulated display",
          "[view][parameter-binding][modulation]") {
    state::StateStore store;
    populate(store);
    view::Knob knob;
    {
        auto binding = view::bind_parameter(knob, store, 1);
        store.set_mod_offset(1, 3.0f);
        store.pump_listeners();
        REQUIRE(knob.has_modulated_value());
    }
    store.set_mod_offset(1, -3.0f);
    store.pump_listeners();   // the watch is gone: no callback into the knob
    REQUIRE_THAT(knob.modulated_display_value(), WithinAbs((3.0f + 12.0f) / 24.0f, 1e-5f));
}
