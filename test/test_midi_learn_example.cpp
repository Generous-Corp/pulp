// MidiLearnDemo end to end, headless: a right-click on a bound knob starts a
// learn, a controller binds it, the controller then drives the parameter,
// automation moves are sent back to the controller, undo reverses the learn,
// and the mappings survive saving and restoring the plug-in state.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "midi_learn_demo.hpp"
#include "midi_learn_demo_ui.hpp"

#include <pulp/midi/buffer.hpp>
#include <pulp/view/context_menu.hpp>

#include <cstdint>
#include <memory>

using namespace pulp;
using namespace pulp::examples;
using Catch::Matchers::WithinAbs;
using Phase = view::MidiLearnParamState::Phase;

namespace {

struct Rig {
    state::StateStore store;
    std::unique_ptr<format::Processor> owned =
        create_midi_learn_demo(format::MidiLearnFormat::vst3);
    MidiLearnDemoProcessor& processor = static_cast<MidiLearnDemoProcessor&>(*owned);
    std::uint64_t now = 1000;
    midi::MidiBuffer midi_out;

    Rig() {
        processor.set_state_store(&store);
        processor.define_parameters(store);
        processor.prepare({.sample_rate = 48000.0, .max_buffer_size = 64});
    }

    // One audio block carrying `events`; returns what the plug-in sent to MIDI out.
    void block(const std::vector<midi::MidiEvent>& events = {}) {
        std::vector<float> l(64, 0.5f), r(64, 0.5f), ol(64), orr(64);
        const float* in_ptrs[2] = {l.data(), r.data()};
        float* out_ptrs[2] = {ol.data(), orr.data()};
        audio::BufferView<const float> in(in_ptrs, 2, 64);
        audio::BufferView<float> out(out_ptrs, 2, 64);
        midi::MidiBuffer midi_in;
        for (const auto& e : events)
            midi_in.add(e);
        midi_out.clear();
        processor.process(out, in, midi_in, midi_out, format::ProcessContext{});
    }
};

view::KeyEvent key(view::KeyCode k) {
    view::KeyEvent e;
    e.key = k;
    e.is_down = true;
    return e;
}

// Right-click the knob at `root_pos` and choose the first row.
void choose_first_row(view::View& root, view::Point root_pos) {
    const std::size_t before = root.child_count();
    REQUIRE(root.simulate_context_click(root_pos));
    REQUIRE(root.child_count() == before + 1);
    auto* menu = dynamic_cast<view::ContextMenu*>(root.child_at(root.child_count() - 1));
    REQUIRE(menu != nullptr);
    REQUIRE(menu->on_key_event(key(view::KeyCode::down)));
    REQUIRE(menu->on_key_event(key(view::KeyCode::enter)));
}

} // namespace

TEST_CASE("midi learn demo: learn from the menu, then the controller drives the knob",
          "[example][midi-learn]") {
    Rig rig;
    MidiLearnDemoUi ui(rig.processor, [&] { return rig.now; });
    CHECK(ui.level_overlay().current().phase == Phase::idle);

    // Right-click the Level knob (80x80 at 20,30) and pick "MIDI Learn".
    choose_first_row(ui.root(), {60, 70});
    ui.tick();
    CHECK(ui.level_overlay().current().phase == Phase::listening);
    CHECK(ui.pan_overlay().current().phase == Phase::idle);

    // The user turns a hardware knob: CC 74 on channel 3.
    rig.block({midi::MidiEvent::cc(2, 74, 20), midi::MidiEvent::cc(2, 74, 60)});
    rig.now += 10;
    ui.tick();
    CHECK(ui.level_overlay().current().phase == Phase::listening); // still settling
    rig.now += 400;
    ui.tick();
    CHECK(ui.level_overlay().current().phase == Phase::mapped);
    CHECK(ui.level_overlay().current().mapping_label == "CC 74 ch 3");
    CHECK(ui.level_overlay().current().just_bound);
    CHECK(ui.pan_overlay().current().phase == Phase::idle);

    // Now the controller moves the parameter.
    rig.block({midi::MidiEvent::cc(2, 74, 127)});
    CHECK_THAT(rig.store.get_normalized(kDemoLevel), WithinAbs(1.0f, 1e-5f));
    rig.block({midi::MidiEvent::cc(2, 74, 0)});
    CHECK_THAT(rig.store.get_normalized(kDemoLevel), WithinAbs(0.0f, 1e-5f));
}

TEST_CASE("midi learn demo: feedback follows the parameter and is not echoed",
          "[example][midi-learn]") {
    Rig rig;
    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 0, 74};
    m.id = kDemoPan;
    m.feedback = true;
    REQUIRE(rig.processor.learn().set_mapping(m));

    rig.block(); // adopts the mapping and syncs the controller to the current Pan (centre)
    REQUIRE(rig.midi_out.size() == 1);
    CHECK(rig.midi_out.begin()->data()[0] == 0xB0);
    CHECK(rig.midi_out.begin()->data()[1] == 74);
    CHECK(rig.midi_out.begin()->data()[2] == 64);

    // Host automation moves Pan: the controller is told.
    rig.store.set_normalized_rt(kDemoPan, 1.0f);
    rig.block();
    REQUIRE(rig.midi_out.size() == 1);
    CHECK(rig.midi_out.begin()->data()[2] == 127);

    // The controller moves Pan itself: nothing is sent back.
    rig.block({midi::MidiEvent::cc(0, 74, 30)});
    CHECK(rig.midi_out.size() == 0);
}

TEST_CASE("midi learn demo: undo reverses a learn", "[example][midi-learn]") {
    Rig rig;
    MidiLearnDemoUi ui(rig.processor, [&] { return rig.now; });
    choose_first_row(ui.root(), {180, 70}); // Pan knob
    rig.block({midi::MidiEvent::cc(0, 10, 10), midi::MidiEvent::cc(0, 10, 90)});
    rig.now += 10;
    ui.tick();
    rig.now += 400;
    ui.tick();
    REQUIRE(ui.pan_overlay().current().phase == Phase::mapped);
    REQUIRE(ui.undo().can_undo());

    ui.undo().undo();
    ui.tick();
    CHECK(ui.pan_overlay().current().phase == Phase::idle);
    rig.block({midi::MidiEvent::cc(0, 10, 127)});
    CHECK_THAT(rig.store.get_normalized(kDemoPan), WithinAbs(0.5f, 1e-5f)); // unmapped again

    ui.undo().redo();
    ui.tick();
    CHECK(ui.pan_overlay().current().phase == Phase::mapped);
}

TEST_CASE("midi learn demo: Escape cancels a listening learn", "[example][midi-learn]") {
    Rig rig;
    MidiLearnDemoUi ui(rig.processor, [&] { return rig.now; });
    choose_first_row(ui.root(), {60, 70});
    ui.tick();
    REQUIRE(ui.level_overlay().current().phase == Phase::listening);
    CHECK(ui.session().handle_key(key(view::KeyCode::escape)));
    ui.tick();
    CHECK(ui.level_overlay().current().phase == Phase::idle);
}

TEST_CASE("midi learn demo: mappings are saved with the plug-in state", "[example][midi-learn]") {
    Rig a;
    state::MidiMapping m;
    m.source = {state::MidiSourceKind::cc7, 1, 21};
    m.id = kDemoLevel;
    m.scale = {0.25f, 0.75f};
    REQUIRE(a.processor.learn().set_mapping(m));
    const auto bytes = a.processor.serialize_plugin_state();
    REQUIRE_FALSE(bytes.empty());

    Rig b;
    REQUIRE(b.processor.deserialize_plugin_state(bytes));
    const auto restored = b.processor.learn().mapping_for(kDemoLevel);
    REQUIRE(restored.has_value());
    CHECK(restored->source == m.source);
    b.block({midi::MidiEvent::cc(1, 21, 127)});
    CHECK_THAT(b.store.get_normalized(kDemoLevel), WithinAbs(0.75f, 1e-5f));

    // A state saved before mappings existed restores as no mappings.
    Rig c;
    REQUIRE(c.processor.deserialize_plugin_state({}));
    CHECK(c.processor.learn().mappings().empty());
}

TEST_CASE("midi learn demo: the plug-in reports feedback support for its format",
          "[example][midi-learn]") {
    const auto vst3 = create_midi_learn_demo(format::MidiLearnFormat::vst3);
    CHECK(format::midi_feedback_capabilities(format::MidiLearnFormat::vst3, vst3->descriptor())
              .supported);
    CHECK_FALSE(format::midi_feedback_capabilities(format::MidiLearnFormat::lv2, vst3->descriptor())
                    .supported);
}
