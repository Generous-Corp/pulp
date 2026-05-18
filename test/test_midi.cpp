#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/midi/midi.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/midi_file.hpp>
#include <pulp/midi/midi_message_sequence.hpp>
#include <pulp/midi/ump_buffer.hpp>
#include <pulp/midi/ump_conversion.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

using namespace pulp::midi;
using Catch::Approx;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;

    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("pulp-midi-test-" + std::to_string(stamp));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void require_bytes(const MidiEvent& event,
                   uint8_t status,
                   uint8_t data1,
                   uint8_t data2) {
    REQUIRE(event.data()[0] == status);
    REQUIRE(event.data()[1] == data1);
    REQUIRE(event.data()[2] == data2);
}

} // namespace

TEST_CASE("MidiEvent factory methods", "[midi][message]") {
    SECTION("Note on") {
        auto evt = MidiEvent::note_on(0, 60, 100);
        REQUIRE(evt.is_note_on());
        REQUIRE_FALSE(evt.is_note_off());
        REQUIRE(evt.channel() == 0);
        REQUIRE(evt.note() == 60);
        REQUIRE(evt.velocity() == 100);
        REQUIRE(evt.size() == 3);
        require_bytes(evt, 0x90, 60, 100);
    }

    SECTION("Note off") {
        auto evt = MidiEvent::note_off(1, 64, 0);
        REQUIRE(evt.is_note_off());
        REQUIRE_FALSE(evt.is_note_on());
        REQUIRE(evt.channel() == 1);
        REQUIRE(evt.note() == 64);
        require_bytes(evt, 0x81, 64, 0);
    }

    SECTION("Note on with velocity 0 is note off") {
        auto evt = MidiEvent::note_on(0, 60, 0);
        REQUIRE(evt.is_note_off());
        REQUIRE_FALSE(evt.is_note_on());
    }

    SECTION("Control change") {
        auto evt = MidiEvent::cc(2, 74, 127);
        REQUIRE(evt.is_cc());
        REQUIRE(evt.channel() == 2);
        REQUIRE(evt.cc_number() == 74);
        REQUIRE(evt.cc_value() == 127);
        require_bytes(evt, 0xB2, 74, 127);
    }

    SECTION("Pitch bend") {
        auto evt = MidiEvent::pitch_bend(0, 8192);
        REQUIRE(evt.is_pitch_bend());
        REQUIRE(evt.channel() == 0);
        require_bytes(evt, 0xE0, 0, 64);
    }

    SECTION("Program change") {
        auto evt = MidiEvent::program_change(3, 42);
        REQUIRE(evt.is_program_change());
        REQUIRE(evt.channel() == 3);
        require_bytes(evt, 0xC3, 42, 0);
    }
}

TEST_CASE("MidiEvent factory methods mask MIDI data-byte boundaries",
          "[midi][message][issue-645]") {
    SECTION("Channel values wrap to the low nibble") {
        auto evt = MidiEvent::note_on(0x2F, 60, 100);
        REQUIRE(evt.channel() == 15);
        require_bytes(evt, 0x9F, 60, 100);
    }

    SECTION("Note and velocity arguments stay in the 7-bit data range") {
        auto on = MidiEvent::note_on(0, 0xC0, 0xFF);
        REQUIRE(on.note() == 0x40);
        REQUIRE(on.velocity() == 0x7F);
        require_bytes(on, 0x90, 0x40, 0x7F);

        auto off = MidiEvent::note_off(1, 0x81, 0xFE);
        REQUIRE(off.note() == 0x01);
        REQUIRE(off.velocity() == 0x7E);
        require_bytes(off, 0x81, 0x01, 0x7E);
    }

    SECTION("Controller and program arguments stay in the 7-bit data range") {
        auto cc = MidiEvent::cc(2, 0xF4, 0xC8);
        REQUIRE(cc.cc_number() == 0x74);
        REQUIRE(cc.cc_value() == 0x48);
        require_bytes(cc, 0xB2, 0x74, 0x48);

        auto pc = MidiEvent::program_change(3, 0xAA);
        require_bytes(pc, 0xC3, 0x2A, 0);
    }

    SECTION("Pitch bend arguments stay in the 14-bit data range") {
        auto evt = MidiEvent::pitch_bend(4, 0xFFFF);
        require_bytes(evt, 0xE4, 0x7F, 0x7F);
    }
}

TEST_CASE("MidiBuffer operations", "[midi][buffer]") {
    MidiBuffer buf;

    REQUIRE(buf.empty());
    REQUIRE(buf.size() == 0);

    buf.add(MidiEvent::note_on(0, 60, 100));
    buf.add(MidiEvent::note_on(0, 64, 100));
    buf.add(MidiEvent::note_on(0, 67, 100));

    REQUIRE(buf.size() == 3);
    REQUIRE_FALSE(buf.empty());

    SECTION("Iteration") {
        int count = 0;
        for (const auto& evt : buf) {
            REQUIRE(evt.is_note_on());
            ++count;
        }
        REQUIRE(count == 3);
    }

    SECTION("Sort by sample offset") {
        MidiBuffer sorted_buf;
        auto e1 = MidiEvent::note_on(0, 60, 100);
        e1.sample_offset = 200;
        auto e2 = MidiEvent::note_on(0, 64, 100);
        e2.sample_offset = 50;
        auto e3 = MidiEvent::note_on(0, 67, 100);
        e3.sample_offset = 100;

        sorted_buf.add(e1);
        sorted_buf.add(e2);
        sorted_buf.add(e3);
        sorted_buf.sort();

        REQUIRE(sorted_buf[0].sample_offset == 50);
        REQUIRE(sorted_buf[1].sample_offset == 100);
        REQUIRE(sorted_buf[2].sample_offset == 200);
    }

    SECTION("Clear") {
        buf.clear();
        REQUIRE(buf.empty());
    }
}

TEST_CASE("MidiBuffer stores independent SysEx sidecar events",
          "[midi][buffer][sysex][codecov]") {
    MidiBuffer buffer;
    buffer.add(MidiEvent::note_on(0, 60, 100));
    buffer.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 12, 0.25);
    buffer.add_sysex({}, 24, 0.5);

    REQUIRE(buffer.size() == 1);
    REQUIRE(buffer.sysex_size() == 2);
    REQUIRE(buffer.sysex()[0].data == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});
    REQUIRE(buffer.sysex()[0].sample_offset == 12);
    REQUIRE(buffer.sysex()[0].timestamp == Approx(0.25));
    REQUIRE(buffer.sysex()[1].data.empty());
    REQUIRE(buffer.sysex()[1].sample_offset == 24);

    buffer.sysex()[1].data = {0xF0, 0xF7};
    REQUIRE(buffer.sysex()[1].data == std::vector<uint8_t>{0xF0, 0xF7});

    buffer.clear_sysex();
    REQUIRE(buffer.sysex_size() == 0);
    REQUIRE(buffer.size() == 1);

    buffer.clear();
    REQUIRE(buffer.empty());
}

TEST_CASE("MidiKeyboardState tracks notes and releases with callbacks",
          "[midi][keyboard][codecov]") {
    MidiKeyboardState keys;
    std::vector<int> note_on_notes;
    std::vector<int> note_off_notes;

    keys.on_note_on = [&](uint8_t channel, uint8_t note, uint8_t velocity) {
        REQUIRE(channel == 2);
        REQUIRE(velocity > 0);
        note_on_notes.push_back(note);
    };
    keys.on_note_off = [&](uint8_t channel, uint8_t note) {
        REQUIRE(channel == 2);
        note_off_notes.push_back(note);
    };

    keys.process(MidiEvent::note_on(2, 64, 90));
    keys.process(MidiEvent::note_on(2, 67, 70));

    REQUIRE(keys.is_note_on(2, 64));
    REQUIRE(keys.velocity(2, 64) == 90);
    REQUIRE(keys.notes_held(2) == 2);
    REQUIRE(keys.total_notes_held() == 2);
    REQUIRE(keys.any_notes_held());
    REQUIRE(keys.lowest_note(2) == 64);
    REQUIRE(keys.highest_note(2) == 67);

    keys.process(MidiEvent::note_on(2, 64, 0));
    REQUIRE_FALSE(keys.is_note_on(2, 64));
    REQUIRE(keys.notes_held(2) == 1);

    keys.all_notes_off(2);
    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE(note_on_notes == std::vector<int>{64, 67});
    REQUIRE(note_off_notes == std::vector<int>{64, 67});
}

TEST_CASE("MidiKeyboardState handles invalid channels and reset without callbacks",
          "[midi][keyboard][codecov]") {
    MidiKeyboardState keys;
    int note_off_count = 0;
    keys.on_note_off = [&](uint8_t, uint8_t) { ++note_off_count; };

    keys.process(MidiEvent::note_on(0, 60, 100));
    keys.process(MidiEvent::cc(0, 74, 127));

    REQUIRE(keys.velocity(16, 60) == 0);
    REQUIRE(keys.velocity(0, 128) == 0);
    REQUIRE(keys.notes_held(16) == 0);
    REQUIRE(keys.lowest_note(16) == -1);
    REQUIRE(keys.highest_note(16) == -1);

    keys.all_notes_off(16);
    REQUIRE(note_off_count == 0);

    keys.reset();
    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE(note_off_count == 0);
}

TEST_CASE("MidiKeyboardState all_notes_off releases every channel in order",
          "[midi][keyboard][codecov]") {
    MidiKeyboardState keys;
    std::vector<std::pair<int, int>> released;
    keys.on_note_off = [&](uint8_t channel, uint8_t note) {
        released.emplace_back(channel, note);
    };

    keys.process(MidiEvent::note_on(1, 60, 100));
    keys.process(MidiEvent::note_on(0, 64, 80));
    keys.process(MidiEvent::note_on(1, 67, 90));

    REQUIRE(keys.total_notes_held() == 3);
    keys.all_notes_off();

    REQUIRE_FALSE(keys.any_notes_held());
    REQUIRE((released == std::vector<std::pair<int, int>>{
        {0, 64},
        {1, 60},
        {1, 67},
    }));
}

TEST_CASE("MidiKeyboardState channel release preserves other held notes",
          "[midi][keyboard][codecov]") {
    MidiKeyboardState keys;
    std::vector<std::pair<int, int>> released;
    keys.on_note_off = [&](uint8_t channel, uint8_t note) {
        released.emplace_back(channel, note);
    };

    keys.process(MidiEvent::note_on(0, 48, 100));
    keys.process(MidiEvent::note_on(2, 60, 96));
    keys.process(MidiEvent::note_on(2, 64, 80));
    keys.process(MidiEvent::note_on(5, 72, 70));

    keys.all_notes_off(2);

    REQUIRE_FALSE(keys.is_note_on(2, 60));
    REQUIRE_FALSE(keys.is_note_on(2, 64));
    REQUIRE(keys.is_note_on(0, 48));
    REQUIRE(keys.is_note_on(5, 72));
    REQUIRE(keys.total_notes_held() == 2);
    REQUIRE((released == std::vector<std::pair<int, int>>{
        {2, 60},
        {2, 64},
    }));
}

TEST_CASE("RpnParser emits RPN NRPN and increment callbacks",
          "[midi][rpn][codecov]") {
    RpnParser parser;
    std::vector<uint16_t> rpn_params;
    std::vector<uint16_t> rpn_values;
    std::vector<uint16_t> nrpn_params;
    std::vector<uint16_t> nrpn_values;
    std::vector<bool> increment_is_rpn;
    std::vector<bool> decrement_is_rpn;

    parser.on_rpn = [&](uint8_t channel, uint16_t param, uint16_t value) {
        REQUIRE(channel == 3);
        rpn_params.push_back(param);
        rpn_values.push_back(value);
    };
    parser.on_nrpn = [&](uint8_t channel, uint16_t param, uint16_t value) {
        REQUIRE(channel == 3);
        nrpn_params.push_back(param);
        nrpn_values.push_back(value);
    };
    parser.on_increment = [&](uint8_t channel, uint16_t param, bool is_rpn) {
        REQUIRE(channel == 3);
        REQUIRE(param == ((2u << 7) | 5u));
        increment_is_rpn.push_back(is_rpn);
    };
    parser.on_decrement = [&](uint8_t channel, uint16_t param, bool is_rpn) {
        REQUIRE(channel == 3);
        REQUIRE(param == ((2u << 7) | 5u));
        decrement_is_rpn.push_back(is_rpn);
    };

    parser.process(MidiEvent::program_change(3, 1));
    parser.process(MidiEvent::cc(3, 6, 12));
    parser.process(MidiEvent::cc(3, 38, 34));
    REQUIRE(rpn_params.empty());

    parser.process(MidiEvent::cc(3, 101, 2));
    parser.process(MidiEvent::cc(3, 100, 5));
    parser.process(MidiEvent::cc(3, 6, 12));
    parser.process(MidiEvent::cc(3, 38, 34));
    parser.process(MidiEvent::cc(3, 96, 0));
    parser.process(MidiEvent::cc(3, 97, 0));

    REQUIRE(rpn_params == std::vector<uint16_t>{static_cast<uint16_t>((2u << 7) | 5u)});
    REQUIRE(rpn_values == std::vector<uint16_t>{static_cast<uint16_t>((12u << 7) | 34u)});
    REQUIRE(increment_is_rpn == std::vector<bool>{true});
    REQUIRE(decrement_is_rpn == std::vector<bool>{true});

    parser.process(MidiEvent::cc(3, 99, 7));
    parser.process(MidiEvent::cc(3, 98, 8));
    parser.process(MidiEvent::cc(3, 6, 1));
    parser.process(MidiEvent::cc(3, 38, 2));

    REQUIRE(nrpn_params == std::vector<uint16_t>{static_cast<uint16_t>((7u << 7) | 8u)});
    REQUIRE(nrpn_values == std::vector<uint16_t>{static_cast<uint16_t>((1u << 7) | 2u)});
}

TEST_CASE("RpnParser reset clears active parameter selection",
          "[midi][rpn][codecov]") {
    RpnParser parser;
    int callbacks = 0;
    parser.on_rpn = [&](uint8_t, uint16_t, uint16_t) { ++callbacks; };

    parser.process(MidiEvent::cc(0, 101, 0));
    parser.process(MidiEvent::cc(0, 100, 0));
    parser.reset();
    parser.process(MidiEvent::cc(0, 6, 2));
    parser.process(MidiEvent::cc(0, 38, 0));

    REQUIRE(callbacks == 0);
}

TEST_CASE("MidiMessageSequence sorts ranges and matches note-offs",
          "[midi][sequence][codecov]") {
    MidiMessageSequence sequence;
    sequence.add_note_off(2.0, 1, 64);
    sequence.add_note_on(0.5, 1, 64, 100);
    sequence.add_cc(1.0, 1, 74, 80);
    sequence.add_note_on(1.5, 2, 64, 100);

    REQUIRE(sequence.size() == 4);
    REQUIRE(sequence.duration() == Approx(2.0));
    REQUIRE(sequence[0].is_note_on());
    REQUIRE(sequence[0].channel() == 1);
    REQUIRE(sequence[0].note() == 64);
    REQUIRE(sequence[0].velocity() == 100);
    REQUIRE(sequence[1].is_cc());

    auto in_range = sequence.events_in_range(0.75, 2.0);
    REQUIRE(in_range.size() == 2);
    REQUIRE(in_range[0]->is_cc());
    REQUIRE(in_range[1]->is_note_on());

    auto note_off = sequence.find_note_off(0);
    REQUIRE(note_off.has_value());
    REQUIRE(*note_off == 3);
    REQUIRE_FALSE(sequence.find_note_off(1).has_value());
    REQUIRE_FALSE(sequence.find_note_off(-1).has_value());
    REQUIRE_FALSE(sequence.find_note_off(99).has_value());

    sequence.offset_timestamps(0.25);
    REQUIRE(sequence[0].timestamp == Approx(0.75));

    int iterated = 0;
    for (const auto& event : sequence) {
        REQUIRE(event.timestamp >= 0.75);
        ++iterated;
    }
    REQUIRE(iterated == sequence.size());

    sequence.clear();
    REQUIRE(sequence.size() == 0);
    REQUIRE(sequence.duration() == Approx(0.0));
}

TEST_CASE("UmpPacket factories expose MIDI 2.0 fields", "[midi][ump][codecov]") {
    auto note = UmpPacket::note_on_2(0x1F, 0x2F, 0xC0, 0xABCD, 0xEE, 0x1234);
    REQUIRE(note.word_count == 2);
    REQUIRE(note.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(note.group() == 0x0F);
    REQUIRE(note.status() == 0x9F);
    REQUIRE(note.channel() == 0x0F);
    REQUIRE(note.note_number() == 0x40);
    REQUIRE(note.velocity_16() == 0xABCD);
    REQUIRE(note.velocity_7() == (0xABCD >> 9));
    REQUIRE(note.attribute_type() == 0xEE);
    REQUIRE(note.attribute_data() == 0x1234);

    auto off = UmpPacket::note_off_2(2, 3, 64, 0x4000);
    REQUIRE(off.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(off.status() == 0x83);
    REQUIRE(off.velocity_16() == 0x4000);

    auto cc = UmpPacket::cc_2(1, 2, 74, 0xDEADBEEFu);
    REQUIRE(cc.status() == 0xB2);
    REQUIRE(cc.note_number() == 74);
    REQUIRE(cc.data_32() == 0xDEADBEEFu);

    auto bend = UmpPacket::pitch_bend_2(1, 2, 0x80000000u);
    REQUIRE(bend.status() == 0xE2);
    REQUIRE(bend.data_32() == 0x80000000u);

    auto per_note_bend = UmpPacket::per_note_pitch_bend(3, 4, 65, 0x11112222u);
    REQUIRE(per_note_bend.status() == 0x64);
    REQUIRE(per_note_bend.note_number() == 65);
    REQUIRE(per_note_bend.data_32() == 0x11112222u);

    auto per_note_cc = UmpPacket::registered_per_note_cc(3, 4, 65, 12, 0x33334444u);
    REQUIRE(per_note_cc.channel() == 4);
    REQUIRE(per_note_cc.note_number() == 65);
    REQUIRE(per_note_cc.attribute_type() == 12);
    REQUIRE(per_note_cc.data_32() == 0x33334444u);

    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Utility) == 1);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::DataSysEx) == 2);
    REQUIRE(UmpPacket::size_for_type(UmpMessageType::Data128) == 4);
}

TEST_CASE("UMP conversion scales MIDI 1.0 events both directions",
          "[midi][ump][codecov]") {
    REQUIRE(scale_7_to_16(0) == 0);
    REQUIRE(scale_7_to_16(127) == 0xFFFF);
    REQUIRE(scale_16_to_7(0xFFFF) == 127);
    REQUIRE(scale_14_to_32(0x2000) == 0x80000000u);
    REQUIRE(scale_32_to_14(0x80000000u) == 0x2000);

    auto note_on = midi1_event_to_ump2(MidiEvent::note_on(2, 60, 100), 7);
    REQUIRE(note_on.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(note_on.group() == 7);
    REQUIRE(note_on.status() == 0x92);
    REQUIRE(note_on.velocity_16() == scale_7_to_16(100));

    auto zero_velocity = midi1_event_to_ump2(MidiEvent::note_on(2, 60, 0), 7);
    REQUIRE(zero_velocity.status() == 0x82);

    auto cc = midi1_event_to_ump2(MidiEvent::cc(1, 74, 64), 2);
    REQUIRE(cc.status() == 0xB1);
    REQUIRE(cc.data_32() == (64u << 25));

    auto program = midi1_event_to_ump2(MidiEvent::program_change(3, 42), 5);
    REQUIRE(program.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(program.word_count == 1);

    MidiEvent out;
    REQUIRE(ump_to_midi1_event(note_on, out));
    REQUIRE(out.is_note_on());
    REQUIRE(out.channel() == 2);
    REQUIRE(out.note() == 60);
    REQUIRE(out.velocity() == 100);

    auto quiet_note_on = UmpPacket::note_on_2(0, 4, 61, 1);
    REQUIRE(ump_to_midi1_event(quiet_note_on, out));
    REQUIRE(out.is_note_on());
    REQUIRE(out.velocity() == 1);

    REQUIRE(ump_to_midi1_event(UmpPacket::pitch_bend_2(0, 5, 0x80000000u), out));
    REQUIRE(out.data()[0] == static_cast<uint8_t>(0xE0 | 5));
    REQUIRE((out.data()[1] | (uint16_t(out.data()[2]) << 7)) == 0x2000);

    REQUIRE_FALSE(ump_to_midi1_event(UmpPacket::per_note_pitch_bend(0, 1, 60, 0), out));
}

TEST_CASE("UMP conversion covers note-off cc and unsupported packet paths",
          "[midi][ump][codecov]") {
    auto note_off = midi1_event_to_ump2(MidiEvent::note_off(9, 36, 64), 12);
    REQUIRE(note_off.message_type() == UmpMessageType::Midi2ChannelVoice);
    REQUIRE(note_off.group() == 12);
    REQUIRE(note_off.status() == static_cast<uint8_t>(0x80 | 9));
    REQUIRE(note_off.note_number() == 36);
    REQUIRE(note_off.velocity_16() == scale_7_to_16(64));

    auto cc = UmpPacket::cc_2(3, 4, 74, 127u << 25);
    MidiEvent out;
    REQUIRE(ump_to_midi1_event(cc, out));
    REQUIRE(out.is_cc());
    REQUIRE(out.channel() == 4);
    REQUIRE(out.cc_number() == 74);
    REQUIRE(out.cc_value() == 127);

    UmpPacket utility{};
    utility.word_count = UmpPacket::size_for_type(UmpMessageType::Utility);
    utility.words[0] = 0;
    REQUIRE_FALSE(ump_to_midi1_event(utility, out));
}

TEST_CASE("UMP conversion preserves MIDI 1.0 fallback bytes",
          "[midi][ump][codecov]") {
    auto program = midi1_event_to_ump2(MidiEvent::program_change(5, 42), 15);

    REQUIRE(program.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(program.group() == 15);
    REQUIRE(program.word_count == 1);

    MidiEvent out;
    REQUIRE(ump_to_midi1_event(program, out));
    REQUIRE(out.is_program_change());
    REQUIRE(out.channel() == 5);
    REQUIRE(out.data()[1] == 42);
    REQUIRE(out.data()[2] == 0);
}

TEST_CASE("MIDI 1.0 fallback conversion preserves requested group and offsets",
          "[midi][ump][buffer][codecov]") {
    MidiBuffer midi;
    auto program = MidiEvent::program_change(14, 91);
    program.sample_offset = 256;
    midi.add(program);

    UmpBuffer ump;
    midi1_to_ump(midi, ump, 11);

    REQUIRE(ump.size() == 1);
    REQUIRE(ump[0].sample_offset == 256);
    REQUIRE(ump[0].packet.message_type() == UmpMessageType::Midi1ChannelVoice);
    REQUIRE(ump[0].packet.group() == 11);
    REQUIRE(((ump[0].packet.words[0] >> 16) & 0xFF) == 0xCE);
    REQUIRE(((ump[0].packet.words[0] >> 8) & 0xFF) == 91);
    REQUIRE((ump[0].packet.words[0] & 0xFF) == 0);
}

TEST_CASE("UMP scaling helpers preserve boundary values around pitch center",
          "[midi][ump][codecov]") {
    REQUIRE(scale_14_to_32(0) == 0);
    REQUIRE(scale_14_to_32(0x3FFF) == 0xFFFFFFFFu);
    REQUIRE(scale_32_to_14(0) == 0);
    REQUIRE(scale_32_to_14(0xFFFFFFFFu) == 0x3FFF);
    REQUIRE(scale_32_to_14(scale_14_to_32(0x1FFF)) == 0x1FFF);
    REQUIRE(scale_32_to_14(scale_14_to_32(0x2001)) == 0x2000);
}

TEST_CASE("UmpBuffer and MidiBuffer bridge preserve order and offsets",
          "[midi][ump][buffer][codecov]") {
    MidiBuffer midi;
    auto note = MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 32;
    auto cc = MidiEvent::cc(0, 74, 64);
    cc.sample_offset = 8;
    midi.add(note);
    midi.add(cc);

    UmpBuffer ump;
    midi.attach_ump(&ump);
    REQUIRE(midi.ump() == &ump);

    midi1_to_ump(midi, ump, 3);
    REQUIRE(ump.size() == 2);
    REQUIRE(ump[0].sample_offset == 32);
    REQUIRE(ump[1].sample_offset == 8);
    ump.sort();
    REQUIRE(ump[0].sample_offset == 8);
    REQUIRE(ump[1].sample_offset == 32);

    MidiBuffer round_trip;
    ump_to_midi1(ump, round_trip);
    REQUIRE(round_trip.size() == 2);
    REQUIRE(round_trip[0].is_cc());
    REQUIRE(round_trip[0].sample_offset == 8);
    REQUIRE(round_trip[1].is_note_on());
    REQUIRE(round_trip[1].sample_offset == 32);

    ump.clear();
    REQUIRE(ump.empty());
}

TEST_CASE("UmpBuffer flatten skips packets without MIDI 1.0 equivalents",
          "[midi][ump][buffer][codecov]") {
    UmpBuffer ump;
    ump.add({UmpPacket::note_on_2(1, 2, 60, scale_7_to_16(96)), 12});
    ump.add({UmpPacket::per_note_pitch_bend(1, 2, 60, 0x90000000u), 18});
    ump.add({UmpPacket::cc_2(1, 2, 74, 64u << 25), 24});

    MidiBuffer midi;
    ump_to_midi1(ump, midi);

    REQUIRE(midi.size() == 2);
    REQUIRE(midi[0].is_note_on());
    REQUIRE(midi[0].sample_offset == 12);
    REQUIRE(midi[1].is_cc());
    REQUIRE(midi[1].cc_number() == 74);
    REQUIRE(midi[1].sample_offset == 24);
}

TEST_CASE("MPE buffer binding records tracker callbacks with sample offsets",
          "[midi][mpe][buffer][codecov]") {
    MpeVoiceTracker tracker(MpeConfig::standard_lower(2));
    MpeBuffer buffer;
    int32_t sample_offset = 12;
    bind_tracker_to_buffer(tracker, buffer, sample_offset);

    REQUIRE(buffer.empty());
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 96)));

    sample_offset = 24;
    REQUIRE(tracker.process(MidiEvent::pitch_bend(1, 0x3FFF)));

    sample_offset = 18;
    REQUIRE(tracker.process(MidiEvent::cc(1, 74, 64)));

    sample_offset = 36;
    REQUIRE(tracker.process(MidiEvent::note_off(1, 60)));

    REQUIRE(buffer.size() == 4);
    REQUIRE(buffer[0].kind == MpeExpressionEvent::Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 12);
    REQUIRE(buffer[0].state.note == 60);
    REQUIRE(buffer[1].kind == MpeExpressionEvent::Kind::PitchBend);
    REQUIRE(buffer[1].sample_offset == 24);
    REQUIRE(buffer[2].kind == MpeExpressionEvent::Kind::Timbre);
    REQUIRE(buffer[2].sample_offset == 18);
    REQUIRE(buffer[3].kind == MpeExpressionEvent::Kind::NoteOff);
    REQUIRE(buffer[3].sample_offset == 36);

    buffer.sort();
    REQUIRE(buffer[0].sample_offset == 12);
    REQUIRE(buffer[1].sample_offset == 18);
    REQUIRE(buffer[2].sample_offset == 24);
    REQUIRE(buffer[3].sample_offset == 36);

    int seen = 0;
    for (const auto& event : buffer) {
        REQUIRE(event.state.channel == 1);
        ++seen;
    }
    REQUIRE(seen == 4);

    buffer.clear();
    REQUIRE(buffer.empty());
}

TEST_CASE("MidiFileData summarizes tracks", "[midi][file]") {
    MidiFileData data;
    MidiTrack first;
    first.events.push_back({0.25, MidiEvent::note_on(0, 60, 100)});
    first.events.push_back({1.50, MidiEvent::note_off(0, 60)});

    MidiTrack second;
    second.events.push_back({0.75, MidiEvent::cc(1, 74, 64)});

    data.tracks.push_back(std::move(first));
    data.tracks.push_back(std::move(second));

    REQUIRE(data.total_events() == 3);
    REQUIRE(data.duration_seconds() == Approx(1.50).margin(1e-6));
}

TEST_CASE("MidiFileData handles empty tracks and empty file round-trips",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "empty.mid";

    MidiFileData data;
    data.ticks_per_quarter = 240;
    data.tracks.push_back(MidiTrack{});

    REQUIRE(data.total_events() == 0);
    REQUIRE(data.duration_seconds() == Approx(0.0).margin(1e-9));
    REQUIRE(write_midi_file(path.string(), data));

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 240);
    REQUIRE(read->total_events() == 0);
    REQUIRE(read->duration_seconds() == Approx(0.0).margin(1e-9));
    REQUIRE(read->tracks.size() == 1);
}

TEST_CASE("MidiFile read/write round-trips short messages", "[midi][file]") {
    TempDir tmp;
    const auto path = tmp.path / "roundtrip.mid";

    MidiFileData data;
    data.ticks_per_quarter = 960;

    MidiTrack track;
    track.events.push_back({0.00, MidiEvent::note_on(0, 60, 100)});
    track.events.push_back({0.50, MidiEvent::cc(0, 74, 127)});
    track.events.push_back({1.00, MidiEvent::note_off(0, 60)});
    data.tracks.push_back(std::move(track));

    REQUIRE(write_midi_file(path.string(), data));

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 960);
    REQUIRE(read->total_events() == 3);
    REQUIRE(read->duration_seconds() == Approx(1.0).margin(0.05));
}

TEST_CASE("MidiFile round-trips multi-track program change and pitch bend",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "multi-track.mid";

    MidiFileData data;
    data.ticks_per_quarter = 480;

    MidiTrack first;
    first.name = "programs";
    first.events.push_back({0.00, MidiEvent::program_change(2, 42)});

    MidiTrack second;
    second.name = "notes";
    second.events.push_back({0.25, MidiEvent::pitch_bend(2, 16383)});
    second.events.push_back({0.50, MidiEvent::note_on(2, 65, 100)});

    data.tracks.push_back(std::move(first));
    data.tracks.push_back(std::move(second));

    REQUIRE(write_midi_file(path.string(), data));

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);
    REQUIRE(read->total_events() == 3);
    REQUIRE(read->duration_seconds() > 0.0);

    bool saw_program = false;
    bool saw_bend = false;
    bool saw_note = false;
    for (const auto& track : read->tracks) {
        for (const auto& event : track.events) {
            saw_program = saw_program || event.event.is_program_change();
            saw_bend = saw_bend || event.event.is_pitch_bend();
            saw_note = saw_note || event.event.is_note_on();
            REQUIRE(event.event.timestamp == Approx(event.time_seconds).margin(1e-9));
        }
    }

    REQUIRE(saw_program);
    REQUIRE(saw_bend);
    REQUIRE(saw_note);
}

TEST_CASE("MidiFile helpers report missing, corrupt, and unwritable files", "[midi][file]") {
    TempDir tmp;

    REQUIRE_FALSE(read_midi_file((tmp.path / "missing.mid").string()).has_value());

    const auto corrupt = tmp.path / "corrupt.mid";
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "not a midi file";
    }
    REQUIRE_FALSE(read_midi_file(corrupt.string()).has_value());

    MidiFileData empty;
    REQUIRE_FALSE(write_midi_file((tmp.path / "missing-parent" / "out.mid").string(), empty));
}

#if defined(__APPLE__) && !TARGET_OS_IPHONE
TEST_CASE("CoreMIDI system enumerates ports", "[midi][coremidi]") {
    auto system = create_midi_system();
    REQUIRE(system != nullptr);

    // These may be empty if no MIDI devices are connected, but shouldn't crash
    auto inputs = system->enumerate_inputs();
    auto outputs = system->enumerate_outputs();

    // Just verify the calls succeed
    REQUIRE(true);
}
#endif
