#include <pulp/format/playback_context_projection.hpp>
#include <pulp/sequence/host_transport_projector.hpp>
#include <pulp/timebase/compiled_tempo_map.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <memory>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

std::shared_ptr<const CompiledTempoMap> tempo_map(RationalRate rate = {48'000, 1}) {
    const std::array points{TempoPoint{{0}, 120.0}};
    auto compiled = CompiledTempoMap::compile(points, rate);
    REQUIRE(compiled);
    return std::make_shared<const CompiledTempoMap>(std::move(compiled).value());
}

} // namespace

TEST_CASE("host transport projection fails closed for unsupported loop density "
          "and saturates at the sample-domain boundary") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_end_beats = 0.0005;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::LoopTooShortForBlock);

    context.is_looping = false;
    context.position_samples = std::numeric_limits<std::int64_t>::max() - 4;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);

    context.position_samples = std::numeric_limits<std::int64_t>::max();
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
}

TEST_CASE("host transport projection ignores loop bounds withheld by a validity mask") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_end_beats = 0.0005;
    context.transport_validity.set(format::TransportField::Looping);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.loop.enabled);

    context.transport_validity.set(format::TransportField::LoopRange);
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::InvalidHostBeatClock);
}

TEST_CASE("host transport projection preserves authoritative positions outside an active loop") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_start_beats = 0.0;
    context.loop_end_beats = 64.0 / 24'000.0;
    context.position_samples = 128;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 128);

    context.position_samples = 160;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 160);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
}

TEST_CASE("host transport projection re-anchors after an exact second loop wrap") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 96) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 96;
    context.is_playing = true;
    context.is_looping = true;
    context.loop_start_beats = 0.0;
    context.loop_end_beats = 64.0 / 24'000.0;
    context.position_samples = 32;
    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 0);
    REQUIRE(projected.ranges[1].frame_count == 64);

    context.num_samples = 32;
    context.position_samples = 0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
}

TEST_CASE("host transport projection anchors blocks and seeks to a valid host beat clock") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 24'000) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 24'000;
    context.is_playing = true;
    context.tempo_bpm = 60.0;
    context.position_beats = 2.0;
    context.position_samples = 96'000;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 2 * kTicksPerQuarter);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 5 * kTicksPerQuarter / 2);
    REQUIRE(projected.ranges[0].tempo_bpm == 60.0);

    context.position_beats = 2.5;
    context.position_samples = 120'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 5 * kTicksPerQuarter / 2);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 3 * kTicksPerQuarter);
    REQUIRE_FALSE(projected.reset_requested);

    context.position_beats = 1.0;
    context.position_samples = 48'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == kTicksPerQuarter);
    REQUIRE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
}

TEST_CASE("host beat projection preserves fractional tick phase across small blocks") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 1) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double tempo_bpm = 123.0;
    constexpr double start_beat = 10.0;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 1;
    context.is_playing = true;
    context.tempo_bpm = tempo_bpm;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    long double previous_end_sample = 0.0L;
    for (std::int64_t frame = 0; frame < 64; ++frame) {
        context.position_samples = frame;
        context.position_beats =
            start_beat + static_cast<double>(frame) * tempo_bpm / (60.0 * sample_rate);
        TransportSnapshot projected;
        REQUIRE(projector.project(context, projected) ==
                sequence::HostTransportProjectionError::None);
        const auto& range = projected.ranges[0];
        REQUIRE(range.has_precise_host_ticks);
        const auto expected_start_tick =
            (start_beat + static_cast<double>(frame) * tempo_bpm / (60.0 * sample_rate)) *
            kTicksPerQuarter;
        const auto expected_end_tick =
            (start_beat + static_cast<double>(frame + 1) * tempo_bpm / (60.0 * sample_rate)) *
            kTicksPerQuarter;
        REQUIRE(range.host_tick_start == Catch::Approx(expected_start_tick));
        REQUIRE(range.host_tick_end == Catch::Approx(expected_end_tick));
        const auto nested = format::project_process_context(projected, range);
        REQUIRE(nested.has_transport(format::TransportField::BeatPosition));
        REQUIRE(nested.position_beats == Catch::Approx(context.position_beats));
        const auto start_sample = host_mapped_document_sample_at_output_offset(range, *map, 0);
        const auto end_sample = host_mapped_document_sample_at_output_offset(range, *map, 1);
        if (frame != 0)
            REQUIRE(start_sample == Catch::Approx(static_cast<double>(previous_end_sample)));
        REQUIRE(end_sample > start_sample);
        previous_end_sample = end_sample;
    }
    REQUIRE(previous_end_sample > map->fractional_ticks_to_samples(start_beat * kTicksPerQuarter));
}

TEST_CASE("host beat-domain loop projection splits and resumes at host tempo") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 16'000) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 16'000;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = 1.75;
    // Deliberately use a sample/beat origin that cannot be derived by assuming
    // the current host tempo has applied since beat zero.
    context.position_samples = 100'000;
    context.loop_start_beats = 1.0;
    context.loop_end_beats = 2.0;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 12'000);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == 7 * kTicksPerQuarter / 4);
    REQUIRE(projected.ranges[0].timeline_tick_end.value == 2 * kTicksPerQuarter);
    REQUIRE(projected.ranges[1].sample_offset == 12'000);
    REQUIRE(projected.ranges[1].frame_count == 4'000);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 64'000);
    REQUIRE(projected.ranges[1].timeline_tick_start.value == kTicksPerQuarter);
    REQUIRE(projected.ranges[1].timeline_tick_end.value ==
            kTicksPerQuarter + kTicksPerQuarter / 12);
    REQUIRE(projected.ranges[1].discontinuity);

    context.num_samples = 1'000;
    context.position_beats = 1.0 + 4'000.0 / 48'000.0;
    context.position_samples = 68'000;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE(projected.ranges[0].timeline_tick_start.value ==
            kTicksPerQuarter + kTicksPerQuarter / 12);
}

TEST_CASE("host beat-domain loop projection keeps the final fractional frame cell") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 15) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double loop_start_beats = 1.0;
    const double loop_end_beats = loop_start_beats + 100.4 / sample_rate;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 15;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = loop_end_beats - 10.4 / sample_rate;
    context.position_samples = 100'000;
    context.loop_start_beats = loop_start_beats;
    context.loop_end_beats = loop_end_beats;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 11);
    REQUIRE(projected.ranges[0].timeline_tick_end == projected.loop.end);
    REQUIRE(projected.ranges[1].sample_offset == 11);
    REQUIRE(projected.ranges[1].frame_count == 4);
    REQUIRE(projected.ranges[1].timeline_sample_start.value == 99'910);
    REQUIRE(projected.ranges[1].timeline_tick_start == projected.loop.start);

    context.num_samples = 8;
    context.position_beats = loop_start_beats + 4.0 / sample_rate;
    context.position_samples = 99'914;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE_FALSE(projected.ranges[0].discontinuity);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 99'914);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            TickPosition{static_cast<std::int64_t>(
                std::llround(context.position_beats * static_cast<double>(kTicksPerQuarter)))});
}

TEST_CASE("host beat-domain loop projection clamps a ceil-rounded complete loop") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 112) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double loop_start_beats = 1.0;
    const double loop_end_beats = loop_start_beats + 100.4 / sample_rate;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 112;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = loop_end_beats - 10.4 / sample_rate;
    context.position_samples = 100'000;
    context.loop_start_beats = loop_start_beats;
    context.loop_end_beats = loop_end_beats;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 11);
    REQUIRE(projected.ranges[1].frame_count == 101);
    REQUIRE(projected.ranges[1].timeline_tick_end == projected.loop.end);
    REQUIRE(projected.ranges[1].host_tick_end ==
            Catch::Approx(loop_end_beats * static_cast<double>(kTicksPerQuarter)));
}

TEST_CASE("host beat-domain loop admission keeps a sub-tick final cell") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 2) == sequence::HostTransportProjectionError::None);

    const auto tick_scale = static_cast<double>(kTicksPerQuarter);
    const auto loop_end_beats = 100.4 / tick_scale;
    const auto position_beats = 100.1 / tick_scale;
    REQUIRE(std::llround(position_beats * tick_scale) == std::llround(loop_end_beats * tick_scale));
    REQUIRE(position_beats < loop_end_beats);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 2;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = position_beats;
    context.position_samples = 100'000;
    context.loop_start_beats = 0.0;
    context.loop_end_beats = loop_end_beats;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 2);
    REQUIRE(projected.ranges[0].frame_count == 1);
    REQUIRE(projected.ranges[0].timeline_tick_start == projected.loop.end);
    REQUIRE(projected.ranges[0].timeline_tick_end == projected.loop.end);
    REQUIRE(projected.ranges[1].sample_offset == 1);
    REQUIRE(projected.ranges[1].frame_count == 1);
    REQUIRE(projected.ranges[1].timeline_tick_start == projected.loop.start);
    REQUIRE(projected.ranges[1].discontinuity);
}

TEST_CASE("host beat-domain loop projection carries a block-edge wrap into the next block") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 11) == sequence::HostTransportProjectionError::None);

    constexpr double sample_rate = 48'000.0;
    constexpr double loop_start_beats = 1.0;
    const double loop_end_beats = loop_start_beats + 100.4 / sample_rate;
    format::ProcessContext context;
    context.sample_rate = sample_rate;
    context.num_samples = 11;
    context.is_playing = true;
    context.is_looping = true;
    context.tempo_bpm = 60.0;
    context.position_beats = loop_end_beats - 10.4 / sample_rate;
    context.position_samples = 100'000;
    context.loop_start_beats = loop_start_beats;
    context.loop_end_beats = loop_end_beats;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.transport_validity.set(format::TransportField::LoopRange);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.range_count == 1);
    REQUIRE(projected.ranges[0].frame_count == 11);
    REQUIRE(projected.ranges[0].timeline_tick_end == projected.loop.end);

    context.num_samples = 8;
    context.position_beats = loop_start_beats;
    context.position_samples = 99'910;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.reset_requested);
    REQUIRE(projected.ranges[0].timeline_sample_start.value == 99'910);
}

TEST_CASE("host beat-domain projection falls back when beat clock validity is incomplete") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 24'000;
    context.position_beats = 2.0;
    context.tempo_bpm = 60.0;

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'032;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::SamplePosition);
    context.tempo_bpm = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'064;
    context.position_beats = std::numeric_limits<double>::max();
    context.tempo_bpm = 60.0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));
    REQUIRE(projected.ranges[0].tempo_bpm == 120.0);

    context.position_samples = 24'096;
    context.position_beats = 1.0;
    context.tempo_bpm = 60.0;
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE_FALSE(projected.ranges[0].host_beat_mapping);
    REQUIRE(projected.ranges[0].timeline_tick_start ==
            map->samples_to_ticks({context.position_samples}));

    context.position_samples = 24'128;
    context.transport_validity.set(format::TransportField::Tempo);
    REQUIRE(projector.project(context, projected) == sequence::HostTransportProjectionError::None);
    REQUIRE(projected.ranges[0].host_beat_mapping);
    REQUIRE(projected.reset_requested);
    REQUIRE(projected.ranges[0].discontinuity);
    REQUIRE(projected.ranges[0].timeline_tick_start.value == kTicksPerQuarter);
}

TEST_CASE("host loop projection rejects an incomplete authoritative beat clock") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    for (const auto present :
         {format::TransportField::BeatPosition, format::TransportField::Tempo}) {
        format::ProcessContext context;
        context.sample_rate = 48'000.0;
        context.num_samples = 32;
        context.is_playing = true;
        context.is_looping = true;
        context.position_samples = 24'000;
        context.position_beats = 1.0;
        context.tempo_bpm = 60.0;
        context.loop_start_beats = 0.0;
        context.loop_end_beats = 2.0;
        context.transport_validity.set(format::TransportField::SamplePosition);
        context.transport_validity.set(format::TransportField::LoopRange);
        context.transport_validity.set(present);

        TransportSnapshot projected;
        REQUIRE(projector.project(context, projected) ==
                sequence::HostTransportProjectionError::InvalidHostBeatClock);
    }
}

TEST_CASE("host beat-domain projection rejects invalid authoritative beat fields") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 24'000;
    context.position_beats = 1.0;
    context.tempo_bpm = std::numeric_limits<double>::quiet_NaN();
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::InvalidHostBeatClock);

    context.tempo_bpm = 60.0;
    context.position_beats = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::InvalidHostBeatClock);

    context.position_beats = 1.0;
    context.tempo_bpm = 0.0;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::InvalidHostBeatClock);
}

TEST_CASE("host beat-domain projection rejects an authoritative beat outside the tick domain") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 24'000;
    context.position_beats = std::numeric_limits<double>::max();
    context.tempo_bpm = 60.0;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::BeatPositionOutOfRange);
}

TEST_CASE("host transport projection rejects a withheld sample position") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 0;
    context.position_beats = 1.0;
    context.tempo_bpm = 120.0;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::InvalidHostBeatClock);
}

TEST_CASE("host beat-domain projection rejects a precise range that cannot advance") {
    const auto map = tempo_map();
    sequence::HostTransportProjector projector;
    REQUIRE(projector.prepare(*map, 32) == sequence::HostTransportProjectionError::None);

    format::ProcessContext context;
    context.sample_rate = 48'000.0;
    context.num_samples = 32;
    context.is_playing = true;
    context.position_samples = 24'000;
    context.position_beats = 1.0e13;
    context.tempo_bpm = 60.0;
    context.transport_validity.set(format::TransportField::BeatPosition);
    context.transport_validity.set(format::TransportField::Tempo);
    context.transport_validity.set(format::TransportField::SamplePosition);

    TransportSnapshot projected;
    REQUIRE(projector.project(context, projected) ==
            sequence::HostTransportProjectionError::BeatPositionOutOfRange);
}
