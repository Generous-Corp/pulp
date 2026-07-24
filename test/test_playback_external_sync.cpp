#include <pulp/playback/external_sync.hpp>
#include <pulp/playback/transport.hpp>

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <tuple>
#include <vector>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map(double bpm = 120.0, RationalRate sample_rate = {48'000, 1}) {
    const std::array points{TempoPoint{{0}, bpm}};
    return require_compiled_tempo_map(points, sample_rate);
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

midi::MidiEvent quarter_frame(std::uint8_t piece, std::uint8_t nibble) {
    return {choc::midi::ShortMessage(0xf1, static_cast<std::uint8_t>((piece << 4) | nibble), 0), 0,
            0.0};
}

std::vector<std::uint8_t> statuses(const midi::MidiBuffer& buffer) {
    std::vector<std::uint8_t> result;
    for (const auto& event : buffer)
        result.push_back(event.data()[0]);
    return result;
}

} // namespace

TEST_CASE("MTC conversion covers every SMPTE rate and drop-frame boundary",
          "[playback][external-sync]") {
    constexpr RationalRate sample_rate{48'000, 1};
    for (const auto timecode : std::array{
             MtcTimecode{1, 2, 3, 4, MtcFrameRate::Fps24},
             MtcTimecode{1, 2, 3, 4, MtcFrameRate::Fps25},
             MtcTimecode{1, 2, 3, 4, MtcFrameRate::Fps30},
             MtcTimecode{0, 10, 0, 0, MtcFrameRate::Fps2997Drop},
             MtcTimecode{1, 0, 0, 0, MtcFrameRate::Fps2997Drop},
         }) {
        REQUIRE(valid_mtc_timecode(timecode));
        const auto round_trip = samples_to_mtc_timecode(
            mtc_timecode_to_samples(timecode, sample_rate), sample_rate, timecode.frame_rate);
        CAPTURE(timecode.hours, timecode.minutes, timecode.seconds, timecode.frames,
                static_cast<int>(timecode.frame_rate), round_trip.hours, round_trip.minutes,
                round_trip.seconds, round_trip.frames);
        REQUIRE(round_trip == timecode);
    }

    REQUIRE_FALSE(valid_mtc_timecode({0, 1, 0, 0, MtcFrameRate::Fps2997Drop}));
    REQUIRE_FALSE(valid_mtc_timecode({0, 1, 0, 1, MtcFrameRate::Fps2997Drop}));
    REQUIRE(valid_mtc_timecode({0, 1, 0, 2, MtcFrameRate::Fps2997Drop}));
    REQUIRE(valid_mtc_timecode({0, 10, 0, 0, MtcFrameRate::Fps2997Drop}));
    REQUIRE_FALSE(valid_mtc_timecode({0, 0, 0, 0, static_cast<MtcFrameRate>(99)}));

    bool every_minute_round_trips = true;
    for (std::uint8_t hour = 0; hour < 24; ++hour) {
        for (std::uint8_t minute = 0; minute < 60; ++minute) {
            const auto first_frame = static_cast<std::uint8_t>(minute % 10 == 0 ? 0 : 2);
            const MtcTimecode boundary{hour, minute, 0, first_frame, MtcFrameRate::Fps2997Drop};
            every_minute_round_trips =
                every_minute_round_trips &&
                samples_to_mtc_timecode(mtc_timecode_to_samples(boundary, sample_rate), sample_rate,
                                        boundary.frame_rate) == boundary;
        }
    }
    REQUIRE(every_minute_round_trips);
}

TEST_CASE("MTC chaser locks on coherent forward quarter-frame cycles",
          "[playback][external-sync]") {
    MtcChaser chaser;
    constexpr RationalRate sample_rate{48'000, 1};
    const std::array nibbles{
        std::uint8_t{12}, std::uint8_t{0}, std::uint8_t{14}, std::uint8_t{1},
        std::uint8_t{2},  std::uint8_t{0}, std::uint8_t{1},  std::uint8_t{6},
    };

    for (std::uint8_t piece = 0; piece < 7; ++piece) {
        const auto update = chaser.consume(quarter_frame(piece, nibbles[piece]), sample_rate);
        REQUIRE(update.code == MtcChaseCode::Incomplete);
    }
    const auto locked = chaser.consume(quarter_frame(7, nibbles[7]), sample_rate);
    REQUIRE(locked.code == MtcChaseCode::Locked);
    REQUIRE(locked.direction == MtcDirection::Forward);
    REQUIRE(locked.timecode == MtcTimecode{1, 2, 30, 12, MtcFrameRate::Fps30});
    REQUIRE(locked.position == mtc_timecode_to_samples(locked.timecode, sample_rate));

    MtcChaser reverse;
    for (std::uint8_t piece = 7; piece > 0; --piece) {
        REQUIRE(reverse.consume(quarter_frame(piece, nibbles[piece]), sample_rate).code ==
                MtcChaseCode::Incomplete);
    }
    const auto reverse_locked = reverse.consume(quarter_frame(0, nibbles[0]), sample_rate);
    REQUIRE(reverse_locked.code == MtcChaseCode::Locked);
    REQUIRE(reverse_locked.direction == MtcDirection::Reverse);
    REQUIRE(reverse_locked.timecode == locked.timecode);
}

TEST_CASE("MTC chaser reports broken cycles and accepts full-frame locate",
          "[playback][external-sync]") {
    MtcChaser chaser;
    constexpr RationalRate sample_rate{48'000, 1};
    REQUIRE(chaser.consume(quarter_frame(0, 0), sample_rate).code == MtcChaseCode::Incomplete);
    REQUIRE(chaser.consume(quarter_frame(3, 0), sample_rate).code == MtcChaseCode::Discontinuity);
    REQUIRE(chaser.consume(quarter_frame(7, 8), sample_rate).code == MtcChaseCode::Invalid);

    const std::array<std::uint8_t, 10> locate{0xf0, 0x7f, 0x10, 0x01, 0x01,
                                              0x41, 0x02, 0x03, 0x04, 0xf7};
    const auto update = chaser.consume_sysex(locate, sample_rate);
    REQUIRE(update.code == MtcChaseCode::Locked);
    REQUIRE(update.direction == MtcDirection::Unknown);
    REQUIRE(update.timecode == MtcTimecode{1, 2, 3, 4, MtcFrameRate::Fps2997Drop});
}

TEST_CASE("MTC chaser rejects a quarter-frame cycle assembled across a direction change",
          "[playback][external-sync]") {
    MtcChaser chaser;
    constexpr RationalRate sample_rate{48'000, 1};
    const std::array nibbles{
        std::uint8_t{12}, std::uint8_t{0}, std::uint8_t{14}, std::uint8_t{1},
        std::uint8_t{2},  std::uint8_t{0}, std::uint8_t{1},  std::uint8_t{6},
    };

    for (std::uint8_t piece = 0; piece < 5; ++piece)
        REQUIRE(chaser.consume(quarter_frame(piece, nibbles[piece]), sample_rate).code ==
                MtcChaseCode::Incomplete);

    const auto changed =
        chaser.consume(quarter_frame(3, static_cast<std::uint8_t>(nibbles[3] ^ 1)), sample_rate);
    REQUIRE(changed.code == MtcChaseCode::Discontinuity);
    REQUIRE(changed.direction == MtcDirection::Reverse);

    for (std::uint8_t piece = 2; piece > 0; --piece)
        REQUIRE(chaser.consume(quarter_frame(piece, nibbles[piece]), sample_rate).code ==
                MtcChaseCode::Incomplete);
    REQUIRE(chaser.consume(quarter_frame(0, nibbles[0]), sample_rate).code ==
            MtcChaseCode::Incomplete);
}

TEST_CASE("external sync emits sample-accurate clock and MTC", "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 4'800;
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    midi::MidiBuffer output;
    output.reserve(32, 2, 10);
    output.set_realtime_capacity_limit();
    const ExternalSyncOutput sync;
    const auto result = sync.process(block(transport, 4'800), output);
    REQUIRE(result.code == ExternalSyncOutputCode::Complete);
    REQUIRE(result.sysex_messages == 1);
    REQUIRE(output.sysex_size() == 1);

    const auto event_statuses = statuses(output);
    REQUIRE(event_statuses.front() == 0xfa);
    std::vector<std::int32_t> clock_offsets;
    std::vector<std::int32_t> mtc_offsets;
    for (const auto& event : output) {
        if (event.data()[0] == 0xf8)
            clock_offsets.push_back(event.sample_offset);
        if (event.data()[0] == 0xf1)
            mtc_offsets.push_back(event.sample_offset);
    }
    REQUIRE(clock_offsets == std::vector<std::int32_t>{0, 1'000, 2'000, 3'000, 4'000});
    REQUIRE(mtc_offsets == std::vector<std::int32_t>{0, 400, 800, 1'200, 1'600, 2'000, 2'400, 2'800,
                                                     3'200, 3'600, 4'000, 4'400});
}

TEST_CASE("external sync projects MIDI clock through host beat mapping",
          "[playback][external-sync]") {
    const auto map = constant_map();
    TransportSnapshot transport;
    transport.tempo_map = &map;
    transport.sample_rate = map.sample_rate();
    transport.frame_count = 4'800;
    transport.range_count = 1;
    transport.is_playing = true;
    transport.ranges[0].frame_count = 4'800;
    transport.ranges[0].timeline_tick_start = {0};
    transport.ranges[0].timeline_tick_end = {kTicksPerQuarter / 10};
    transport.ranges[0].host_beat_mapping = true;

    midi::MidiBuffer output;
    output.reserve(8);
    output.set_realtime_capacity_limit();
    ExternalSyncOutputConfig config;
    config.emit_mtc = false;
    REQUIRE(ExternalSyncOutput(config).process(transport, output).code ==
            ExternalSyncOutputCode::Complete);
    std::vector<std::int32_t> offsets;
    for (const auto& event : output) {
        if (event.data()[0] == 0xf8)
            offsets.push_back(event.sample_offset);
    }
    REQUIRE(offsets == std::vector<std::int32_t>{0, 2'000, 4'000});
}

TEST_CASE("external sync rejects a sample rate inconsistent with its tempo map",
          "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransport transport;
    REQUIRE(transport.prepare(map, {.max_buffer_size = 64, .initially_playing = true}) ==
            TransportError::None);
    auto snapshot = block(transport, 64);
    snapshot.sample_rate = {44'100, 1};
    midi::MidiBuffer output;
    REQUIRE(ExternalSyncOutput{}.process(snapshot, output).code ==
            ExternalSyncOutputCode::InvalidSampleRate);
    REQUIRE(output.empty());
}

TEST_CASE("external sync sends song position and continue after a locate",
          "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initial_position = {2 * kTicksPerQuarter};
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    midi::MidiBuffer output;
    output.reserve(16, 2, 10);
    output.set_realtime_capacity_limit();
    ExternalSyncOutputConfig output_config;
    output_config.emit_mtc = false;
    const ExternalSyncOutput sync(output_config);
    const auto result = sync.process(block(transport, 64), output);
    REQUIRE(result.code == ExternalSyncOutputCode::Complete);
    REQUIRE(output.size() >= 2);
    REQUIRE(output[0].size() == 3);
    REQUIRE(output[0].data()[0] == 0xf2);
    REQUIRE(output[0].data()[1] == 8);
    REQUIRE(output[0].data()[2] == 0);
    REQUIRE(output[1].data()[0] == 0xfb);
}

TEST_CASE("external sync republishes song position after seeks and loop wraps",
          "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 1'024;
    config.loop = {true, {0}, {kTicksPerQuarter}};
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    (void)block(transport, 64);

    ExternalSyncOutputConfig output_config;
    output_config.emit_mtc = false;
    const ExternalSyncOutput sync(output_config);

    REQUIRE(transport.seek({kTicksPerQuarter / 2}) == TransportError::None);
    midi::MidiBuffer seek_output;
    seek_output.reserve(16);
    seek_output.set_realtime_capacity_limit();
    REQUIRE(sync.process(block(transport, 64), seek_output).code ==
            ExternalSyncOutputCode::Complete);
    REQUIRE(seek_output[0].data()[0] == 0xf2);
    REQUIRE(seek_output[0].data()[1] == 2);
    REQUIRE(seek_output[0].sample_offset == 0);
    REQUIRE(seek_output[1].data()[0] == 0xfb);

    REQUIRE(transport.seek(map.samples_to_ticks({23'500})) == TransportError::None);
    (void)block(transport, 1);
    midi::MidiBuffer wrap_output;
    wrap_output.reserve(32);
    wrap_output.set_realtime_capacity_limit();
    REQUIRE(sync.process(block(transport, 1'024), wrap_output).code ==
            ExternalSyncOutputCode::Complete);
    bool saw_wrapped_position = false;
    bool saw_wrapped_continue = false;
    for (const auto& event : wrap_output) {
        if (event.sample_offset != 499)
            continue;
        saw_wrapped_position = saw_wrapped_position || event.data()[0] == 0xf2;
        saw_wrapped_continue = saw_wrapped_continue || event.data()[0] == 0xfb;
    }
    REQUIRE(saw_wrapped_position);
    REQUIRE(saw_wrapped_continue);
}

TEST_CASE("external sync reports bounded-output overflow", "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 4'800;
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    midi::MidiBuffer output;
    output.reserve(1);
    output.set_realtime_capacity_limit();
    const ExternalSyncOutput sync;
    const auto result = sync.process(block(transport, 4'800), output);
    REQUIRE(result.code == ExternalSyncOutputCode::OutputOverflow);
    REQUIRE(output.dropped_sysex_count() > 0);
}

TEST_CASE("external sync applies a work limit even to an unbounded MIDI buffer",
          "[playback][external-sync]") {
    const auto map = constant_map(1'000.0);
    MasterTransportConfig config;
    config.max_buffer_size = 4'800;
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    midi::MidiBuffer output;
    ExternalSyncOutputConfig output_config;
    output_config.emit_mtc = false;
    output_config.max_messages_per_block = 3;
    const ExternalSyncOutput sync(output_config);
    const auto result = sync.process(block(transport, 4'800), output);
    REQUIRE(result.code == ExternalSyncOutputCode::OutputOverflow);
    REQUIRE(result.short_messages == 3);
    REQUIRE(output.size() == 3);
}

TEST_CASE("external sync rejects an unknown frame-rate configuration",
          "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    midi::MidiBuffer output;
    ExternalSyncOutputConfig output_config;
    output_config.mtc_frame_rate = static_cast<MtcFrameRate>(99);
    const ExternalSyncOutput sync(output_config);
    REQUIRE(sync.process(block(transport, 64), output).code ==
            ExternalSyncOutputCode::InvalidFrameRate);

    output_config.mtc_frame_rate = MtcFrameRate::Fps30;
    output_config.max_messages_per_block = 0;
    const ExternalSyncOutput no_capacity(output_config);
    REQUIRE(no_capacity.process(block(transport, 64), output).code ==
            ExternalSyncOutputCode::InvalidOutputLimit);
}

TEST_CASE("external sync output is invariant to callback partitioning",
          "[playback][external-sync]") {
    struct Captured {
        std::int64_t offset = 0;
        std::array<std::uint8_t, 3> bytes{};
        std::uint32_t size = 0;
        constexpr auto operator<=>(const Captured&) const = default;
    };
    const auto render = [](std::span<const std::uint32_t> blocks) {
        const auto map = constant_map(120.0, {44'100, 1});
        MasterTransportConfig config;
        config.max_buffer_size = 4'800;
        config.initially_playing = true;
        MasterTransport transport;
        REQUIRE(transport.prepare(map, config) == TransportError::None);
        ExternalSyncOutputConfig output_config;
        output_config.mtc_frame_rate = MtcFrameRate::Fps2997Drop;
        ExternalSyncOutput sync(output_config);
        std::vector<Captured> captured;
        std::int64_t base = 0;
        for (const auto frames : blocks) {
            midi::MidiBuffer output;
            output.reserve(32, 2, 10);
            output.set_realtime_capacity_limit();
            const auto result = sync.process(block(transport, frames), output);
            REQUIRE(result.code == ExternalSyncOutputCode::Complete);
            output.sort();
            for (const auto& event : output) {
                Captured item;
                item.offset = base + event.sample_offset;
                item.size = event.size();
                for (std::uint32_t index = 0; index < item.size; ++index)
                    item.bytes[index] = event.data()[index];
                captured.push_back(item);
            }
            base += frames;
        }
        return captured;
    };

    const std::array one_block{std::uint32_t{4'800}};
    const std::array partitioned{std::uint32_t{127}, std::uint32_t{257}, std::uint32_t{1'024},
                                 std::uint32_t{3'392}};
    const auto continuous = render(one_block);
    const auto split = render(partitioned);
    REQUIRE(continuous.size() == split.size());
    for (std::size_t index = 0; index < continuous.size(); ++index) {
        CAPTURE(index, continuous[index].offset, continuous[index].bytes, continuous[index].size,
                split[index].offset, split[index].bytes, split[index].size);
        REQUIRE(continuous[index] == split[index]);
    }
}

TEST_CASE("emitted MTC quarter frames form a chaseable stream", "[playback][external-sync]") {
    const auto map = constant_map();
    MasterTransportConfig config;
    config.max_buffer_size = 3'200;
    config.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, config) == TransportError::None);

    midi::MidiBuffer output;
    output.reserve(32, 2, 10);
    output.set_realtime_capacity_limit();
    const ExternalSyncOutput sync;
    REQUIRE(sync.process(block(transport, 3'200), output).code == ExternalSyncOutputCode::Complete);

    MtcChaser chaser;
    MtcChaseUpdate update;
    for (const auto& event : output) {
        if (event.data()[0] == 0xf1)
            update = chaser.consume(event, {48'000, 1});
    }
    REQUIRE(update.code == MtcChaseCode::Locked);
    REQUIRE(update.direction == MtcDirection::Forward);
    REQUIRE(update.timecode == MtcTimecode{0, 0, 0, 2, MtcFrameRate::Fps30});
}
