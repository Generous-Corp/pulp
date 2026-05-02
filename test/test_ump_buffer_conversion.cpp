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

TEST_CASE("UmpPacket helper factories mask groups channels and data fields",
          "[midi][ump][helpers]") {
    auto note = UmpPacket::note_on_2(0x2F, 0x1E, 0xFF, 0x1234, 0x8F, 0xABCD);
    REQUIRE(note.word_count == 2);
    REQUIRE(note.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(note.group() == 0x0F);
    REQUIRE(note.channel() == 0x0E);
    REQUIRE(note.note_number() == 0x7F);
    REQUIRE(note.attribute_type() == 0x8F);
    REQUIRE(note.attribute_data() == 0xABCD);

    auto per_note = UmpPacket::registered_per_note_cc(0x10, 0x11, 0xFE, 0xFF,
                                                       0xCAFEBABEu);
    REQUIRE(per_note.group() == 0);
    REQUIRE(per_note.channel() == 1);
    REQUIRE(per_note.note_number() == 0x7E);
    REQUIRE(per_note.attribute_type() == 0x7F);
    REQUIRE(per_note.data_32() == 0xCAFEBABEu);

    auto midi1 = UmpPacket::midi1_note_on(0x12, 0x13, 0xFC, 0xFE);
    REQUIRE(midi1.word_count == 1);
    REQUIRE(midi1.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(midi1.group() == 2);
    REQUIRE(midi1.status() == 0x93);
    REQUIRE(((midi1.words[0] >> 8) & 0xFFu) == 0x7Cu);
    REQUIRE((midi1.words[0] & 0xFFu) == 0x7Eu);
}

TEST_CASE("UmpPacket remaining channel voice helpers encode masked fields",
          "[midi][ump][helpers][issue-645]") {
    auto off = UmpPacket::note_off_2(0x21, 0x2F, 0xFE, 0xCAFE);
    REQUIRE(off.word_count == 2);
    REQUIRE(off.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(off.group() == 1);
    REQUIRE(off.status() == 0x8F);
    REQUIRE(off.channel() == 0x0F);
    REQUIRE(off.note_number() == 0x7E);
    REQUIRE(off.velocity_16() == 0xCAFE);

    auto cc = UmpPacket::cc_2(0x3A, 0x2B, 0xFF, 0x12345678u);
    REQUIRE(cc.word_count == 2);
    REQUIRE(cc.group() == 0x0A);
    REQUIRE(cc.status() == 0xBB);
    REQUIRE(cc.channel() == 0x0B);
    REQUIRE(cc.note_number() == 0x7F);
    REQUIRE(cc.data_32() == 0x12345678u);

    auto bend = UmpPacket::pitch_bend_2(0x1C, 0x1D, 0x87654321u);
    REQUIRE(bend.word_count == 2);
    REQUIRE(bend.group() == 0x0C);
    REQUIRE(bend.status() == 0xED);
    REQUIRE(bend.channel() == 0x0D);
    REQUIRE(bend.data_32() == 0x87654321u);

    auto per_note_bend = UmpPacket::per_note_pitch_bend(0x2E, 0x3F, 0xFD,
                                                        0xABCDEF01u);
    REQUIRE(per_note_bend.word_count == 2);
    REQUIRE(per_note_bend.group() == 0x0E);
    REQUIRE(per_note_bend.status() == 0x6F);
    REQUIRE(per_note_bend.channel() == 0x0F);
    REQUIRE(per_note_bend.note_number() == 0x7D);
    REQUIRE(per_note_bend.data_32() == 0xABCDEF01u);
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
    REQUIRE(scale_14_to_32(0) == 0);
    REQUIRE(scale_14_to_32(0x2000) == 0x80000000u);
    REQUIRE(scale_14_to_32(0x3FFF) == 0xFFFFFFFFu);
    REQUIRE(scale_32_to_14(0) == 0);
    REQUIRE(scale_32_to_14(0x80000000u) == 0x2000);
    REQUIRE(scale_32_to_14(0xFFFFFFFFu) == 0x3FFF);
}

TEST_CASE("midi1_event_to_ump2 handles note-off aliases and raw fallbacks",
          "[midi][ump][issue-645]") {
    auto velocity_zero_note_on = midi1_event_to_ump2(MidiEvent::note_on(2, 64, 0), 3);
    REQUIRE(velocity_zero_note_on.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(velocity_zero_note_on.group() == 3);
    REQUIRE(velocity_zero_note_on.status() == 0x82);
    REQUIRE(velocity_zero_note_on.note_number() == 64);
    REQUIRE(velocity_zero_note_on.velocity_16() == 0);

    auto program = MidiEvent::program_change(5, 12);
    auto fallback = midi1_event_to_ump2(program, 0x1F);
    REQUIRE(fallback.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(fallback.word_count == 1);
    REQUIRE(fallback.group() == 0x0F);
    REQUIRE(fallback.status() == 0xC5);

    MidiEvent out{};
    REQUIRE(ump_to_midi1_event(fallback, out));
    REQUIRE(out.is_program_change());
    REQUIRE(out.channel() == 5);
    REQUIRE(out.data()[1] == 12);
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

TEST_CASE("midi1_to_ump preserves sample offsets and group metadata",
          "[midi][ump][issue-645]") {
    MidiBuffer in;
    auto note = MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 128;
    auto cc = MidiEvent::cc(1, 74, 127);
    cc.sample_offset = 256;
    in.add(note);
    in.add(cc);

    UmpBuffer out;
    midi1_to_ump(in, out, 7);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].sample_offset == 128);
    REQUIRE(out[0].packet.group() == 7);
    REQUIRE(out[1].sample_offset == 256);
    REQUIRE(out[1].packet.group() == 7);
    REQUIRE(out[1].packet.data_32() == 0xFE000000u);
}

TEST_CASE("ump_to_midi1_event converts MIDI2 edge values",
          "[midi][ump][issue-645]") {
    auto pitch_bend_value = [](const MidiEvent& ev) {
        return static_cast<uint16_t>(ev.data()[1] |
                                     (static_cast<uint16_t>(ev.data()[2]) << 7));
    };

    MidiEvent out{};
    REQUIRE(ump_to_midi1_event(UmpPacket::note_on_2(0, 9, 61, 1), out));
    REQUIRE(out.is_note_on());
    REQUIRE(out.channel() == 9);
    REQUIRE(out.note() == 61);
    REQUIRE(out.velocity() == 1);

    REQUIRE(ump_to_midi1_event(UmpPacket::note_off_2(0, 10, 62, 0xFFFF), out));
    REQUIRE(out.is_note_off());
    REQUIRE(out.channel() == 10);
    REQUIRE(out.note() == 62);
    REQUIRE(out.velocity() == 127);

    REQUIRE(ump_to_midi1_event(UmpPacket::cc_2(0, 3, 11, 0xFFFFFFFFu), out));
    REQUIRE(out.is_cc());
    REQUIRE(out.channel() == 3);
    REQUIRE(out.cc_number() == 11);
    REQUIRE(out.cc_value() == 127);

    REQUIRE(ump_to_midi1_event(UmpPacket::pitch_bend_2(0, 4, 0x80000000u), out));
    REQUIRE(out.is_pitch_bend());
    REQUIRE(out.channel() == 4);
    REQUIRE(pitch_bend_value(out) == 0x2000);
}

TEST_CASE("ump_to_midi1 skips packets with no MIDI1 equivalent",
          "[midi][ump][issue-645]") {
    MidiEvent out{};
    UmpPacket utility{};
    utility.word_count = 1;
    utility.words[0] = 0;
    REQUIRE_FALSE(ump_to_midi1_event(utility, out));
    REQUIRE_FALSE(ump_to_midi1_event(
        UmpPacket::per_note_pitch_bend(0, 1, 60, 0x80000000u), out));

    UmpBuffer in;
    in.add(UmpPacket::note_on_2(0, 1, 60, 0x8000), 17);
    in.add(UmpPacket::per_note_pitch_bend(0, 1, 60, 0x90000000u), 23);
    in.add(UmpPacket::midi1_note_on(0, 2, 67, 100), 31);

    MidiBuffer flattened;
    ump_to_midi1(in, flattened);
    REQUIRE(flattened.size() == 2);
    REQUIRE(flattened[0].is_note_on());
    REQUIRE(flattened[0].sample_offset == 17);
    REQUIRE(flattened[1].is_note_on());
    REQUIRE(flattened[1].channel() == 2);
    REQUIRE(flattened[1].sample_offset == 31);
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
