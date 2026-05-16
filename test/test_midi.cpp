#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <pulp/midi/midi.hpp>
#include <pulp/midi/midi_file.hpp>
#include <pulp/midi/midi_message_sequence.hpp>
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
