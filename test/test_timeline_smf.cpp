#include <catch2/catch_test_macros.hpp>

#include <pulp/midi/ump_conversion.hpp>
#include <pulp/timebase/rational_time.hpp>
#include <pulp/timebase/tick.hpp>
#include <pulp/timeline/smf.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace pulp::timeline;
namespace timebase = pulp::timebase;

namespace {

constexpr std::int64_t kQuarter = timebase::kTicksPerQuarter;

void append_variable_length(std::vector<std::uint8_t>& out, std::uint32_t value) {
    std::uint8_t groups[4] = {0, 0, 0, 0};
    std::size_t count = 0;
    do {
        groups[count++] = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
    } while (value != 0);
    while (count != 0) {
        --count;
        out.push_back(static_cast<std::uint8_t>(count != 0 ? (groups[count] | 0x80u)
                                                           : groups[count]));
    }
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24u));
    out.push_back(static_cast<std::uint8_t>(value >> 16u));
    out.push_back(static_cast<std::uint8_t>(value >> 8u));
    out.push_back(static_cast<std::uint8_t>(value));
}

std::vector<std::uint8_t> header_chunk(std::uint16_t format, std::uint16_t tracks,
                                       std::uint16_t division) {
    std::vector<std::uint8_t> bytes{'M', 'T', 'h', 'd'};
    append_u32(bytes, 6);
    append_u16(bytes, format);
    append_u16(bytes, tracks);
    append_u16(bytes, division);
    return bytes;
}

std::vector<std::uint8_t> track_chunk(const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> bytes{'M', 'T', 'r', 'k'};
    append_u32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

void append(std::vector<std::uint8_t>& out, const std::vector<std::uint8_t>& more) {
    out.insert(out.end(), more.begin(), more.end());
}

// One delta-timed event body fragment.
void append_event(std::vector<std::uint8_t>& out, std::uint32_t delta,
                  const std::vector<std::uint8_t>& message) {
    append_variable_length(out, delta);
    append(out, message);
}

std::vector<std::uint8_t> note_on(std::uint8_t channel, std::uint8_t pitch,
                                  std::uint8_t velocity) {
    return {static_cast<std::uint8_t>(0x90u | channel), pitch, velocity};
}

std::vector<std::uint8_t> note_off(std::uint8_t channel, std::uint8_t pitch,
                                   std::uint8_t velocity = 0x40u) {
    return {static_cast<std::uint8_t>(0x80u | channel), pitch, velocity};
}

std::vector<std::uint8_t> set_tempo(std::uint32_t microseconds_per_quarter) {
    return {0xffu, 0x51u, 0x03u, static_cast<std::uint8_t>(microseconds_per_quarter >> 16u),
            static_cast<std::uint8_t>(microseconds_per_quarter >> 8u),
            static_cast<std::uint8_t>(microseconds_per_quarter)};
}

std::vector<std::uint8_t> time_signature(std::uint8_t numerator,
                                         std::uint8_t denominator_power) {
    return {0xffu, 0x58u, 0x04u, numerator, denominator_power, 24u, 8u};
}

const std::vector<std::uint8_t> kEndOfTrack{0xffu, 0x2fu, 0x00u};

// A format-0 file whose single track is `body` followed by end-of-track.
std::vector<std::uint8_t> single_track_file(std::uint16_t division,
                                            std::vector<std::uint8_t> body) {
    append_event(body, 0, kEndOfTrack);
    auto bytes = header_chunk(0, 1, division);
    append(bytes, track_chunk(body));
    return bytes;
}

// A note as it appears on the timeline, in absolute canonical ticks.
struct AbsoluteNote {
    std::int64_t start = 0;
    std::int64_t duration = 0;
    std::uint16_t velocity = 0;
    std::uint8_t pitch = 0;
    std::uint8_t channel = 0;

    bool operator==(const AbsoluteNote&) const = default;
    bool operator<(const AbsoluteNote& other) const {
        return std::tie(start, pitch, channel, duration, velocity) <
               std::tie(other.start, other.pitch, other.channel, other.duration, other.velocity);
    }
};

std::vector<AbsoluteNote> absolute_notes(const Project& project) {
    std::vector<AbsoluteNote> result;
    const auto* sequence = project.find_sequence(project.root_sequence_id());
    REQUIRE(sequence != nullptr);
    for (const auto& track : sequence->tracks()) {
        for (const auto& clip : track.clips()) {
            const auto* notes = std::get_if<MidiContent>(&clip.content());
            if (notes == nullptr)
                continue;
            for (const auto& note : notes->notes()) {
                result.push_back(AbsoluteNote{clip.start().value + note.start.value,
                                              note.duration.value, note.velocity, note.pitch,
                                              note.channel});
            }
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

// Builds a single-track project whose notes are given in absolute canonical
// ticks, plus optional tempo and meter maps.
Project make_project(const std::vector<AbsoluteNote>& notes,
                     std::vector<timebase::TempoPoint> tempo_points = {},
                     std::vector<timebase::MeterPoint> meter_points = {}) {
    REQUIRE_FALSE(notes.empty());
    std::int64_t first = notes.front().start;
    std::int64_t last = 0;
    for (const auto& note : notes) {
        first = std::min(first, note.start);
        last = std::max(last, note.start + note.duration);
    }
    std::uint64_t next_id = 1;
    std::vector<NoteEvent> events;
    for (const auto& note : notes) {
        NoteEvent event{};
        event.id = ItemId{next_id++};
        event.start = timebase::TickPosition{note.start - first};
        event.duration = timebase::TickDuration{note.duration};
        event.velocity = note.velocity;
        event.pitch = note.pitch;
        event.channel = note.channel;
        events.push_back(event);
    }
    auto content = MidiContent::create(std::move(events));
    REQUIRE(content);
    auto clip = Clip::create(ItemId{next_id++}, timebase::TickPosition{first},
                             timebase::TickDuration{last - first}, std::move(content.value()));
    REQUIRE(clip);
    auto track = Track::create(ItemId{next_id++}, "Notes", {std::move(clip.value())});
    REQUIRE(track);
    const auto sequence_id = ItemId{next_id++};
    auto sequence =
        Sequence::create(sequence_id, "Arrangement", std::nullopt, {std::move(track.value())});
    REQUIRE(sequence);

    ProjectInput input{};
    input.id = ItemId{next_id++};
    input.next_item_id = next_id;
    input.root_sequence_id = sequence_id;
    input.sequences.push_back(std::move(sequence.value()));
    if (!tempo_points.empty()) {
        auto tempo_map = timebase::TempoMap::create(tempo_points);
        REQUIRE(tempo_map);
        input.tempo_map = std::move(tempo_map.value());
    }
    if (!meter_points.empty()) {
        auto meter_map = timebase::MeterMap::create(meter_points);
        REQUIRE(meter_map);
        input.meter_map = std::move(meter_map.value());
    }
    auto project = Project::create(std::move(input));
    REQUIRE(project);
    return std::move(project.value());
}

} // namespace

TEST_CASE("SMF import places notes on the canonical tick grid", "[timeline][smf][import]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    CHECK(imported.value().division == 960);
    CHECK(imported.value().exact_tick_conversion);
    CHECK(imported.value().max_tick_rounding_error == 0);

    const auto notes = absolute_notes(imported.value().project);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].start == 0);
    CHECK(notes[0].duration == kQuarter);
    CHECK(notes[0].pitch == 60);
    CHECK(notes[0].channel == 0);
    CHECK(notes[0].velocity == pulp::midi::scale_7_to_16(100));
}

TEST_CASE("SMF import treats a zero-velocity note on as a note off",
          "[timeline][smf][import]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(2, 64, 90));
    append_event(body, 480, note_on(2, 64, 0));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto notes = absolute_notes(imported.value().project);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].duration == kQuarter / 2);
    CHECK(notes[0].channel == 2);
}

TEST_CASE("SMF import decodes running status", "[timeline][smf][import][parser]") {
    // One 0x90 status byte followed by three bare data pairs: on, on, then the
    // zero-velocity pair that releases the first note.
    std::vector<std::uint8_t> body;
    append_event(body, 0, {0x90u, 60u, 100u});
    append_event(body, 240, {62u, 100u});
    append_event(body, 240, {60u, 0u});
    append_event(body, 240, {62u, 0u});
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto notes = absolute_notes(imported.value().project);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].pitch == 60);
    CHECK(notes[0].start == 0);
    CHECK(notes[0].duration == kQuarter / 2);
    CHECK(notes[1].pitch == 62);
    CHECK(notes[1].start == kQuarter / 4);
    CHECK(notes[1].duration == kQuarter / 2);
}

TEST_CASE("SMF import rejects a data byte with no running status",
          "[timeline][smf][import][parser]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, {60u, 100u});
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE_FALSE(imported);
    CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
}

TEST_CASE("SMF import clears running status after a meta event",
          "[timeline][smf][import][parser]") {
    // A meta event cancels running status, so the following bare data pair is
    // malformed rather than a repeat of the preceding note on.
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 0, set_tempo(500'000));
    append_event(body, 240, {60u, 0u});
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE_FALSE(imported);
    CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
}

TEST_CASE("SMF import honours a system-exclusive length rather than scanning for a status byte",
          "[timeline][smf][import][parser]") {
    // The payload contains bytes that look like a note on. A reader that scans
    // for the next high-bit byte instead of honouring the declared length would
    // resynchronise inside the payload and invent events.
    std::vector<std::uint8_t> payload{0x90u, 0x3cu, 0x64u, 0x00u, 0xf7u};
    std::vector<std::uint8_t> sysex{0xf0u};
    append_variable_length(sysex, static_cast<std::uint32_t>(payload.size()));
    sysex.insert(sysex.end(), payload.begin(), payload.end());

    std::vector<std::uint8_t> body;
    append_event(body, 0, sysex);
    append_event(body, 0, note_on(0, 70, 80));
    append_event(body, 960, note_off(0, 70));
    const auto file = single_track_file(960, body);

    SmfImportOptions options;
    options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
    auto imported = import_smf(file, options);
    REQUIRE(imported);
    const auto notes = absolute_notes(imported.value().project);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].pitch == 70);

    // The default policy refuses the file outright rather than dropping it.
    auto rejected = import_smf(file);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == SmfErrorCode::UnsupportedFeature);
}

TEST_CASE("SMF import turns tempo meta events into tempo-map points",
          "[timeline][smf][import][tempo]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, set_tempo(500'000));  // 120 bpm
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, set_tempo(400'000)); // 150 bpm
    append_event(body, 0, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto points = imported.value().project.tempo_map().points();
    REQUIRE(points.size() == 2);
    CHECK(points[0].tick.value == 0);
    CHECK(points[0].bpm == 120.0);
    CHECK(points[1].tick.value == kQuarter);
    CHECK(points[1].bpm == 150.0);
}

TEST_CASE("SMF import defaults tempo and meter when the file declares none",
          "[timeline][smf][import][tempo]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto tempo = imported.value().project.tempo_map().points();
    REQUIRE(tempo.size() == 1);
    CHECK(tempo[0].bpm == 120.0);
    const auto meter = imported.value().project.meter_map().points();
    REQUIRE(meter.size() == 1);
    CHECK(meter[0].signature.numerator == 4);
    CHECK(meter[0].signature.denominator == 4);
}

TEST_CASE("SMF import turns time-signature meta events into meter-map points",
          "[timeline][smf][import][meter]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, time_signature(3, 2));      // 3/4
    append_event(body, 2880, time_signature(7, 3));   // 7/8 one bar later
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 480, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto points = imported.value().project.meter_map().points();
    REQUIRE(points.size() == 2);
    CHECK(points[0].signature.numerator == 3);
    CHECK(points[0].signature.denominator == 4);
    CHECK(points[1].tick.value == 3 * kQuarter);
    CHECK(points[1].signature.numerator == 7);
    CHECK(points[1].signature.denominator == 8);
}

TEST_CASE("SMF import rejects a time-signature change off a bar boundary",
          "[timeline][smf][import][meter]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, time_signature(4, 2));
    append_event(body, 960, time_signature(3, 2)); // one quarter into a 4/4 bar
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 480, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto imported = import_smf(file);
    REQUIRE_FALSE(imported);
    CHECK(imported.error().code == SmfErrorCode::MeterMapRejected);
}

TEST_CASE("SMF import merges a format-1 conductor track with note tracks",
          "[timeline][smf][import][parser]") {
    std::vector<std::uint8_t> conductor;
    append_event(conductor, 0, set_tempo(500'000));
    append_event(conductor, 1920, set_tempo(250'000)); // 240 bpm at bar 2
    append_event(conductor, 0, kEndOfTrack);

    std::vector<std::uint8_t> notes;
    append_event(notes, 0, {0xffu, 0x03u, 0x04u, 'L', 'e', 'a', 'd'});
    append_event(notes, 0, note_on(1, 67, 110));
    append_event(notes, 1920, note_off(1, 67));
    append_event(notes, 0, kEndOfTrack);

    auto file = header_chunk(1, 2, 480);
    append(file, track_chunk(conductor));
    append(file, track_chunk(notes));

    auto imported = import_smf(file);
    REQUIRE(imported);
    const auto tempo = imported.value().project.tempo_map().points();
    REQUIRE(tempo.size() == 2);
    CHECK(tempo[1].tick.value == 4 * kQuarter);
    CHECK(tempo[1].bpm == 240.0);

    const auto* sequence =
        imported.value().project.find_sequence(imported.value().project.root_sequence_id());
    REQUIRE(sequence != nullptr);
    // The conductor chunk has neither notes nor a name, so it contributes its
    // tempo events to the map and no track of its own.
    REQUIRE(sequence->tracks().size() == 1);
    CHECK(sequence->tracks()[0].name() == "Lead");

    const auto note_list = absolute_notes(imported.value().project);
    REQUIRE(note_list.size() == 1);
    CHECK(note_list[0].pitch == 67);
    CHECK(note_list[0].duration == 4 * kQuarter);
}

TEST_CASE("SMF import rejects malformed framing", "[timeline][smf][import][parser]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));

    SECTION("no MThd chunk") {
        std::vector<std::uint8_t> file{'R', 'I', 'F', 'F', 0, 0, 0, 0};
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MissingHeader);
    }
    SECTION("truncated track chunk") {
        auto file = single_track_file(960, body);
        file.resize(file.size() - 3);
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("missing end-of-track event") {
        auto file = header_chunk(0, 1, 960);
        append(file, track_chunk(body));
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("trailing bytes after the last track") {
        auto file = single_track_file(960, body);
        file.push_back(0x00u);
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("SMPTE division") {
        auto file = single_track_file(0xe728u, body);
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnsupportedDivision);
    }
    SECTION("format 2") {
        auto file = header_chunk(2, 1, 960);
        auto with_track = body;
        append_event(with_track, 0, kEndOfTrack);
        append(file, track_chunk(with_track));
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnsupportedFormat);
    }
    SECTION("non-MTrk chunk where a track is declared") {
        auto file = header_chunk(0, 1, 960);
        append(file, {'X', 'Y', 'Z', 'W', 0, 0, 0, 0});
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnsupportedFeature);
    }
}

TEST_CASE("SMF import rejects unbalanced and empty notes", "[timeline][smf][import]") {
    SECTION("note on with no note off") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, note_on(0, 60, 100));
        auto imported = import_smf(single_track_file(960, body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnbalancedNote);
    }
    SECTION("note off with no note on") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, note_off(0, 60));
        auto imported = import_smf(single_track_file(960, body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnbalancedNote);
    }
    SECTION("zero-length note") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, note_on(0, 60, 100));
        append_event(body, 0, note_off(0, 60));
        auto imported = import_smf(single_track_file(960, body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidValue);
    }
}

TEST_CASE("SMF import rejects out-of-subset events unless the caller opts in",
          "[timeline][smf][import]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, {0xb0u, 0x07u, 0x64u}); // control change
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));
    const auto file = single_track_file(960, body);

    auto rejected = import_smf(file);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == SmfErrorCode::UnsupportedFeature);

    SmfImportOptions options;
    options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
    auto accepted = import_smf(file, options);
    REQUIRE(accepted);
    CHECK(absolute_notes(accepted.value().project).size() == 1);
}

TEST_CASE("SMF import enforces resource limits before growing state",
          "[timeline][smf][import][limits]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 480, note_on(0, 62, 100));
    append_event(body, 480, note_off(0, 60));
    append_event(body, 480, note_off(0, 62));
    const auto file = single_track_file(960, body);

    SECTION("file bytes") {
        SmfImportOptions options;
        options.limits.max_file_bytes = file.size() - 1;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("track count") {
        SmfImportOptions options;
        options.limits.max_tracks = 0;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("event count") {
        SmfImportOptions options;
        options.limits.max_events = 3;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("note count") {
        SmfImportOptions options;
        options.limits.max_notes = 1;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("concurrent notes") {
        SmfImportOptions options;
        options.limits.max_concurrent_notes = 1;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("meta payload bytes") {
        SmfImportOptions options;
        options.limits.max_payload_bytes = 2;
        auto imported = import_smf(file, options);
        REQUIRE(imported); // note events carry no meta payload
        options.limits.max_payload_bytes = 0;
        auto with_tempo = single_track_file(960, [] {
            std::vector<std::uint8_t> tempo_body;
            append_event(tempo_body, 0, set_tempo(500'000));
            append_event(tempo_body, 0, note_on(0, 60, 100));
            append_event(tempo_body, 960, note_off(0, 60));
            return tempo_body;
        }());
        auto rejected = import_smf(with_tempo, options);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("absolute tick ceiling") {
        SmfImportOptions options;
        options.limits.max_smf_ticks = 100;
        auto imported = import_smf(file, options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
}

TEST_CASE("SMF import reports rounding for a division that does not divide the canonical grid",
          "[timeline][smf][import][tempo]") {
    // 1920 is 2^7 * 3 * 5; kTicksPerQuarter is 2^6 * 3^2 * 5^2 * 7^2, so
    // 1920 does not divide it and every odd SMF tick rounds.
    static_assert(kQuarter % 1920 != 0);
    std::vector<std::uint8_t> body;
    append_event(body, 1, note_on(0, 60, 100));
    append_event(body, 1920, note_off(0, 60));
    const auto file = single_track_file(1920, body);

    auto imported = import_smf(file);
    REQUIRE(imported);
    CHECK_FALSE(imported.value().exact_tick_conversion);
    CHECK(imported.value().max_tick_rounding_error == 1);

    const auto notes = absolute_notes(imported.value().project);
    REQUIRE(notes.size() == 1);
    // 1 * 705600 / 1920 = 367.5, rounded away from zero.
    CHECK(notes[0].start == 368);
    CHECK(notes[0].duration == kQuarter);
}

TEST_CASE("SMF export writes note pairs and a conductor track",
          "[timeline][smf][export]") {
    const auto project = make_project(
        {AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}},
        {{timebase::TickPosition{0}, 140.0, timebase::TempoCurve::Constant}},
        {{timebase::TickPosition{0}, timebase::MeterSignature{3, 4}}});

    auto exported = export_smf(project);
    REQUIRE(exported);
    CHECK(exported.value().exact_tick_conversion);
    CHECK(exported.value().max_tick_rounding_error == 0);

    const auto& bytes = exported.value().bytes;
    REQUIRE(bytes.size() > 14);
    CHECK(bytes[0] == 'M');
    CHECK(bytes[9] == 1);  // format 1
    CHECK(bytes[11] == 2); // conductor plus one note track
    CHECK(bytes[12] == 0x03u);
    CHECK(bytes[13] == 0xc0u); // division 960

    // The exported bytes must survive its own reader unchanged.
    auto reimported = import_smf(bytes);
    REQUIRE(reimported);
    const auto tempo = reimported.value().project.tempo_map().points();
    REQUIRE(tempo.size() == 1);
    // A set-tempo event stores whole microseconds per quarter note. Rounding
    // the period by up to half a microsecond moves the tempo by at most
    // bpm^2 / 120e6 — about 1.6e-4 bpm here.
    CHECK(std::abs(tempo[0].bpm - 140.0) <= 140.0 * 140.0 / 120'000'000.0);
    const auto meter = reimported.value().project.meter_map().points();
    REQUIRE(meter.size() == 1);
    CHECK(meter[0].signature.numerator == 3);
    CHECK(meter[0].signature.denominator == 4);
}

TEST_CASE("SMF export rejects positions the requested division cannot represent",
          "[timeline][smf][export]") {
    // kTicksPerQuarter / 960 is 735, so a start of 1 is off the 960 grid.
    const auto project =
        make_project({AbsoluteNote{1, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}});

    auto rejected = export_smf(project);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == SmfErrorCode::InexactTickConversion);

    SmfExportOptions options;
    options.allow_lossy_tick_rounding = true;
    auto rounded = export_smf(project, options);
    REQUIRE(rounded);
    CHECK_FALSE(rounded.value().exact_tick_conversion);
    // Half a division step: 735 / 2 rounded down.
    CHECK(rounded.value().max_tick_rounding_error <= kQuarter / (2 * 960) + 1);
}

TEST_CASE("SMF export rejects values with no MIDI representation",
          "[timeline][smf][export]") {
    SECTION("velocity that scales to zero") {
        const auto project = make_project({AbsoluteNote{0, kQuarter, 0, 60, 0}});
        auto exported = export_smf(project);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("tempo ramp") {
        const auto project = make_project(
            {AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}},
            {{timebase::TickPosition{0}, 120.0, timebase::TempoCurve::LinearInTicks},
             {timebase::TickPosition{kQuarter * 4}, 140.0, timebase::TempoCurve::Constant}});
        auto exported = export_smf(project);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::UnsupportedFeature);
    }
    SECTION("tempo below the set-tempo range") {
        const auto project = make_project(
            {AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}},
            {{timebase::TickPosition{0}, 2.0, timebase::TempoCurve::Constant}});
        auto exported = export_smf(project);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("absolute-anchored clip") {
        auto content = MidiContent::create({NoteEvent{ItemId{1}, timebase::TickPosition{0},
                                                      timebase::TickDuration{kQuarter},
                                                      0xffffu, 60, 0}});
        REQUIRE(content);
        auto clip = Clip::create_absolute(ItemId{2}, timebase::SamplePosition{0}, 48'000,
                                          timebase::RationalRate{48'000, 1},
                                          std::move(content.value()));
        REQUIRE(clip);
        auto track = Track::create(ItemId{3}, "Absolute", {std::move(clip.value())});
        REQUIRE(track);
        auto sequence =
            Sequence::create(ItemId{4}, "Arrangement", std::nullopt, {std::move(track.value())});
        REQUIRE(sequence);
        ProjectInput input{};
        input.id = ItemId{5};
        input.next_item_id = 6;
        input.root_sequence_id = ItemId{4};
        input.sequences.push_back(std::move(sequence.value()));
        auto project = Project::create(std::move(input));
        REQUIRE(project);
        auto exported = export_smf(project.value());
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::UnsupportedFeature);
    }
    SECTION("per-note playback modifiers") {
        for (const bool seed_only : {false, true}) {
            NoteEvent note{ItemId{1}, timebase::TickPosition{0},
                           timebase::TickDuration{kQuarter}, 0xffffu, 60, 0};
            std::vector<NoteModifier> modifiers;
            if (!seed_only) {
                NoteModifier modifier;
                modifier.note_id = note.id;
                modifier.probability = 0;
                modifiers.push_back(modifier);
            }
            auto content =
                MidiContent::create({note}, std::move(modifiers), seed_only ? 42 : 0);
            REQUIRE(content);
            auto clip = Clip::create(ItemId{2}, timebase::TickPosition{0},
                                     timebase::TickDuration{kQuarter},
                                     std::move(content.value()));
            REQUIRE(clip);
            auto track = Track::create(ItemId{3}, "Modified", {std::move(clip.value())});
            REQUIRE(track);
            auto sequence = Sequence::create(ItemId{4}, "Arrangement", std::nullopt,
                                             {std::move(track.value())});
            REQUIRE(sequence);
            ProjectInput input{};
            input.id = ItemId{5};
            input.next_item_id = 6;
            input.root_sequence_id = ItemId{4};
            input.sequences.push_back(std::move(sequence.value()));
            auto project = Project::create(std::move(input));
            REQUIRE(project);

            auto exported = export_smf(project.value());
            REQUIRE_FALSE(exported);
            CHECK(exported.error().code == SmfErrorCode::UnsupportedFeature);
            CHECK(exported.error().message.find("per-note playback modifiers") !=
                  std::string::npos);
        }
    }
}

TEST_CASE("SMF round trip preserves grid-aligned notes exactly",
          "[timeline][smf][roundtrip]") {
    constexpr std::int64_t kStep = kQuarter / 960;
    static_assert(kQuarter % 960 == 0);
    const std::vector<AbsoluteNote> notes{
        AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0},
        AbsoluteNote{kStep * 480, kQuarter * 2, pulp::midi::scale_7_to_16(1), 67, 3},
        AbsoluteNote{kQuarter * 4, kStep * 7, pulp::midi::scale_7_to_16(127), 36, 9},
        // Two identical pitches stacked on one channel exercise first-in
        // first-out note matching on the way back in.
        AbsoluteNote{kQuarter * 8, kQuarter, pulp::midi::scale_7_to_16(64), 48, 0},
        AbsoluteNote{kQuarter * 8 + kStep, kQuarter * 3, pulp::midi::scale_7_to_16(64), 48, 0},
    };
    const auto project = make_project(
        notes, {{timebase::TickPosition{0}, 96.0, timebase::TempoCurve::Constant},
                {timebase::TickPosition{kQuarter * 4}, 132.5, timebase::TempoCurve::Constant}},
        {{timebase::TickPosition{0}, timebase::MeterSignature{5, 8}}});

    auto exported = export_smf(project);
    REQUIRE(exported);
    CHECK(exported.value().exact_tick_conversion);
    CHECK(exported.value().max_tick_rounding_error == 0);

    auto reimported = import_smf(exported.value().bytes);
    REQUIRE(reimported);
    CHECK(reimported.value().exact_tick_conversion);

    auto expected = absolute_notes(project);
    auto actual = absolute_notes(reimported.value().project);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(actual[index].start == expected[index].start);
        CHECK(actual[index].duration == expected[index].duration);
        CHECK(actual[index].pitch == expected[index].pitch);
        CHECK(actual[index].channel == expected[index].channel);
        CHECK(actual[index].velocity == expected[index].velocity);
    }

    const auto tempo = reimported.value().project.tempo_map().points();
    REQUIRE(tempo.size() == 2);
    CHECK(tempo[0].bpm == 96.0);
    CHECK(tempo[1].tick.value == kQuarter * 4);
    // 60e6 / 132.5 is not a whole number of microseconds, so the tempo returns
    // within the set-tempo event's resolution: bpm^2 / 120e6.
    CHECK(std::abs(tempo[1].bpm - 132.5) <= 132.5 * 132.5 / 120'000'000.0);
    const auto meter = reimported.value().project.meter_map().points();
    REQUIRE(meter.size() == 1);
    CHECK(meter[0].signature.numerator == 5);
    CHECK(meter[0].signature.denominator == 8);
}

TEST_CASE("SMF round trip bounds off-grid note error by half a division step",
          "[timeline][smf][roundtrip]") {
    constexpr std::int64_t kStep = kQuarter / 960;
    constexpr std::int64_t kBound = kStep / 2 + 1;
    const std::vector<AbsoluteNote> notes{
        AbsoluteNote{1, kQuarter + 1, pulp::midi::scale_7_to_16(100), 60, 0},
        AbsoluteNote{kStep * 3 + 367, kQuarter, pulp::midi::scale_7_to_16(90), 64, 1},
    };
    const auto project = make_project(notes);

    SmfExportOptions options;
    options.allow_lossy_tick_rounding = true;
    auto exported = export_smf(project, options);
    REQUIRE(exported);
    CHECK_FALSE(exported.value().exact_tick_conversion);
    CHECK(exported.value().max_tick_rounding_error <= kBound);

    auto reimported = import_smf(exported.value().bytes);
    REQUIRE(reimported);
    auto expected = absolute_notes(project);
    auto actual = absolute_notes(reimported.value().project);
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        CHECK(std::abs(actual[index].start - expected[index].start) <= kBound);
        // Start and end round independently, so a duration can shift by twice
        // the per-position bound.
        CHECK(std::abs(actual[index].duration - expected[index].duration) <= 2 * kBound);
        CHECK(actual[index].pitch == expected[index].pitch);
        CHECK(actual[index].channel == expected[index].channel);
        CHECK(actual[index].velocity == expected[index].velocity);
    }
}

TEST_CASE("SMF velocity scaling matches the shared MIDI 2.0 conversion",
          "[timeline][smf][roundtrip]") {
    // The interop module keeps its own copy of the 7/16-bit scaling because it
    // cannot include the CHOC-backed pulp::midi headers under -fno-exceptions.
    // Round-tripping every 7-bit velocity through an SMF proves the two agree.
    for (std::uint8_t velocity = 1; velocity < 128; ++velocity) {
        std::vector<std::uint8_t> body;
        append_event(body, 0, note_on(0, 60, velocity));
        append_event(body, 960, note_off(0, 60));
        auto imported = import_smf(single_track_file(960, body));
        REQUIRE(imported);
        const auto notes = absolute_notes(imported.value().project);
        REQUIRE(notes.size() == 1);
        REQUIRE(notes[0].velocity == pulp::midi::scale_7_to_16(velocity));

        auto exported = export_smf(imported.value().project);
        REQUIRE(exported);
        auto reimported = import_smf(exported.value().bytes);
        REQUIRE(reimported);
        const auto round_tripped = absolute_notes(reimported.value().project);
        REQUIRE(round_tripped.size() == 1);
        REQUIRE(round_tripped[0].velocity == notes[0].velocity);
    }
}

TEST_CASE("SMF round trip keeps the track set stable across repeated passes",
          "[timeline][smf][roundtrip]") {
    // Export always writes a conductor chunk. If import rebuilt a track for it,
    // every pass would add one; a named empty track must still survive.
    auto project = make_project(
        {AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}},
        {{timebase::TickPosition{0}, 120.0, timebase::TempoCurve::Constant}});

    std::vector<std::size_t> track_counts;
    for (int pass = 0; pass < 3; ++pass) {
        auto exported = export_smf(project);
        REQUIRE(exported);
        auto reimported = import_smf(exported.value().bytes);
        REQUIRE(reimported);
        project = std::move(reimported.value().project);
        const auto* sequence = project.find_sequence(project.root_sequence_id());
        REQUIRE(sequence != nullptr);
        track_counts.push_back(sequence->tracks().size());
        REQUIRE(absolute_notes(project).size() == 1);
    }
    CHECK(track_counts == std::vector<std::size_t>{1, 1, 1});
    const auto* sequence = project.find_sequence(project.root_sequence_id());
    REQUIRE(sequence != nullptr);
    CHECK(sequence->tracks()[0].name() == "Notes");
}

TEST_CASE("SMF import bounds out-of-subset events it is told to ignore",
          "[timeline][smf][import][limits]") {
    // Ignored events must still count against max_events, or an opt-in caller
    // could be walked through an unbounded flood of control changes.
    std::vector<std::uint8_t> body;
    for (int index = 0; index < 8; ++index)
        append_event(body, 0, {0xb0u, 0x07u, 0x64u});
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));
    const auto file = single_track_file(960, body);

    SmfImportOptions options;
    options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
    auto accepted = import_smf(file, options);
    REQUIRE(accepted);

    options.limits.max_events = 4;
    auto rejected = import_smf(file, options);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == SmfErrorCode::LimitExceeded);
}

TEST_CASE("SMF import rejects malformed headers", "[timeline][smf][import][parser]") {
    std::vector<std::uint8_t> body;
    append_event(body, 0, note_on(0, 60, 100));
    append_event(body, 960, note_off(0, 60));
    append_event(body, 0, kEndOfTrack);

    SECTION("MThd length below the required payload") {
        std::vector<std::uint8_t> file{'M', 'T', 'h', 'd'};
        append_u32(file, 4);
        append_u16(file, 0);
        append_u16(file, 1);
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidHeader);
    }
    SECTION("MThd length field truncated") {
        std::vector<std::uint8_t> file{'M', 'T', 'h', 'd', 0, 0};
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("MThd payload truncated") {
        std::vector<std::uint8_t> file{'M', 'T', 'h', 'd'};
        append_u32(file, 6);
        append_u16(file, 0);
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("data too short for a chunk type") {
        std::vector<std::uint8_t> file{'M', 'T'};
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MissingHeader);
    }
    SECTION("zero division") {
        auto file = header_chunk(0, 1, 0);
        append(file, track_chunk(body));
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::UnsupportedDivision);
    }
    SECTION("format 0 declaring more than one track") {
        auto file = header_chunk(0, 2, 960);
        append(file, track_chunk(body));
        append(file, track_chunk(body));
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidHeader);
    }
    SECTION("fewer track chunks than declared") {
        auto file = header_chunk(1, 2, 960);
        append(file, track_chunk(body));
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("MTrk length field truncated") {
        auto file = header_chunk(1, 1, 960);
        append(file, {'M', 'T', 'r', 'k', 0, 0});
        auto imported = import_smf(file);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
}

TEST_CASE("SMF import rejects malformed events", "[timeline][smf][import][parser]") {
    auto file_from = [](std::vector<std::uint8_t> body) {
        auto file = header_chunk(0, 1, 960);
        append(file, track_chunk(body));
        return file;
    };

    SECTION("meta event type byte missing") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("meta length is a five-group variable-length quantity") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x51u, 0x80u, 0x80u, 0x80u, 0x80u, 0x00u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("meta payload truncated") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x51u, 0x03u, 0x07u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("set-tempo length is not three") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x51u, 0x02u, 0x07u, 0xa1u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("time-signature length is not four") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x58u, 0x02u, 0x04u, 0x02u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("end-of-track carries a payload") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x2fu, 0x01u, 0x00u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("two track-name events") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x03u, 0x01u, 'A'});
        append_event(body, 0, {0xffu, 0x03u, 0x01u, 'B'});
        append_event(body, 0, kEndOfTrack);
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("track name over the caller's limit") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xffu, 0x03u, 0x02u, 'A', 'B'});
        append_event(body, 0, kEndOfTrack);
        SmfImportOptions options;
        options.limits.max_track_name_bytes = 1;
        auto imported = import_smf(file_from(body), options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("events after end-of-track") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, kEndOfTrack);
        append_event(body, 0, note_on(0, 60, 100));
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("system-common status inside a track") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xf1u, 0x00u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("status byte where a data byte belongs") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0x90u, 0x3cu, 0x90u});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("channel message truncated") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0x90u, 0x3cu});
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("delta time is a five-group variable-length quantity") {
        std::vector<std::uint8_t> body{0x80u, 0x80u, 0x80u, 0x80u, 0x00u};
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("system-exclusive length malformed") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xf0u, 0x80u, 0x80u, 0x80u, 0x80u, 0x00u});
        SmfImportOptions options;
        options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
        auto imported = import_smf(file_from(body), options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::MalformedEvent);
    }
    SECTION("system-exclusive payload truncated") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xf0u, 0x04u, 0x01u});
        SmfImportOptions options;
        options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
        auto imported = import_smf(file_from(body), options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::Truncated);
    }
    SECTION("system-exclusive payload over the caller's limit") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, {0xf0u, 0x02u, 0x01u, 0xf7u});
        SmfImportOptions options;
        options.unsupported_events = SmfUnsupportedEventPolicy::IgnoreNonNote;
        options.limits.max_payload_bytes = 1;
        auto imported = import_smf(file_from(body), options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);
    }
    SECTION("zero microseconds per quarter note") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, set_tempo(0));
        append_event(body, 0, kEndOfTrack);
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("tempo above the representable range") {
        // 1 microsecond per quarter note is 60,000,000 bpm.
        std::vector<std::uint8_t> body;
        append_event(body, 0, set_tempo(1));
        append_event(body, 0, kEndOfTrack);
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("time signature with a zero numerator") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, time_signature(0, 2));
        append_event(body, 0, kEndOfTrack);
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("time signature denominator beyond a representable power of two") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, time_signature(4, 9));
        append_event(body, 0, kEndOfTrack);
        auto imported = import_smf(file_from(body));
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("tempo and meter point limits") {
        std::vector<std::uint8_t> body;
        append_event(body, 0, set_tempo(500'000));
        append_event(body, 960, set_tempo(400'000));
        append_event(body, 0, kEndOfTrack);
        SmfImportOptions options;
        options.limits.max_tempo_points = 1;
        auto imported = import_smf(file_from(body), options);
        REQUIRE_FALSE(imported);
        CHECK(imported.error().code == SmfErrorCode::LimitExceeded);

        std::vector<std::uint8_t> meter_body;
        append_event(meter_body, 0, time_signature(4, 2));
        append_event(meter_body, 3840, time_signature(3, 2));
        append_event(meter_body, 0, kEndOfTrack);
        SmfImportOptions meter_options;
        meter_options.limits.max_meter_points = 1;
        auto meter = import_smf(file_from(meter_body), meter_options);
        REQUIRE_FALSE(meter);
        CHECK(meter.error().code == SmfErrorCode::LimitExceeded);
    }
}

TEST_CASE("SMF export rejects unexportable project shapes", "[timeline][smf][export]") {
    const std::vector<AbsoluteNote> notes{
        AbsoluteNote{0, kQuarter, pulp::midi::scale_7_to_16(100), 60, 0}};

    SECTION("division outside the metrical range") {
        const auto project = make_project(notes);
        SmfExportOptions options;
        options.ticks_per_quarter = 0;
        auto exported = export_smf(project, options);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::UnsupportedDivision);
    }
    SECTION("time-signature numerator beyond the meta-event field") {
        const auto project = make_project(
            notes, {}, {{timebase::TickPosition{0}, timebase::MeterSignature{300, 4}}});
        auto exported = export_smf(project);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::InvalidValue);
    }
    SECTION("emitted events over the caller's ceiling") {
        const auto project = make_project(notes);
        SmfExportOptions options;
        options.max_events = 1;
        auto exported = export_smf(project, options);
        REQUIRE_FALSE(exported);
        CHECK(exported.error().code == SmfErrorCode::LimitExceeded);
    }
}

TEST_CASE("SMF export handles non-note clip content by policy", "[timeline][smf][export]") {
    // An empty clip contributes nothing; a media clip is an error unless the
    // caller explicitly asks for it to be skipped.
    auto empty_clip = Clip::create(ItemId{1}, timebase::TickPosition{0},
                                   timebase::TickDuration{kQuarter}, EmptyContent{});
    REQUIRE(empty_clip);
    auto media_clip =
        Clip::create(ItemId{2}, timebase::TickPosition{kQuarter * 2},
                     timebase::TickDuration{kQuarter},
                     MediaRef{ItemId{9}, timebase::SamplePosition{0}, 48'000});
    REQUIRE(media_clip);
    auto track = Track::create(ItemId{3}, "Audio",
                               {std::move(empty_clip.value()), std::move(media_clip.value())});
    REQUIRE(track);
    auto sequence =
        Sequence::create(ItemId{4}, "Arrangement", std::nullopt, {std::move(track.value())});
    REQUIRE(sequence);

    MediaAsset asset{};
    asset.id = ItemId{9};
    asset.name = "take";
    asset.frame_count = 48'000;
    asset.sample_rate = timebase::RationalRate{48'000, 1};
    const auto hash = ContentHash::from_hex(std::string(64, 'a'));
    REQUIRE(hash);
    asset.content_hash = *hash;

    ProjectInput input{};
    input.id = ItemId{5};
    input.next_item_id = 10;
    input.root_sequence_id = ItemId{4};
    input.assets.push_back(std::move(asset));
    input.sequences.push_back(std::move(sequence.value()));
    auto project = Project::create(std::move(input));
    REQUIRE(project);

    auto rejected = export_smf(project.value());
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == SmfErrorCode::UnsupportedFeature);

    SmfExportOptions options;
    options.skip_non_note_clips = true;
    auto skipped = export_smf(project.value(), options);
    REQUIRE(skipped);
    // The named track survives as an MTrk carrying only its name.
    auto reimported = import_smf(skipped.value().bytes);
    REQUIRE(reimported);
    const auto* root = reimported.value().project.find_sequence(
        reimported.value().project.root_sequence_id());
    REQUIRE(root != nullptr);
    REQUIRE(root->tracks().size() == 1);
    CHECK(root->tracks()[0].name() == "Audio");
    CHECK(root->tracks()[0].clips().empty());
}
