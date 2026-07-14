#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <pulp/midi/midi_file.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace pulp::midi;
using Catch::Approx;

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;

    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto entropy = std::random_device{}();
        path = fs::temp_directory_path() / ("pulp-midi-file-test-"
            + std::to_string(stamp) + "-" + std::to_string(entropy));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.is_open());
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    REQUIRE(out.good());
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

TEST_CASE("MidiFileData aggregates empty and multi-track metadata",
          "[midi][file]") {
    MidiFileData empty;
    REQUIRE(empty.duration_seconds() == Approx(0.0).margin(1e-9));
    REQUIRE(empty.total_events() == 0);

    MidiFileData data;
    MidiTrack early;
    early.events.push_back({0.125, MidiEvent::note_on(0, 60, 100)});
    early.events.push_back({0.25, MidiEvent::note_off(0, 60)});

    MidiTrack late;
    late.events.push_back({1.5, MidiEvent::cc(1, 74, 64)});

    data.tracks.push_back(std::move(early));
    data.tracks.push_back(std::move(late));

    REQUIRE(data.total_events() == 3);
    REQUIRE(data.duration_seconds() == Approx(1.5).margin(1e-9));
}

TEST_CASE("MidiFileData aggregates duration and event counts across tracks",
          "[midi][file]") {
    MidiFileData data;
    REQUIRE(data.total_events() == 0);
    REQUIRE(data.duration_seconds() == Approx(0.0).margin(1e-9));

    MidiTrack first;
    first.events.push_back({0.25, MidiEvent::note_on(0, 60, 100)});
    first.events.push_back({0.75, MidiEvent::note_off(0, 60, 0)});

    MidiTrack second;
    second.events.push_back({0.5, MidiEvent::cc(1, 74, 64)});
    second.events.push_back({1.25, MidiEvent::program_change(1, 4)});

    data.tracks.push_back(std::move(first));
    data.tracks.push_back(std::move(second));

    REQUIRE(data.total_events() == 4);
    REQUIRE(data.duration_seconds() == Approx(1.25).margin(1e-9));
}

TEST_CASE("read_midi_file decodes running status byte fixtures",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "running-status.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x13,
        0x00, 0x90, 0x3C, 0x40,
        0x81, 0x70, 0x3E, 0x41,
        0x00, 0x80, 0x3C, 0x00,
        0x00, 0x3E, 0x00,
        0x00, 0xFF, 0x2F, 0x00,
    });

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);
    REQUIRE(read->tracks.size() == 1);
    REQUIRE(read->total_events() == 4);

    const auto& events = read->tracks.front().events;
    REQUIRE(events[0].time_seconds == Approx(0.0).margin(1e-9));
    REQUIRE(events[1].time_seconds == Approx(0.25).margin(1e-6));
    REQUIRE(events[2].time_seconds == Approx(0.25).margin(1e-6));
    REQUIRE(events[3].time_seconds == Approx(0.25).margin(1e-6));

    REQUIRE(events[0].event.is_note_on());
    REQUIRE(events[0].event.note() == 60);
    REQUIRE(events[0].event.velocity() == 64);
    REQUIRE(events[1].event.is_note_on());
    REQUIRE(events[1].event.note() == 62);
    REQUIRE(events[1].event.velocity() == 65);
    REQUIRE(events[2].event.is_note_off());
    REQUIRE(events[2].event.note() == 60);
    REQUIRE(events[3].event.is_note_off());
    REQUIRE(events[3].event.note() == 62);

    for (const auto& event : events)
        REQUIRE(event.event.timestamp == Approx(event.time_seconds).margin(1e-9));
}

TEST_CASE("read_midi_file applies tempo meta events and skips non-short messages",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "tempo-meta.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x14,
        0x00, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,
        0x00, 0x90, 0x3C, 0x40,
        0x81, 0x70, 0x80, 0x3C, 0x00,
        0x00, 0xFF, 0x2F, 0x00,
    });

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);
    REQUIRE(read->total_events() == 2);

    const auto& events = read->tracks.front().events;
    REQUIRE(events[0].event.is_note_on());
    REQUIRE(events[0].time_seconds == Approx(0.0).margin(1e-9));
    REQUIRE(events[1].event.is_note_off());
    REQUIRE(events[1].time_seconds == Approx(0.5).margin(1e-6));
    REQUIRE(read->duration_seconds() == Approx(0.5).margin(1e-6));
}

TEST_CASE("read_midi_file falls back for non-PPQ divisions",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "smpte-division.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0xE7, 0x28,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x04,
        0x00, 0xFF, 0x2F, 0x00,
    });

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);
    REQUIRE(read->tracks.size() == 1);
    REQUIRE(read->total_events() == 0);
    REQUIRE(read->duration_seconds() == Approx(0.0).margin(1e-9));
}

TEST_CASE("read_midi_file rejects truncated track chunks",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "truncated-track.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x08,
        0x00, 0xFF, 0x2F, 0x00,
    });

    REQUIRE_FALSE(read_midi_file(path.string()).has_value());
}

TEST_CASE("midi file helpers report missing and unwritable paths",
          "[midi][file]") {
    TempDir tmp;

    REQUIRE_FALSE(read_midi_file((tmp.path / "missing.mid").string()).has_value());

    MidiFileData data;
    data.ticks_per_quarter = 480;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    data.tracks.push_back(std::move(track));

    REQUIRE_FALSE(write_midi_file(tmp.path.string(), data));
}

TEST_CASE("read_midi_file rejects empty files and malformed SMF headers",
          "[midi][file][issue-645]") {
    TempDir tmp;

    // A zero-byte file has no header, so it is not a MIDI file and is refused.
    //
    // It used to be ACCEPTED, reporting a valid-looking empty file at 60 ticks
    // per quarter — a number that came from nowhere but a backend's default
    // member initializer and meant nothing. A caller could not tell that result
    // apart from a genuinely empty MIDI file, which is the whole problem: a
    // truncated download and an intentional file looked identical.
    const auto empty = tmp.path / "empty.mid";
    write_bytes(empty, {});
    REQUIRE_FALSE(read_midi_file(empty.string()).has_value());

    const auto bad_magic = tmp.path / "bad-magic.mid";
    write_bytes(bad_magic, {
        'N', 'O', 'P', 'E', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x04,
        0x00, 0xFF, 0x2F, 0x00,
    });
    REQUIRE_FALSE(read_midi_file(bad_magic.string()).has_value());

    const auto short_header = tmp.path / "short-header.mid";
    write_bytes(short_header, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00,
    });
    REQUIRE_FALSE(read_midi_file(short_header.string()).has_value());

    const auto missing_track = tmp.path / "missing-track.mid";
    write_bytes(missing_track, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
    });
    REQUIRE_FALSE(read_midi_file(missing_track.string()).has_value());
}

TEST_CASE("read_midi_file preserves short message channels and payloads",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "short-message-payloads.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x13,
        0x00, 0xC3, 0x2A,
        0x00, 0xD4, 0x55,
        0x00, 0xB5, 0x4A, 0x40,
        0x81, 0x70, 0xE6, 0x00, 0x40,
        0x00, 0xFF, 0x2F, 0x00,
    });

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);
    REQUIRE(read->tracks.size() == 1);
    REQUIRE(read->total_events() == 4);
    REQUIRE(read->duration_seconds() == Approx(0.25).margin(1e-6));

    const auto& events = read->tracks.front().events;
    REQUIRE(events[0].time_seconds == Approx(0.0).margin(1e-9));
    REQUIRE(events[0].event.is_program_change());
    REQUIRE(events[0].event.channel() == 3);
    REQUIRE(events[0].event.size() == 2);
    REQUIRE(events[0].event.data()[1] == 42);
    REQUIRE(events[1].time_seconds == Approx(0.0).margin(1e-9));
    REQUIRE(events[1].event.channel() == 4);
    REQUIRE(events[1].event.size() == 2);
    REQUIRE(events[1].event.data()[0] == 0xD4);
    REQUIRE(events[1].event.data()[1] == 0x55);
    REQUIRE(events[2].event.is_cc());
    REQUIRE(events[2].event.channel() == 5);
    REQUIRE(events[2].event.cc_number() == 74);
    REQUIRE(events[2].event.cc_value() == 64);
    REQUIRE(events[3].time_seconds == Approx(0.25).margin(1e-6));
    REQUIRE(events[3].event.is_pitch_bend());
    REQUIRE(events[3].event.channel() == 6);
    REQUIRE(events[3].event.data()[1] == 0x00);
    REQUIRE(events[3].event.data()[2] == 0x40);
}

TEST_CASE("write_midi_file emits a readable SMF header",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "written.mid";

    MidiFileData data;
    data.ticks_per_quarter = 960;

    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::program_change(1, 12)});
    track.events.push_back({0.5, MidiEvent::cc(1, 74, 99)});
    data.tracks.push_back(std::move(track));

    REQUIRE(write_midi_file(path.string(), data));

    const auto bytes = read_bytes(path);
    REQUIRE(bytes.size() > 22);
    REQUIRE(std::string(bytes.begin(), bytes.begin() + 4) == "MThd");
    REQUIRE(bytes[4] == 0x00);
    REQUIRE(bytes[5] == 0x00);
    REQUIRE(bytes[6] == 0x00);
    REQUIRE(bytes[7] == 0x06);
    REQUIRE(bytes[12] == 0x03);
    REQUIRE(bytes[13] == 0xC0);
    REQUIRE(std::string(bytes.begin() + 14, bytes.begin() + 18) == "MTrk");

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 960);
    REQUIRE(read->total_events() == 2);
    REQUIRE(read->duration_seconds() == Approx(0.5).margin(0.05));
}

TEST_CASE("write_midi_file preserves mixed short messages across tracks",
          "[midi][file][issue-645]") {
    TempDir tmp;
    const auto path = tmp.path / "mixed-short-messages.mid";

    MidiFileData data;
    data.ticks_per_quarter = 240;

    MidiTrack controls;
    controls.events.push_back({0.00, MidiEvent::cc(9, 1, 127)});
    controls.events.push_back({0.25, MidiEvent::program_change(9, 7)});

    MidiTrack notes;
    notes.events.push_back({0.50, MidiEvent::note_on(10, 36, 100)});
    notes.events.push_back({0.75, MidiEvent::pitch_bend(10, 8192)});
    notes.events.push_back({1.00, MidiEvent::note_off(10, 36, 12)});

    data.tracks.push_back(std::move(controls));
    data.tracks.push_back(std::move(notes));

    REQUIRE(data.total_events() == 5);
    REQUIRE(write_midi_file(path.string(), data));

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 240);
    REQUIRE(read->total_events() == 5);

    // The two tracks come back as TWO tracks. They used to be merged into one on
    // the way out and merged again on the way in, so a caller who wrote a drum
    // track and a bass track got a single track back with both interleaved, and
    // nothing anywhere said so.
    REQUIRE(read->tracks.size() == 2);

    const auto& controls_read = read->tracks[0].events;
    REQUIRE(controls_read.size() == 2);
    REQUIRE(controls_read[0].event.is_cc());
    REQUIRE(controls_read[0].event.cc_number() == 1);
    REQUIRE(controls_read[0].event.cc_value() == 127);
    REQUIRE(controls_read[1].event.is_program_change());
    REQUIRE(controls_read[1].event.data()[1] == 7);

    const auto& notes_read = read->tracks[1].events;
    REQUIRE(notes_read.size() == 3);
    REQUIRE(notes_read[0].event.is_note_on());
    REQUIRE(notes_read[0].event.note() == 36);
    REQUIRE(notes_read[0].event.velocity() == 100);
    REQUIRE(notes_read[1].event.is_pitch_bend());
    REQUIRE(notes_read[1].event.channel() == 10);
    REQUIRE(notes_read[1].event.data()[1] == 0x00);
    REQUIRE(notes_read[1].event.data()[2] == 0x40);
    REQUIRE(notes_read[2].event.is_note_off());
    REQUIRE(notes_read[2].event.note() == 36);
    REQUIRE(notes_read[2].event.velocity() == 12);

    // And every event kept its time across the round trip, on both tracks.
    REQUIRE(controls_read[0].time_seconds == Approx(0.00).margin(1e-6));
    REQUIRE(controls_read[1].time_seconds == Approx(0.25).margin(1e-6));
    REQUIRE(notes_read[0].time_seconds == Approx(0.50).margin(1e-6));
    REQUIRE(notes_read[1].time_seconds == Approx(0.75).margin(1e-6));
    REQUIRE(notes_read[2].time_seconds == Approx(1.00).margin(1e-6));
}

TEST_CASE("write_midi_file rejects missing parent directories",
          "[midi][file]") {
    TempDir tmp;
    MidiFileData data;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    data.tracks.push_back(std::move(track));

    const auto path = tmp.path / "missing" / "out.mid";
    REQUIRE_FALSE(write_midi_file(path.string(), data));
    REQUIRE_FALSE(fs::exists(path));
}

TEST_CASE("write_midi_file rejects directory destinations",
          "[midi][file]") {
    TempDir tmp;
    MidiFileData data;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    data.tracks.push_back(std::move(track));

    REQUIRE_FALSE(write_midi_file(tmp.path.string(), data));
}

#if defined(__linux__)
TEST_CASE("write_midi_file reports deferred flush failures on a full device",
          "[midi][file][reliability]") {
    // /dev/full opens and accepts small buffered writes but fails the
    // flush/close with ENOSPC. Before the explicit close()+good() check,
    // write_midi_file() returned the still-good() buffered stream state and
    // reported success even though the bytes never reached the device. Linux-
    // only mechanism (/dev/full); passes (no-op) where the device is absent.
    if (!fs::exists("/dev/full")) return;

    MidiFileData data;
    data.ticks_per_quarter = 480;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    track.events.push_back({0.25, MidiEvent::note_off(0, 60)});
    data.tracks.push_back(std::move(track));

    REQUIRE_FALSE(write_midi_file("/dev/full", data));
}
#endif

// ── The four defects reported against the writer ────────────────────────────

TEST_CASE("write_midi_file places ticks at the division it declares",
          "[midi][file][smf]") {
    // THE bug. The writer baked tick values at a fixed internal timebase and then
    // stamped the header with the caller's division, so the file declared one
    // timebase and was written in another. At 480 PPQN the two disagreed by
    // slightly over 2x, and every exported file played at roughly half speed.
    //
    // The old test suite missed this because it happened to use 960 PPQN — close
    // enough to the internal 1000 that the error fell inside a 0.05s margin. At
    // 480 it is unmissable, so this pins 480 specifically.
    TempDir tmp;
    const auto path = tmp.path / "ppqn-480.mid";

    MidiFileData data;
    data.ticks_per_quarter = 480;
    data.tempo_bpm = 120.0;

    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    track.events.push_back({1.0, MidiEvent::note_off(0, 60, 0)});   // one second in
    data.tracks.push_back(std::move(track));

    REQUIRE(write_midi_file(path.string(), data));

    const auto bytes = read_bytes(path);
    REQUIRE(bytes[12] == 0x01);          // division 480 == 0x01E0
    REQUIRE(bytes[13] == 0xE0);

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->ticks_per_quarter == 480);

    // At 120 BPM a quarter note is half a second, so one second is 960 ticks.
    // The event must come back at one second — not at 2.08, which is what a
    // 1000-tick bake read through a 480 division produces.
    const auto& events = read->tracks.front().events;
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].time_seconds == Approx(0.0).margin(1e-6));
    REQUIRE(events[1].time_seconds == Approx(1.0).margin(1e-6));
}

TEST_CASE("write_midi_file survives a round trip at every common division",
          "[midi][file][smf]") {
    // The bug was a function of the division, so sweep the divisions people
    // actually use. A writer that agrees with itself at one PPQN and not another
    // is the exact failure this replaces.
    for (const int ppqn : {96, 192, 240, 384, 480, 960}) {
        TempDir tmp;
        const auto path = tmp.path / "sweep.mid";

        MidiFileData data;
        data.ticks_per_quarter = ppqn;
        data.tempo_bpm = 140.0;

        MidiTrack track;
        track.events.push_back({0.0, MidiEvent::note_on(0, 36, 120)});
        track.events.push_back({0.5, MidiEvent::note_off(0, 36, 0)});
        track.events.push_back({1.25, MidiEvent::note_on(0, 38, 90)});
        data.tracks.push_back(std::move(track));

        REQUIRE(write_midi_file(path.string(), data));

        auto read = read_midi_file(path.string());
        REQUIRE(read.has_value());
        REQUIRE(read->ticks_per_quarter == ppqn);
        REQUIRE(read->tempo_bpm == Approx(140.0).margin(0.01));

        const auto& events = read->tracks.front().events;
        REQUIRE(events.size() == 3);

        // One tick of slack: a time that does not land exactly on a tick boundary
        // is quantized to the grid, and at 96 PPQN that grid is coarse.
        const double tick = (60.0 / 140.0) / static_cast<double>(ppqn);
        REQUIRE(events[0].time_seconds == Approx(0.0).margin(tick));
        REQUIRE(events[1].time_seconds == Approx(0.5).margin(tick));
        REQUIRE(events[2].time_seconds == Approx(1.25).margin(tick));
    }
}

TEST_CASE("write_midi_file emits the tempo as a meta event",
          "[midi][file][smf]") {
    // MidiFileData::tempo_bpm existed and the writer never looked at it, so every
    // file went out claiming nothing about its tempo and a reader had to guess.
    TempDir tmp;
    const auto path = tmp.path / "tempo.mid";

    MidiFileData data;
    data.ticks_per_quarter = 480;
    data.tempo_bpm = 128.0;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    data.tracks.push_back(std::move(track));

    REQUIRE(write_midi_file(path.string(), data));

    // FF 51 03 — a tempo meta must be physically present in the bytes.
    const auto bytes = read_bytes(path);
    bool found = false;
    for (size_t i = 0; i + 2 < bytes.size(); ++i)
        if (bytes[i] == 0xFF && bytes[i + 1] == 0x51 && bytes[i + 2] == 0x03) found = true;
    REQUIRE(found);

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->tempo_bpm == Approx(128.0).margin(0.01));
}

TEST_CASE("a tempo other than 120 actually changes where events land",
          "[midi][file][smf]") {
    // Guards against a writer that emits a tempo meta but keeps converting at a
    // hardcoded 120 BPM: the file would SAY 60 and be written as if it were 120.
    // The same note at the same second must land on a different tick.
    TempDir tmp;

    const auto write_at = [&](double bpm, const char* name) {
        MidiFileData data;
        data.ticks_per_quarter = 480;
        data.tempo_bpm = bpm;
        MidiTrack track;
        track.events.push_back({1.0, MidiEvent::note_on(0, 60, 100)});
        data.tracks.push_back(std::move(track));
        const auto path = tmp.path / name;
        REQUIRE(write_midi_file(path.string(), data));
        return read_bytes(path);
    };

    const auto slow = write_at(60.0, "slow.mid");
    const auto fast = write_at(240.0, "fast.mid");

    // At 60 BPM one second is 480 ticks; at 240 BPM it is 1920. The byte streams
    // must therefore differ, and not only in the tempo meta.
    REQUIRE(slow != fast);

    // And both must still read back at one second — the tempo and the ticks agree.
    auto slow_read = read_midi_file((tmp.path / "slow.mid").string());
    auto fast_read = read_midi_file((tmp.path / "fast.mid").string());
    REQUIRE(slow_read.has_value());
    REQUIRE(fast_read.has_value());
    REQUIRE(slow_read->tracks.front().events[0].time_seconds == Approx(1.0).margin(1e-6));
    REQUIRE(fast_read->tracks.front().events[0].time_seconds == Approx(1.0).margin(1e-6));
}

TEST_CASE("write_midi_file terminates every track with an end-of-track meta",
          "[midi][file][smf]") {
    // Required by the specification. Its absence is what makes a file technically
    // non-conformant, and some hosts refuse such a file outright.
    TempDir tmp;
    const auto path = tmp.path / "eot.mid";

    MidiFileData data;
    data.ticks_per_quarter = 480;
    MidiTrack a;
    a.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    MidiTrack b;
    b.events.push_back({0.0, MidiEvent::cc(1, 74, 64)});
    data.tracks.push_back(std::move(a));
    data.tracks.push_back(std::move(b));

    REQUIRE(write_midi_file(path.string(), data));

    // FF 2F 00, once per track.
    const auto bytes = read_bytes(path);
    int eot = 0;
    for (size_t i = 0; i + 2 < bytes.size(); ++i)
        if (bytes[i] == 0xFF && bytes[i + 1] == 0x2F && bytes[i + 2] == 0x00) ++eot;
    REQUIRE(eot == 2);
}

TEST_CASE("write_midi_file declares format 1 only when there is more than one track",
          "[midi][file][smf]") {
    TempDir tmp;

    MidiFileData one;
    one.ticks_per_quarter = 480;
    MidiTrack t;
    t.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    one.tracks.push_back(t);

    MidiFileData two = one;
    two.tracks.push_back(t);

    const auto single = tmp.path / "single.mid";
    const auto multi = tmp.path / "multi.mid";
    REQUIRE(write_midi_file(single.string(), one));
    REQUIRE(write_midi_file(multi.string(), two));

    const auto sb = read_bytes(single);
    const auto mb = read_bytes(multi);

    REQUIRE(sb[9] == 0x00);   // format 0
    REQUIRE(sb[11] == 0x01);  // one track
    REQUIRE(mb[9] == 0x01);   // format 1 — simultaneous tracks
    REQUIRE(mb[11] == 0x02);  // two of them
}

TEST_CASE("write_midi_file round-trips track names", "[midi][file][smf]") {
    TempDir tmp;
    const auto path = tmp.path / "names.mid";

    MidiFileData data;
    data.ticks_per_quarter = 480;
    MidiTrack drums;
    drums.name = "Drums";
    drums.events.push_back({0.0, MidiEvent::note_on(9, 36, 120)});
    MidiTrack bass;
    bass.name = "Bass";
    bass.events.push_back({0.0, MidiEvent::note_on(1, 28, 100)});
    data.tracks.push_back(std::move(drums));
    data.tracks.push_back(std::move(bass));

    REQUIRE(write_midi_file(path.string(), data));

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->tracks.size() == 2);
    REQUIRE(read->tracks[0].name == "Drums");
    REQUIRE(read->tracks[1].name == "Bass");
}

TEST_CASE("read_midi_file honours a tempo change partway through a file",
          "[midi][file][smf]") {
    // The previous backend supported a tempo map, so replacing it must not lose
    // that. 480 PPQN; 120 BPM for the first quarter, then 60 BPM.
    TempDir tmp;
    const auto path = tmp.path / "tempo-map.mid";

    write_bytes(path, {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06,
        0x00, 0x00, 0x00, 0x01, 0x01, 0xE0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x1C,
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,   // 500000us = 120 BPM
        0x00, 0x90, 0x3C, 0x40,                     // note on at tick 0
        0x83, 0x60, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,  // tick 480: 1000000us = 60 BPM
        0x83, 0x60, 0x80, 0x3C, 0x00,               // note off at tick 960
        0x00, 0xFF, 0x2F, 0x00,
    });

    auto read = read_midi_file(path.string());
    REQUIRE(read.has_value());
    REQUIRE(read->tempo_bpm == Approx(120.0).margin(0.01));   // the INITIAL tempo

    const auto& events = read->tracks.front().events;
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].time_seconds == Approx(0.0).margin(1e-6));
    // First 480 ticks at 120 BPM = 0.5s. Next 480 ticks at 60 BPM = 1.0s.
    // A reader that used a single rate would say 1.0 (all at 120) or 2.0 (all at
    // 60); the right answer is 1.5.
    REQUIRE(events[1].time_seconds == Approx(1.5).margin(1e-6));
}
