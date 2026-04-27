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

TEST_CASE("MpeVoiceTracker reset clears per-channel expression caches", "[midi][mpe]") {
    // Regression for Codex P2: notes added after reset() must not inherit
    // stale bend/pressure/timbre values from a previous session.
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::pitch_bend(1, 16383));  // member pitch bend
    tracker.process(channel_pressure(1, 127));
    tracker.process(MidiEvent::cc(1, 74, 127));
    tracker.reset();

    tracker.process(MidiEvent::note_on(1, 60, 100));
    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->pitch_bend_semitones == Approx(0.0f).margin(1e-6f));
    REQUIRE(n->pressure == Approx(0.0f).margin(1e-6f));
    REQUIRE(n->timbre == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker bend range setters reject invalid values",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};

    tracker.set_member_bend_range(12.0f);
    tracker.set_manager_bend_range(3.0f);
    tracker.set_member_bend_range(0.0f);
    tracker.set_manager_bend_range(-4.0f);

    REQUIRE(tracker.member_bend_range() == Approx(12.0f));
    REQUIRE(tracker.manager_bend_range() == Approx(3.0f));
}

TEST_CASE("MpeVoiceTracker set_config resets notes and adopts new zones",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    REQUIRE(tracker.active_count() == 1);

    tracker.set_config(MpeConfig::dual(/*lower=*/2, /*upper=*/3));

    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.config().lower_zone.member_channels == 2);
    REQUIRE(tracker.config().upper_zone.member_channels == 3);
    REQUIRE_FALSE(tracker.process(MidiEvent::note_on(4, 60, 100)));
    REQUIRE(tracker.process(MidiEvent::note_on(13, 72, 100)));
    REQUIRE(tracker.find(13, 72)->is_upper_zone);
}

TEST_CASE("MpeVoiceTracker manager pressure and timbre update both zone states",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::dual(/*lower=*/3, /*upper=*/3)};

    REQUIRE(tracker.process(channel_pressure(0, 64)));
    REQUIRE(tracker.process(MidiEvent::cc(0, 74, 32)));
    REQUIRE(tracker.process(channel_pressure(15, 127)));
    REQUIRE(tracker.process(MidiEvent::cc(15, 74, 100)));

    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.lower_zone_state().pressure == Approx(64.0f / 127.0f).margin(1e-6f));
    REQUIRE(tracker.lower_zone_state().timbre == Approx(32.0f / 127.0f).margin(1e-6f));
    REQUIRE(tracker.upper_zone_state().pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(tracker.upper_zone_state().timbre == Approx(100.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker member expression cache seeds later notes",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(24.0f);

    REQUIRE(tracker.process(MidiEvent::pitch_bend(6, 0)));
    REQUIRE(tracker.process(channel_pressure(6, 96)));
    REQUIRE(tracker.process(MidiEvent::cc(6, 74, 80)));
    REQUIRE(tracker.process(MidiEvent::cc(6, 1, 127)));  // belongs to zone, not MPE timbre
    REQUIRE(tracker.process(MidiEvent::note_on(6, 70, 90)));

    const auto* n = tracker.find(6, 70);
    REQUIRE(n != nullptr);
    REQUIRE(n->pitch_bend_semitones == Approx(-24.0f).margin(0.01f));
    REQUIRE(n->pressure == Approx(96.0f / 127.0f).margin(1e-6f));
    REQUIRE(n->timbre == Approx(80.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker snapshot reports full count with bounded output",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::note_on(2, 64, 101));
    tracker.process(MidiEvent::note_on(3, 67, 102));

    MpeNoteState out[2]{};
    REQUIRE(tracker.snapshot(out, 2) == 3);
    REQUIRE(out[0].active);
    REQUIRE(out[0].note == 60);
    REQUIRE(out[1].active);
    REQUIRE(out[1].note == 64);

    std::size_t active_slots = 0;
    for (const auto& slot : tracker.slots()) {
        if (slot.active) ++active_slots;
    }
    REQUIRE(active_slots == 3);
}

TEST_CASE("MpeVoiceTracker full table drops extra notes without callback",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    for (int note = 0; note < static_cast<int>(MpeVoiceTracker::kMaxNotes); ++note) {
        REQUIRE(tracker.process(MidiEvent::note_on(1, static_cast<uint8_t>(note), 64)));
    }
    REQUIRE(tracker.active_count() == MpeVoiceTracker::kMaxNotes);

    int callbacks = 0;
    tracker.on_note_on = [&](const MpeNoteState&) { ++callbacks; };
    REQUIRE(tracker.process(MidiEvent::note_on(2, 0, 100)));

    REQUIRE(tracker.active_count() == MpeVoiceTracker::kMaxNotes);
    REQUIRE(tracker.find(2, 0) == nullptr);
    REQUIRE(callbacks == 0);
}

TEST_CASE("MpeVoiceTracker dispatches MIDI 1.0 UMP packets through MPE zones", "[midi][mpe][ump]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    REQUIRE(tracker.process(UmpPacket::midi1_note_on(0, 1, 60, 100)));

    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->velocity == 100);
    REQUIRE(tracker.active_count() == 1);
}

TEST_CASE("MpeVoiceTracker applies MIDI 2.0 per-note expression to the matching note", "[midi][mpe][ump]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(48.0f);

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 1, 60, 0x8000)));
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 2, 64, 0x8000)));

    REQUIRE(tracker.process(UmpPacket::per_note_pitch_bend(0, 1, 60, 0xC0000000)));
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(0, 1, 60, 74, 0xFFFFFFFF)));

    const auto* bent = tracker.find(1, 60);
    const auto* untouched = tracker.find(2, 64);
    REQUIRE(bent != nullptr);
    REQUIRE(untouched != nullptr);
    REQUIRE(bent->pitch_bend_semitones == Approx(24.0f).margin(0.01f));
    REQUIRE(bent->timbre == Approx(1.0f).margin(1e-6f));
    REQUIRE(untouched->pitch_bend_semitones == Approx(0.0f).margin(1e-6f));
    REQUIRE(untouched->timbre == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker ignores unmatched MIDI 2.0 per-note expression without seeding channel state",
          "[midi][mpe][ump][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(24.0f);

    int bend_callbacks = 0;
    int timbre_callbacks = 0;
    tracker.on_pitch_bend = [&](const MpeNoteState&) { ++bend_callbacks; };
    tracker.on_timbre = [&](const MpeNoteState&) { ++timbre_callbacks; };

    REQUIRE(tracker.process(UmpPacket::per_note_pitch_bend(0, 4, 60, 0xFFFFFFFF)));
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(0, 4, 60, 74, 0xFFFFFFFF)));
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(bend_callbacks == 0);
    REQUIRE(timbre_callbacks == 0);

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 4, 60, 0xFFFF)));
    const auto* n = tracker.find(4, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->pitch_bend_semitones == Approx(0.0f).margin(1e-6f));
    REQUIRE(n->timbre == Approx(0.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker applies MIDI 2.0 member expression to all active channel notes",
          "[midi][mpe][ump][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(12.0f);

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 7, 60, 0xFFFF)));
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 7, 64, 0xFFFF)));
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(0, 7, 0x00000000)));

    UmpPacket pressure;
    pressure.word_count = 2;
    pressure.words[0] = (0x4u << 28) | (uint32_t(0xD0 | 7) << 16);
    pressure.words[1] = 0xFFFFFFFFu;
    REQUIRE(tracker.process(pressure));
    REQUIRE(tracker.process(UmpPacket::cc_2(0, 7, 74, 0x80000000)));

    const auto* low = tracker.find(7, 60);
    const auto* high = tracker.find(7, 64);
    REQUIRE(low != nullptr);
    REQUIRE(high != nullptr);
    REQUIRE(low->pitch_bend_semitones == Approx(-12.0f).margin(0.01f));
    REQUIRE(high->pitch_bend_semitones == Approx(-12.0f).margin(0.01f));
    REQUIRE(low->pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(high->pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(low->timbre == Approx(0.5f).margin(0.001f));
    REQUIRE(high->timbre == Approx(0.5f).margin(0.001f));
}

TEST_CASE("MpeVoiceTracker handles MIDI 2.0 manager state and note release variants", "[midi][mpe][ump]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_manager_bend_range(2.0f);

    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(0, 0, 0x00000000)));
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(-2.0f).margin(0.01f));

    REQUIRE(tracker.process(UmpPacket::cc_2(0, 0, 74, 0x80000000)));
    REQUIRE(tracker.lower_zone_state().timbre == Approx(0.5f).margin(0.001f));

    UmpPacket pressure;
    pressure.word_count = 2;
    pressure.words[0] = (0x4u << 28) | (uint32_t(0xD0) << 16);
    pressure.words[1] = 0x80000000u;
    REQUIRE(tracker.process(pressure));
    REQUIRE(tracker.lower_zone_state().pressure == Approx(0.5f).margin(0.001f));

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 3, 67, 0xFFFF)));
    REQUIRE(tracker.active_count() == 1);
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 3, 67, 0)));
    REQUIRE(tracker.active_count() == 0);

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 3, 67, 0xFFFF)));
    REQUIRE(tracker.process(UmpPacket::note_off_2(0, 3, 67)));
    REQUIRE(tracker.active_count() == 0);
}

TEST_CASE("MpeVoiceTracker consumes supported-zone MIDI 2.0 packets without mapped side effects",
          "[midi][mpe][ump][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 1, 60, 0xFFFF)));
    REQUIRE(tracker.process(UmpPacket::cc_2(0, 1, 1, 0xFFFFFFFF)));
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(0, 1, 60, 1, 0xFFFFFFFF)));

    UmpPacket program_change;
    program_change.word_count = 2;
    program_change.words[0] = (0x4u << 28) | (uint32_t(0xC0 | 1) << 16);
    program_change.words[1] = 0x12345678u;
    REQUIRE(tracker.process(program_change));

    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->timbre == Approx(0.0f).margin(1e-6f));
    REQUIRE(tracker.active_count() == 1);
}

TEST_CASE("MpeVoiceTracker ignores non-channel and out-of-zone UMP packets", "[midi][mpe][ump]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(4)};

    UmpPacket utility;
    utility.word_count = 1;
    utility.words[0] = 0;

    REQUIRE_FALSE(tracker.process(utility));
    REQUIRE_FALSE(tracker.process(UmpPacket::note_on_2(0, 5, 60, 0xFFFF)));
    REQUIRE(tracker.active_count() == 0);
}
