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
          "[midi][file][coverage][phase3]") {
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

TEST_CASE("write_midi_file rejects missing parent directories",
          "[midi][file][coverage][phase3]") {
    TempDir tmp;
    MidiFileData data;
    MidiTrack track;
    track.events.push_back({0.0, MidiEvent::note_on(0, 60, 100)});
    data.tracks.push_back(std::move(track));

    const auto path = tmp.path / "missing" / "out.mid";
    REQUIRE_FALSE(write_midi_file(path.string(), data));
    REQUIRE_FALSE(fs::exists(path));
}
