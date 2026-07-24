// MIDI control bus + bind_midi (controls send MIDI out) and
// MidiParameterMap (incoming MIDI CC -> parameter, with learn).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/midi/midi_control_bus.hpp>
#include <pulp/state/midi_parameter_map.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/midi_binding.hpp>
#include <pulp/view/widgets.hpp>

#include <limits>

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
    knob.on_change(1.0f); // user moves the knob to the top
    knob.on_change(0.0f); // and to the bottom

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

TEST_CASE("bind_midi_cc covers fader, range slider, and discrete controls", "[midi][bind]") {
    midi::MidiControlBus bus;
    midi::MidiBuffer out;

    view::Fader fader;
    view::bind_midi_cc(fader, bus, 1, 7);
    fader.on_change(0.5f);
    bus.drain_into(out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].channel() == 1);
    REQUIRE(out[0].cc_number() == 7);
    REQUIRE(out[0].cc_value() == 64);

    view::RangeSlider slider;
    view::bind_midi_cc(slider, bus, 0, 10);
    slider.on_change(1.0f);
    out = midi::MidiBuffer{};
    bus.drain_into(out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].cc_value() == 127);

    view::ToggleButton button;
    view::bind_midi_cc(button, bus, 0, 64);
    button.on_toggle(true);
    button.on_toggle(false);
    out = midi::MidiBuffer{};
    bus.drain_into(out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].cc_value() == 127);
    REQUIRE(out[1].cc_value() == 0);

    view::Toggle toggle;
    view::bind_midi_cc(toggle, bus, 0, 65);
    toggle.on_toggle(true);
    out = midi::MidiBuffer{};
    bus.drain_into(out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].cc_number() == 65);

    view::Toggle note_toggle;
    view::bind_midi_note(note_toggle, bus, 0, 48, 90);
    note_toggle.on_toggle(true);
    note_toggle.on_toggle(false);
    out = midi::MidiBuffer{};
    bus.drain_into(out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].is_note_on());
    REQUIRE(out[0].note() == 48);
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

TEST_CASE("MidiParameterMap sanitizes non-finite scale endpoints", "[state][midi-map]") {
    state::StateStore store;
    populate(store);
    state::MidiParameterMap map;

    map.set_mapping(0, 10, 1, {std::numeric_limits<float>::quiet_NaN(), 0.75f});
    map.pump();

    map.handle_cc(store, 0, 10, 0);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.0f, 1e-6f));
    map.handle_cc(store, 0, 10, 127);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(0.75f, 1e-6f));

    map.arm_learn(2, {0.25f, std::numeric_limits<float>::infinity()});
    map.pump();
    map.handle_cc(store, 1, 11, 0);
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(0.25f, 1e-6f));
    map.handle_cc(store, 1, 11, 127);
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("MidiParameterMap maps any parameter with zero driver code", "[state][midi-map]") {
    // The map is parameter-agnostic: it drives whatever StateStore + ParamID it
    // is handed, so a hardware controller reaches any registered parameter
    // without a line of per-controller or per-parameter code — only runtime
    // learn/apply calls carrying opaque IDs. Prove that generically: register a
    // spread of parameters, then learn and drive each one through the same loop.
    state::StateStore store;
    struct Target {
        state::ParamID id;
        uint8_t channel;
        uint8_t cc;
    };
    const std::array<Target, 4> targets{{
        {10, 0, 1},    // controller 1 on channel 0
        {20, 3, 74},   // filter-cutoff-style CC on channel 3
        {30, 9, 7},    // volume-style CC on channel 9
        {40, 15, 120}, // a high CC on the top channel
    }};
    for (const auto& t : targets)
        store.add_parameter(
            {.id = t.id, .name = "P", .unit = "", .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    state::MidiParameterMap map;
    for (const auto& t : targets) {
        // Learn: the next CC on this (channel, cc) binds to this parameter.
        map.arm_learn(t.id);
        map.pump();
        map.handle_cc(store, t.channel, t.cc, 127);
        REQUIRE_THAT(store.get_normalized(t.id), WithinAbs(1.0f, 1e-3f));
        // The learned controller keeps driving it afterwards.
        map.handle_cc(store, t.channel, t.cc, 0);
        REQUIRE_THAT(store.get_normalized(t.id), WithinAbs(0.0f, 1e-3f));
    }

    // Every parameter has its own independent mapping: driving one does not
    // disturb the others, and a CC that was never learned is ignored.
    map.handle_cc(store, targets[1].channel, targets[1].cc, 127);
    REQUIRE_THAT(store.get_normalized(targets[1].id), WithinAbs(1.0f, 1e-3f));
    REQUIRE_THAT(store.get_normalized(targets[0].id), WithinAbs(0.0f, 1e-3f));
    map.handle_cc(store, 0, 55, 127); // unmapped controller — no effect anywhere
    REQUIRE_THAT(store.get_normalized(targets[0].id), WithinAbs(0.0f, 1e-3f));
    REQUIRE_THAT(store.get_normalized(targets[3].id), WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("MidiParameterMap re-learn replaces an existing mapping", "[state][midi-map]") {
    // Re-learning the same controller must retarget it, not stack a second
    // binding: the old parameter goes quiet and the new one takes over.
    state::StateStore store;
    populate(store); // params 1 (Cutoff) and 2 (Res)

    state::MidiParameterMap map;
    map.arm_learn(1);
    map.pump();
    map.handle_cc(store, 0, 20, 127); // CC 20 → param 1
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));

    // Re-learn CC 20 onto param 2.
    map.arm_learn(2);
    map.pump();
    map.handle_cc(store, 0, 20, 64); // CC 20 now → param 2 only
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(64.0f / 127.0f, 1e-3f));
    // Param 1 is no longer driven by CC 20 (still at its earlier value).
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f));
    map.handle_cc(store, 0, 20, 0);
    REQUIRE_THAT(store.get_normalized(1), WithinAbs(1.0f, 1e-3f)); // untouched
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(0.0f, 1e-3f)); // followed
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

    // Clear with multiple mappings: only the targeted one is removed, the
    // other is retained (exercises the remove-keep path).
    map.set_mapping(0, 40, 1);
    map.set_mapping(0, 41, 2);
    map.pump();
    map.clear(1);
    map.pump();
    map.handle_cc(store, 0, 40, 127); // cleared — no effect
    map.handle_cc(store, 0, 41, 127); // retained — drives param 2
    REQUIRE_THAT(store.get_normalized(2), WithinAbs(1.0f, 1e-3f));
}
