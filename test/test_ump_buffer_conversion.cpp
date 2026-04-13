#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <pulp/midi/mpe_voice_tracker.hpp>

using namespace pulp::midi;
using Catch::Approx;

TEST_CASE("UmpBuffer sort + clear", "[midi][ump]") {
    UmpBuffer buf;
    buf.add(UmpPacket::note_on_2(0, 0, 60, 0x8000), 30);
    buf.add(UmpPacket::note_on_2(0, 0, 62, 0x8000), 10);
    buf.add(UmpPacket::note_on_2(0, 0, 64, 0x8000), 20);
    REQUIRE(buf.size() == 3);
    buf.sort();
    REQUIRE(buf[0].sample_offset == 10);
    REQUIRE(buf[2].sample_offset == 30);
    buf.clear();
    REQUIRE(buf.empty());
}

TEST_CASE("Processor::supports_ump defaults to false", "[midi][ump]") {
    pulp::format::PluginDescriptor desc;
    REQUIRE_FALSE(desc.supports_ump);
}

TEST_CASE("scale_7_to_16 preserves 0 and expands 127", "[midi][ump]") {
    REQUIRE(scale_7_to_16(0) == 0);
    REQUIRE(scale_7_to_16(127) == 0xFFFF);   // spec says 127 → 0xFFFF
    REQUIRE(scale_7_to_16(64) > 0x7FFF);
}

TEST_CASE("scale_16_to_7 inverts scale_7_to_16 for common values", "[midi][ump]") {
    for (uint8_t v : {uint8_t(0), uint8_t(1), uint8_t(63), uint8_t(100), uint8_t(127)}) {
        REQUIRE(scale_16_to_7(scale_7_to_16(v)) == v);
    }
}

TEST_CASE("scale_14_to_32 / scale_32_to_14 preserve centre", "[midi][ump]") {
    REQUIRE(scale_14_to_32(0x2000) == 0x80000000u);
    REQUIRE(scale_32_to_14(0x80000000u) == 0x2000);
}

TEST_CASE("midi1_to_ump round-trip via ump_to_midi1", "[midi][ump]") {
    MidiBuffer in;
    in.add(MidiEvent::note_on(1, 60, 100));
    in.add(MidiEvent::note_off(1, 60, 0));
    in.add(MidiEvent::cc(2, 74, 64));
    in.add(MidiEvent::pitch_bend(3, 16383));
    in.add(MidiEvent::note_on(4, 72, 1));

    UmpBuffer ump;
    midi1_to_ump(in, ump);
    REQUIRE(ump.size() == in.size());
    for (const auto& ue : ump) {
        REQUIRE(ue.packet.message_type() == UmpMessageType::Midi2ChannelVoice);
    }

    MidiBuffer out;
    ump_to_midi1(ump, out);
    REQUIRE(out.size() == in.size());

    // Note-on with velocity 100 round-trips exactly.
    REQUIRE(out[0].is_note_on());
    REQUIRE(out[0].note() == 60);
    REQUIRE(out[0].velocity() == 100);

    // Note-off round-trips.
    REQUIRE(out[1].is_note_off());
    REQUIRE(out[1].note() == 60);

    // CC 74.
    REQUIRE(out[2].is_cc());
    REQUIRE(out[2].cc_number() == 74);
    REQUIRE(out[2].cc_value() == 64);

    // Pitch bend maxes out.
    REQUIRE(out[3].is_pitch_bend());

    // Tiny velocity clamped to 1 (not zero, which would be a note-off).
    REQUIRE(out[4].is_note_on());
    REQUIRE(out[4].velocity() >= 1);
}

TEST_CASE("MpeVoiceTracker ingests UMP MIDI 2.0 note-on", "[midi][ump][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    // Member channel 1, note 60, half velocity (0x8000 → ~64 for 7-bit view).
    auto p = UmpPacket::note_on_2(0, 1, 60, 0x8000);
    REQUIRE(tracker.process(p));
    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->channel == 1);
    REQUIRE(n->note == 60);
}

TEST_CASE("MpeVoiceTracker applies UMP per-note pitch bend to one note", "[midi][ump][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(48.0f);
    tracker.process(UmpPacket::note_on_2(0, 1, 60, 0x8000));
    tracker.process(UmpPacket::note_on_2(0, 1, 67, 0x8000));  // two notes same channel

    // Per-note pitch bend on note 67 only — note 60 should be unaffected.
    tracker.process(UmpPacket::per_note_pitch_bend(0, 1, 67, 0xFFFFFFFFu));
    const auto* n60 = tracker.find(1, 60);
    const auto* n67 = tracker.find(1, 67);
    REQUIRE(n60 != nullptr);
    REQUIRE(n67 != nullptr);
    REQUIRE(n60->pitch_bend_semitones == Approx(0.0f).margin(0.01f));
    REQUIRE(n67->pitch_bend_semitones > 40.0f);
}

TEST_CASE("MpeVoiceTracker applies UMP channel pitch bend to all notes on channel", "[midi][ump][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.set_member_bend_range(48.0f);
    tracker.process(UmpPacket::note_on_2(0, 2, 72, 0x8000));

    // Max positive channel pitch bend (all-ones) → +48 semitones.
    tracker.process(UmpPacket::pitch_bend_2(0, 2, 0xFFFFFFFFu));
    const auto* n = tracker.find(2, 72);
    REQUIRE(n != nullptr);
    REQUIRE(n->pitch_bend_semitones > 40.0f);
}

TEST_CASE("MpeVoiceTracker routes UMP per-note CC 74 by controller index", "[midi][ump][mpe]") {
    // Regression for PR #141 Codex P1: the per-note controller index lives
    // in byte 3 (bits 0-7) of word 0, NOT byte 2 (which is the note number).
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(UmpPacket::note_on_2(0, 1, 60, 0x8000));

    // Per-note CC 74 (timbre) on note 60, full value. Before the fix,
    // this was silently ignored because the code read the note byte (60)
    // as the controller index.
    tracker.process(UmpPacket::registered_per_note_cc(0, 1, 60, 74, 0xFFFFFFFFu));
    const auto* n = tracker.find(1, 60);
    REQUIRE(n != nullptr);
    REQUIRE(n->timbre > 0.9f);
}

TEST_CASE("MpeVoiceTracker note-off via UMP status 0x80", "[midi][ump][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(UmpPacket::note_on_2(0, 3, 64, 0x8000));
    REQUIRE(tracker.active_count() == 1);
    tracker.process(UmpPacket::note_off_2(0, 3, 64));
    REQUIRE(tracker.active_count() == 0);
}
