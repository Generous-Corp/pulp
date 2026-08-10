#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/midi_voice_modulation_adapter.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <limits>

TEST_CASE("MIDI note events populate the existing voice modulation contract",
          "[audio][midi][voice-mod]") {
    pulp::audio::MidiVoiceModulationAdapter<2> adapter;
    pulp::audio::VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 4, .max_frames = 64}));
    REQUIRE(adapter.note_event(1, pulp::midi::MidiEvent::note_on(3, 60, 100), 42));

    pulp::midi::MpeNoteState expression;
    expression.active = true;
    expression.channel = 3;
    expression.note = 60;
    expression.note_id = 42;
    expression.pitch_bend_semitones = 1.5f;
    expression.pressure = 0.25f;
    expression.timbre = 0.75f;
    REQUIRE(adapter.mpe_expression(1, expression));

    auto stale_expression = expression;
    stale_expression.note_id = 43;
    CHECK_FALSE(adapter.mpe_expression(1, stale_expression));
    stale_expression = expression;
    stale_expression.active = false;
    CHECK_FALSE(adapter.mpe_expression(1, stale_expression));
    stale_expression = expression;
    stale_expression.pitch_bend_semitones = std::numeric_limits<float>::max();
    CHECK_FALSE(adapter.mpe_expression(1, stale_expression));

    auto invalid_expression = expression;
    invalid_expression.pressure = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(adapter.mpe_expression(1, invalid_expression));

    pulp::test::RtAllocationProbe allocations;
    REQUIRE(adapter.write_voice(1, buffer, 64).ok);
    CHECK_FALSE(allocations.saw_allocation());
    const auto block = buffer.block();
    CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Gain, 0) ==
          Catch::Approx(100.0f / 127.0f));
    CHECK(block.value_at(pulp::audio::VoiceModulationTarget::PitchCents, 0) ==
          Catch::Approx(150.0f));
    CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Pressure, 0) == Catch::Approx(0.25f));
    CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Timbre, 0) == Catch::Approx(0.75f));

    pulp::audio::VoiceModulationBuffer undersized;
    REQUIRE(undersized.prepare({.max_lanes = 3, .max_frames = 64}));
    const auto lane_failure = adapter.write_voice(1, undersized, 64);
    CHECK_FALSE(lane_failure.ok);
    CHECK(lane_failure.status == pulp::audio::VoiceModulationStatus::LaneOverflow);
    CHECK(undersized.block().empty());

    pulp::audio::VoiceModulationBuffer short_buffer;
    REQUIRE(short_buffer.prepare({.max_lanes = 4, .max_frames = 8}));
    const auto frame_failure = adapter.write_voice(1, short_buffer, 9);
    CHECK_FALSE(frame_failure.ok);
    CHECK(frame_failure.status == pulp::audio::VoiceModulationStatus::InvalidFrameCount);
    CHECK(short_buffer.block().empty());
}

TEST_CASE("MIDI voice modulation keeps simultaneous voices isolated",
          "[audio][midi][voice-mod][polyphony][ownership]") {
    pulp::audio::MidiVoiceModulationAdapter<2> adapter;
    constexpr pulp::midi::MpeNoteGeneration lower_generation = 10;
    constexpr pulp::midi::MpeNoteGeneration upper_generation = 20;

    REQUIRE(adapter.note_event(
        0, pulp::midi::MidiEvent::note_on(2, 60, 64), lower_generation));
    REQUIRE(adapter.note_event(
        1, pulp::midi::MidiEvent::note_on(3, 67, 112), upper_generation));

    pulp::midi::MpeNoteState lower_expression;
    lower_expression.active = true;
    lower_expression.channel = 2;
    lower_expression.note = 60;
    lower_expression.note_id = lower_generation;
    lower_expression.pitch_bend_semitones = 1.25f;
    lower_expression.pressure = 0.2f;
    lower_expression.timbre = 0.3f;
    REQUIRE(adapter.mpe_expression(0, lower_expression));

    pulp::midi::MpeNoteState upper_expression;
    upper_expression.active = true;
    upper_expression.channel = 3;
    upper_expression.note = 67;
    upper_expression.note_id = upper_generation;
    upper_expression.pitch_bend_semitones = -0.5f;
    upper_expression.pressure = 0.7f;
    upper_expression.timbre = 0.8f;
    REQUIRE(adapter.mpe_expression(1, upper_expression));

    const auto* lower_state = adapter.state(0);
    const auto* upper_state = adapter.state(1);
    REQUIRE(lower_state != nullptr);
    REQUIRE(upper_state != nullptr);
    REQUIRE(lower_state->active);
    REQUIRE(upper_state->active);
    CHECK(lower_state->channel == 2);
    CHECK(lower_state->note == 60);
    CHECK(lower_state->note_id == lower_generation);
    CHECK(upper_state->channel == 3);
    CHECK(upper_state->note == 67);
    CHECK(upper_state->note_id == upper_generation);

    pulp::audio::VoiceModulationBuffer lower_buffer;
    pulp::audio::VoiceModulationBuffer upper_buffer;
    REQUIRE(lower_buffer.prepare({.max_lanes = 4, .max_frames = 32}));
    REQUIRE(upper_buffer.prepare({.max_lanes = 4, .max_frames = 32}));
    REQUIRE(adapter.write_voice(0, lower_buffer, 32).ok);
    REQUIRE(adapter.write_voice(1, upper_buffer, 32).ok);

    const auto check_voice = [](const pulp::audio::VoiceModulationBlock& block,
                                float gain, float pitch, float pressure, float timbre) {
        REQUIRE(block.frame_count == 32);
        for (const auto frame : std::array{0u, 31u}) {
            CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Gain, frame) ==
                  Catch::Approx(gain));
            CHECK(block.value_at(pulp::audio::VoiceModulationTarget::PitchCents, frame) ==
                  Catch::Approx(pitch));
            CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Pressure, frame) ==
                  Catch::Approx(pressure));
            CHECK(block.value_at(pulp::audio::VoiceModulationTarget::Timbre, frame) ==
                  Catch::Approx(timbre));
        }
    };
    check_voice(lower_buffer.block(), 64.0f / 127.0f, 125.0f, 0.2f, 0.3f);
    check_voice(upper_buffer.block(), 112.0f / 127.0f, -50.0f, 0.7f, 0.8f);

    CHECK_FALSE(adapter.release_voice(1, lower_generation));
    CHECK_FALSE(adapter.note_event(
        0, pulp::midi::MidiEvent::note_off(2, 60), upper_generation));
    REQUIRE(adapter.state(0)->active);
    REQUIRE(adapter.state(1)->active);

    REQUIRE(adapter.release_voice(0, lower_generation));
    CHECK_FALSE(adapter.state(0)->active);
    REQUIRE(adapter.state(1)->active);
    CHECK(adapter.state(1)->note_id == upper_generation);
    REQUIRE(adapter.write_voice(1, upper_buffer, 32).ok);
    check_voice(upper_buffer.block(), 112.0f / 127.0f, -50.0f, 0.7f, 0.8f);

    REQUIRE(adapter.release_voice(1, upper_generation));
    CHECK_FALSE(adapter.state(1)->active);
}

TEST_CASE("Voice modulation adapter reset and hot swap clear voice ownership",
          "[audio][midi][voice-mod][lifecycle]") {
    pulp::audio::MidiVoiceModulationAdapter<1> adapter;
    REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(0, 64, 127), 1));
    REQUIRE(adapter.state(0)->active);
    adapter.hot_swap_reset();
    CHECK_FALSE(adapter.state(0)->active);
    REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(0, 65, 127), 2));
    adapter.reset();
    CHECK_FALSE(adapter.state(0)->active);

    SECTION("release identity prevents stale note generations from clearing a voice") {
        REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 70, 100), 100));
        REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 70, 110), 101));
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 69, 120), 100));
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 69, 120), 101));
        REQUIRE(adapter.state(0)->note == 70);
        REQUIRE(adapter.state(0)->note_id == 101);
        REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 70, 0), 101));
        CHECK_FALSE(adapter.state(0)->active);
        CHECK(adapter.state(0)->note_id == 101);
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 69, 120), 100));
        REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 70, 110), 102));
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_off(2, 70), 99));
        REQUIRE(adapter.state(0)->active);
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_off(2, 70), 100));
        REQUIRE(adapter.state(0)->active);
        CHECK_FALSE(adapter.release_voice(0, 100));
        REQUIRE(adapter.state(0)->active);
        REQUIRE(adapter.release_voice(0, 102));
        CHECK_FALSE(adapter.state(0)->active);
        CHECK(adapter.state(0)->note_id == 102);
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 71, 120), 101));
        REQUIRE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 71, 120), 103));
    }

    SECTION("zero is not a usable ownership generation") {
        CHECK_FALSE(adapter.note_event(0, pulp::midi::MidiEvent::note_on(2, 70, 100), 0));
        CHECK_FALSE(adapter.release_voice(0, 0));
    }
}

TEST_CASE("Voice modulation adapter preserves wide MPE generations",
          "[audio][midi][voice-mod][generation]") {
    pulp::audio::MidiVoiceModulationAdapter<1> adapter;
    constexpr auto generation = std::numeric_limits<pulp::midi::MpeNoteGeneration>::max();
    const auto on = pulp::midi::MidiEvent::note_on(3, 67, 101);

    REQUIRE(adapter.note_event(0, on, generation));
    REQUIRE(adapter.state(0)->note_id == generation);

    pulp::midi::MpeNoteState expression;
    expression.active = true;
    expression.channel = 3;
    expression.note = 67;
    expression.note_id = generation;
    expression.pressure = 0.75f;
    REQUIRE(adapter.mpe_expression(0, expression));
    REQUIRE(adapter.state(0)->pressure == Catch::Approx(0.75f));

    REQUIRE_FALSE(adapter.release_voice(0, generation - 1));
    REQUIRE(adapter.state(0)->active);
    REQUIRE(adapter.release_voice(0, generation));
    REQUIRE_FALSE(adapter.state(0)->active);
}
