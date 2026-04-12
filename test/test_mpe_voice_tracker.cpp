#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/midi/mpe_voice_tracker.hpp>

using namespace pulp::midi;
using Catch::Approx;

namespace {

MidiEvent channel_pressure(uint8_t channel, uint8_t value) {
    return {choc::midi::ShortMessage(
        static_cast<uint8_t>(0xD0 | (channel & 0x0F)), value, 0), 0, 0.0};
}

} // namespace

TEST_CASE("MpeVoiceTracker allocates on member channel note-on", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 100)));
    REQUIRE(tracker.active_count() == 1);

    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->channel == 1);
    REQUIRE(n->note == 60);
    REQUIRE(n->velocity == 100);
    REQUIRE(n->note_id != 0);
    REQUIRE_FALSE(n->is_upper_zone);
}

TEST_CASE("MpeVoiceTracker ignores events outside any zone", "[midi][mpe]") {
    // Only a lower zone is configured — upper manager channel 15 is outside.
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    // Lower zone: manager=0, member channels 1..4. Channel 5 is outside.
    REQUIRE_FALSE(tracker.process(MidiEvent::note_on(5, 60, 100)));
    REQUIRE(tracker.active_count() == 0);
}

TEST_CASE("MpeVoiceTracker note-off releases oldest matching note", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::note_on(1, 60, 110));  // retrigger same slot
    REQUIRE(tracker.active_count() == 1);

    tracker.process(MidiEvent::note_off(1, 60));
    REQUIRE(tracker.active_count() == 0);
}

TEST_CASE("MpeVoiceTracker note-on velocity 0 counts as note-off", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(2, 64, 90));
    REQUIRE(tracker.active_count() == 1);
    tracker.process(MidiEvent::note_on(2, 64, 0));
    REQUIRE(tracker.active_count() == 0);
}

TEST_CASE("MpeVoiceTracker pitch bend applies per-note via channel", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(48.0f);

    tracker.process(MidiEvent::note_on(3, 72, 100));
    // Max pitch bend up (16383) → +48 semitones.
    tracker.process(MidiEvent::pitch_bend(3, 16383));

    const auto* n = tracker.find(3, 72);
    REQUIRE(n != nullptr);
    REQUIRE(n->pitch_bend_semitones == Approx(48.0f).margin(0.01f));
}

TEST_CASE("MpeVoiceTracker channel pressure updates active note", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(4, 60, 80));
    tracker.process(channel_pressure(4, 127));

    const auto* n = tracker.find(4, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->pressure == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker CC74 maps to timbre 0..1", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(5, 60, 80));
    tracker.process(MidiEvent::cc(5, 74, 64));

    const auto* n = tracker.find(5, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->timbre == Approx(64.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker manager pitch bend is zone-wide", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_manager_bend_range(2.0f);

    // Pitch bend on manager channel 0 — does not create a note; updates
    // zone-wide state at ±2 semitones.
    REQUIRE(tracker.process(MidiEvent::pitch_bend(0, 0)));
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(-2.0f).margin(0.01f));
}

TEST_CASE("MpeVoiceTracker upper zone note-on flagged correctly", "[midi][mpe]") {
    auto cfg = MpeConfig::dual(/*lower=*/4, /*upper=*/4);
    MpeVoiceTracker tracker{cfg};

    // Upper manager = 15, upper members = 11..14.
    REQUIRE(tracker.process(MidiEvent::note_on(12, 72, 110)));
    const auto* n = tracker.find(12, 72);
    REQUIRE(n != nullptr);
    REQUIRE(n->is_upper_zone);
}

TEST_CASE("MpeVoiceTracker callbacks fire on lifecycle events", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    int on_count = 0, off_count = 0, bend_count = 0, pressure_count = 0, timbre_count = 0;
    tracker.on_note_on     = [&](const MpeNoteState&) { ++on_count; };
    tracker.on_note_off    = [&](const MpeNoteState&) { ++off_count; };
    tracker.on_pitch_bend  = [&](const MpeNoteState&) { ++bend_count; };
    tracker.on_pressure    = [&](const MpeNoteState&) { ++pressure_count; };
    tracker.on_timbre      = [&](const MpeNoteState&) { ++timbre_count; };

    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::pitch_bend(1, 8192));
    tracker.process(channel_pressure(1, 64));
    tracker.process(MidiEvent::cc(1, 74, 100));
    tracker.process(MidiEvent::note_off(1, 60));

    REQUIRE(on_count == 1);
    REQUIRE(off_count == 1);
    REQUIRE(bend_count == 1);
    REQUIRE(pressure_count == 1);
    REQUIRE(timbre_count == 1);
}

TEST_CASE("MpeVoiceTracker multiple channels track independently", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::note_on(2, 64, 100));
    tracker.process(MidiEvent::note_on(3, 67, 100));
    REQUIRE(tracker.active_count() == 3);

    tracker.process(MidiEvent::pitch_bend(2, 16383));  // only channel 2 bends
    REQUIRE(tracker.find(1, 60)->pitch_bend_semitones == Approx(0.0f).margin(0.01f));
    REQUIRE(tracker.find(2, 64)->pitch_bend_semitones > 1.0f);
    REQUIRE(tracker.find(3, 67)->pitch_bend_semitones == Approx(0.0f).margin(0.01f));
}

TEST_CASE("MpeVoiceTracker reset clears all state", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::pitch_bend(0, 16383));  // manager — zone state
    REQUIRE(tracker.active_count() == 1);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones != 0.0f);

    tracker.reset();
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == 0.0f);
}
