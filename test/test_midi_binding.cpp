// MIDI control bus + bind_midi (controls send MIDI out) and
// MidiParameterMap (incoming MIDI CC -> parameter, with learn).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/midi/midi_control_bus.hpp>
#include <pulp/state/midi_parameter_map.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/midi_binding.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp;
using Catch::Matchers::WithinAbs;

namespace {
void populate(state::StateStore& s) {
    s.add_parameter({.id = 1, .name = "Cutoff", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    s.add_parameter({.id = 2, .name = "Res", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
}
} // namespace

TEST_CASE("MidiControlBus publishes UI events to the audio thread", "[midi][control-bus]") {
    midi::MidiControlBus bus;
    REQUIRE(bus.send_cc(0, 74, 100));
    REQUIRE(bus.send_note_on(0, 60, 110));

    midi::MidiBuffer out;
    bus.drain_into(out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].is_cc());
    REQUIRE(out[0].cc_number() == 74);
    REQUIRE(out[0].cc_value() == 100);
    REQUIRE(out[1].is_note_on());
    REQUIRE(out[1].note() == 60);

    // Drained: a second drain yields nothing.
    midi::MidiBuffer again;
    bus.drain_into(again);
    REQUIRE(again.size() == 0);
}

TEST_CASE("bind_midi_cc makes a knob emit Control Change", "[midi][bind]") {
    midi::MidiControlBus bus;
    view::Knob knob;
    view::bind_midi_cc(knob, bus, 0, 74);
    knob.on_change(1.0f);   // user moves the knob to the top
    knob.on_change(0.0f);   // and to the bottom

    midi::MidiBuffer out;
    bus.drain_into(out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].cc_value() == 127);
    REQUIRE(out[1].cc_value() == 0);
}

TEST_CASE("bind_midi_note plays a note while a toggle is on", "[midi][bind]") {
    midi::MidiControlBus bus;
    view::ToggleButton button;
    view::bind_midi_note(button, bus, 0, 64, 100);
    button.on_toggle(true);
    button.on_toggle(false);

    midi::MidiBuffer out;
    bus.drain_into(out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].is_note_on());
    REQUIRE(out[0].note() == 64);
    REQUIRE(out[1].is_note_off());
}

TEST_CASE("MidiParameterMap routes a mapped CC to its parameter", "[state][midi-map]") {
    state::StateStore store;
    populate(store);
    state::MidiParameterMap map;
    map.set_mapping(0, 74, 1);
    map.pump(); // apply the UI command

    map.handle_cc(store, 0, 74, 127);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    map.handle_cc(store, 0, 74, 64);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(64.0f / 127.0f, 1e-3f));

    // An unmapped CC does nothing.
    map.handle_cc(store, 0, 75, 127);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(64.0f / 127.0f, 1e-3f));
}

TEST_CASE("MidiParameterMap learn binds the next CC", "[state][midi-map]") {
    state::StateStore store;
    populate(store);
    state::MidiParameterMap map;

    map.arm_learn(2);
    map.pump();
    REQUIRE(map.learn_armed());
    // The next CC binds to param 2 and is applied.
    map.handle_cc(store, 5, 20, 127);
    REQUIRE_FALSE(map.learn_armed());
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(1.0f, 1e-3f));
    // Subsequent CCs on the learned controller keep driving it.
    map.handle_cc(store, 5, 20, 0);
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("MidiParameterMap omni channel + clear", "[state][midi-map]") {
    state::StateStore store;
    populate(store);
    state::MidiParameterMap map;
    map.set_mapping(state::MidiParameterMap::kOmni, 30, 1);
    map.pump();
    // Any channel drives it.
    map.handle_cc(store, 9, 30, 127);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));

    map.clear(1);
    map.pump();
    map.handle_cc(store, 9, 30, 0);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f)); // unchanged
}
