// ParameterEdit + bind_parameter — host-automation gesture wiring.
//
// Verifies that UI parameter edits are bracketed in store gestures (so a
// DAW records and plays back the automation), that values are written, and
// that the in-flight display cache tracks the drag. Gestures are observed
// through StateStore::set_gesture_callbacks (the same hook the format
// adapters use to emit host Begin/EndParameterChangeGesture events).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <functional>
#include <pulp/state/parameter_edit.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/parameter_binding.hpp>
#include <pulp/view/ui_components.hpp>
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
    s.add_parameter({.id = 4, .name = "Choice", .unit = "", .range = {0.0f, 3.0f, 0.0f, 1.0f}});
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
    REQUIRE_FALSE(knob.has_modulated_value()); // nothing modulates it yet

    store.set_mod_offset(1, 6.0f); // what the CLAP adapter writes
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
    for (int i = 0; i < 3; ++i)
        store.pump_listeners();
    REQUIRE_FALSE(fader.has_modulated_value());

    // Opt in: the plugin publishes what its internal LFO plays (plain units).
    store.set_display_modulation(2, 6.0f);
    store.pump_listeners();
    REQUIRE(fader.has_modulated_value());
    REQUIRE_THAT(fader.modulated_display_value(), WithinAbs(0.75f, 1e-5f));
    REQUIRE(store.get_value(2) == 0.0f); // never a write
    REQUIRE(log.begins.empty());         // never a host gesture

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
    store.pump_listeners(); // the watch is gone: no callback into the knob
    REQUIRE_THAT(knob.modulated_display_value(), WithinAbs((3.0f + 12.0f) / 24.0f, 1e-5f));
}

// ── Every bound control: record, play back, show host modulation ────────────

TEST_CASE("Every bound control records the user's change and follows playback without a gesture",
          "[view][parameter-binding][bind-all]") {
    // One table over every control bind_parameter accepts. For each: a user
    // change is a recorded gesture (begin + end on the parameter, value
    // written inside it); host playback moves the control and opens NO
    // gesture (playback must never look like a user edit to the host).
    struct Case {
        const char* name;
        state::ParamID id;
        std::function<view::ParameterBinding(state::StateStore&)> bind;
        std::function<void()> user_edit;
        float playback_value;
        std::function<bool(state::StateStore&)> follows;
    };
    view::Knob knob;
    knob.set_bounds({0, 0, 40, 40});
    view::Fader fader;
    fader.set_bounds({0, 0, 24, 120});
    view::RangeSlider range;
    range.set_bounds({0, 0, 200, 24});
    // A stepped slider: playback of an off-step value must not be quantised
    // and written back (the echo a silent sync exists to prevent).
    range.set_step(0.25f);
    view::XYPad pad;
    pad.set_bounds({0, 0, 200, 200});
    view::Toggle toggle;
    toggle.set_bounds({0, 0, 48, 24});
    view::Checkbox box;
    box.set_bounds({0, 0, 24, 24});
    view::ToggleButton button;
    button.set_bounds({0, 0, 80, 40});
    view::ComboBox combo;
    combo.set_items({"Sine", "Saw", "Square", "Noise"});
    view::DesignStepper stepper({"1/4", "1/8", "1/16", "1/32"}, 0);

    const auto near = [](float a, float b) { return std::abs(a - b) < 1e-4f; };
    std::vector<Case> cases;
    cases.push_back({"knob", 1, [&](auto& st) { return view::bind_parameter(knob, st, 1); },
                     [&] { knob.simulate_drag({20, 30}, {20, 10}); }, -6.0f,
                     [&](auto& st) { return near(knob.value(), st.get_normalized(1)); }});
    cases.push_back({"fader", 2, [&](auto& st) { return view::bind_parameter(fader, st, 2); },
                     [&] { fader.simulate_drag({12, 100}, {12, 20}); }, -9.0f,
                     [&](auto& st) { return near(fader.value(), st.get_normalized(2)); }});
    cases.push_back({"range slider", 1,
                     [&](auto& st) { return view::bind_parameter(range, st, 1); },
                     [&] {
                         view::MouseEvent down;
                         down.position = {10, 12};
                         down.is_down = true;
                         range.on_mouse_event(down);
                         range.on_mouse_drag({190, 12});
                         view::MouseEvent up = down;
                         up.position = {190, 12};
                         up.is_down = false;
                         range.on_mouse_event(up);
                     },
                     -3.0f,
                     [&](auto&) {
                         return near(range.value(), 0.5f); /* its nearest step */
                     }});
    cases.push_back({"toggle", 3, [&](auto& st) { return view::bind_parameter(toggle, st, 3); },
                     [&] { toggle.simulate_click({24, 12}); }, 0.0f,
                     [&](auto&) { return !toggle.is_on(); }});
    cases.push_back({"checkbox", 3, [&](auto& st) { return view::bind_parameter(box, st, 3); },
                     [&] { box.simulate_click({12, 12}); }, 0.0f,
                     [&](auto&) { return !box.is_checked(); }});
    cases.push_back(
        {"toggle button", 3, [&](auto& st) { return view::bind_parameter(button, st, 3); },
         [&] { button.simulate_click({40, 20}); }, 0.0f, [&](auto&) { return !button.is_on(); }});
    cases.push_back({"combo box", 4, [&](auto& st) { return view::bind_parameter(combo, st, 4); },
                     [&] {
                         REQUIRE(combo.on_change);
                         combo.on_change(2);
                     },
                     3.0f, [&](auto&) { return combo.selected() == 3; }});
    cases.push_back({"stepper", 4, [&](auto& st) { return view::bind_parameter(stepper, st, 4); },
                     [&] {
                         REQUIRE(stepper.on_select);
                         stepper.on_select(1);
                     },
                     2.0f, [&](auto&) { return stepper.selected() == 2; }});

    for (auto& c : cases) {
        INFO(c.name);
        state::StateStore store;
        populate(store);
        GestureLog log;
        log.attach(store);
        auto binding = c.bind(store);
        const float before = store.get_value(c.id);

        c.user_edit();
        CHECK(log.begins == std::vector<state::ParamID>{c.id});
        CHECK(log.ends == std::vector<state::ParamID>{c.id});
        CHECK(store.get_value(c.id) != before);

        store.set_value(c.id, c.playback_value); // host automation playback
        store.pump_listeners();
        CHECK(c.follows(store));
        CHECK(store.get_value(c.id) == c.playback_value); // no echo write
        CHECK(log.begins.size() == 1);                    // playback opened no gesture
        CHECK(log.ends.size() == 1);
    }

    // The XY pad: one drag records both axes; playback moves the puck.
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    auto binding = view::bind_parameter(pad, store, 1, 2);
    pad.simulate_drag({20, 180}, {180, 20}, 8);
    CHECK(log.begins.size() == 2);
    CHECK(log.ends.size() == 2);
    store.set_value(1, -12.0f);
    store.set_value(2, 12.0f);
    store.pump_listeners();
    CHECK(near(pad.x_value(), 0.0f));
    CHECK(near(pad.y_value(), 1.0f));
    CHECK(log.begins.size() == 2);
}

TEST_CASE("Host modulation of a bound range slider or XY pad moves its one indicator",
          "[view][parameter-binding][modulation][bind-all]") {
    state::StateStore store;
    populate(store);
    GestureLog log;
    log.attach(store);
    view::RangeSlider range;
    view::XYPad pad;
    auto rb = view::bind_parameter(range, store, 1);
    auto pb = view::bind_parameter(pad, store, 2, 3);

    // Negative: nothing modulates either parameter, nothing is shown.
    store.pump_listeners();
    REQUIRE_FALSE(range.has_modulated_value());
    REQUIRE_FALSE(pad.has_modulated_value());

    // The CLAP adapter's mod offset reaches both with no plugin code.
    store.set_mod_offset(1, 6.0f);
    store.set_mod_offset(2, -6.0f);
    store.pump_listeners();
    REQUIRE(range.has_modulated_value());
    CHECK_THAT(range.modulated_display_value(), WithinAbs(0.75f, 1e-5f));
    REQUIRE(pad.has_modulated_value());
    CHECK_THAT(pad.modulated_display_x(), WithinAbs(0.25f, 1e-5f));
    CHECK_THAT(pad.modulated_display_y(), WithinAbs(pad.y_value(), 1e-5f)); // unmodulated axis
    CHECK(store.get_value(1) == 0.0f);
    CHECK(log.begins.empty());

    store.set_mod_offset(1, 0.0f);
    store.set_mod_offset(2, 0.0f);
    store.pump_listeners();
    CHECK_FALSE(range.has_modulated_value());
    CHECK_FALSE(pad.has_modulated_value());
}

TEST_CASE("A discrete bound control shows its base under host modulation",
          "[view][parameter-binding][modulation][bind-all]") {
    // A stepped parameter has no position between states for a modulator to
    // move it to: the binding keeps showing the state the user set.
    state::StateStore store;
    populate(store);
    view::ComboBox combo;
    combo.set_items({"Sine", "Saw", "Square", "Noise"});
    auto binding = view::bind_parameter(combo, store, 4);
    store.set_mod_offset(4, 2.0f);
    store.pump_listeners();
    CHECK(combo.selected() == 0);
}
