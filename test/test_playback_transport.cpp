#include <pulp/format/playback_context_projection.hpp>
#include <pulp/playback/transport.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <span>
#include <thread>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map() {
    const std::array points{TempoPoint{{0}, 120.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

MasterTransportConfig config(std::uint32_t maximum = 1024) {
    MasterTransportConfig result;
    result.max_buffer_size = maximum;
    return result;
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

} // namespace

static_assert(std::is_trivially_copyable_v<TransportRange>);
static_assert(std::is_trivially_copyable_v<TransportSnapshot>);

TEST_CASE("master transport starts without inventing a reset or jump") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto snapshot = block(transport, 64);
    REQUIRE(snapshot.is_playing);
    REQUIRE_FALSE(snapshot.transport_changed);
    REQUIRE(snapshot.transport_started);
    REQUIRE_FALSE(snapshot.reset_requested);
    REQUIRE_FALSE(snapshot.time_sig_changed);
    REQUIRE(snapshot.range_count == 1);
    REQUIRE_FALSE(snapshot.ranges[0].discontinuity);
    REQUIRE(snapshot.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::None);
    REQUIRE(snapshot.ranges[0].frame_count == 64);
    REQUIRE_FALSE(snapshot.ranges[0].tempo_changed);
    REQUIRE(snapshot.ranges[0].timeline_sample_start == SamplePosition{0});
}

TEST_CASE("stop holds both musical clocks and restart is start-only") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto rolling = block(transport, 64);
    (void)rolling;

    REQUIRE(transport.set_playing(false) == TransportError::None);
    const auto stopped = block(transport, 257);
    REQUIRE_FALSE(stopped.is_playing);
    REQUIRE(stopped.transport_changed);
    REQUIRE(stopped.range_count == 1);
    REQUIRE(stopped.ranges[0].frame_count == 257);
    REQUIRE(stopped.ranges[0].timeline_tick_start == stopped.ranges[0].timeline_tick_end);
    REQUIRE(stopped.ranges[0].monotonic_start == stopped.ranges[0].monotonic_end);

    REQUIRE(transport.set_playing(true) == TransportError::None);
    const auto restarted = block(transport, 1);
    REQUIRE(restarted.transport_started);
    REQUIRE_FALSE(restarted.reset_requested);
    REQUIRE_FALSE(restarted.ranges[0].discontinuity);
    REQUIRE(restarted.ranges[0].timeline_sample_start ==
            stopped.ranges[0].timeline_sample_start);
}

TEST_CASE("seek reanchors timeline while monotonic time remains continuous") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto before_seek = block(transport, 64);

    const TickPosition target{kTicksPerQuarter};
    REQUIRE(transport.seek(target) == TransportError::None);
    const auto snapshot = block(transport, 64);
    REQUIRE(snapshot.reset_requested);
    REQUIRE(snapshot.ranges[0].discontinuity);
    REQUIRE(snapshot.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::Seek);
    REQUIRE(snapshot.ranges[0].timeline_sample_start == map.ticks_to_samples(target));
    REQUIRE(snapshot.ranges[0].timeline_tick_start == target);
    REQUIRE(snapshot.ranges[0].monotonic_start ==
            before_seek.ranges[0].monotonic_end);

    const auto before_backward_seek = snapshot.ranges[0].monotonic_end;
    REQUIRE(transport.seek({-kTicksPerQuarter}) == TransportError::None);
    const auto backward = block(transport, 64);
    REQUIRE(backward.ranges[0].timeline_tick_start ==
            TickPosition{-kTicksPerQuarter});
    REQUIRE(backward.ranges[0].monotonic_start == before_backward_seek);
    REQUIRE(backward.ranges[0].monotonic_end >= backward.ranges[0].monotonic_start);
}

TEST_CASE("stopped seek holds monotonic time and wrap after seek preserves it") {
    const auto map = constant_map();
    auto setup = config();
    setup.loop = {true, {0}, {kTicksPerQuarter}};
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto rolling = block(transport, 64);

    REQUIRE(transport.set_playing(false) == TransportError::None);
    (void)block(transport, 64);
    REQUIRE(transport.seek({kTicksPerQuarter / 2}) == TransportError::None);
    const auto stopped_seek = block(transport, 64);
    REQUIRE(stopped_seek.ranges[0].monotonic_start ==
            rolling.ranges[0].monotonic_end);
    REQUIRE(stopped_seek.ranges[0].monotonic_end ==
            stopped_seek.ranges[0].monotonic_start);

    REQUIRE(transport.seek(map.samples_to_ticks({23'500})) == TransportError::None);
    REQUIRE(transport.set_playing(true) == TransportError::None);
    const auto wrapped = block(transport, 1024);
    REQUIRE(wrapped.range_count == 2);
    REQUIRE(wrapped.ranges[0].monotonic_start ==
            stopped_seek.ranges[0].monotonic_end);
    REQUIRE(wrapped.ranges[1].monotonic_start == wrapped.ranges[0].monotonic_end);
    REQUIRE(wrapped.ranges[1].monotonic_end >= wrapped.ranges[1].monotonic_start);
}

TEST_CASE("variable blocks equal one continuous block at constant tempo") {
    const auto map = constant_map();
    auto setup = config(2048);
    setup.initially_playing = true;
    MasterTransport partitioned;
    MasterTransport continuous;
    REQUIRE(partitioned.prepare(map, setup) == TransportError::None);
    REQUIRE(continuous.prepare(map, setup) == TransportError::None);

    TransportSnapshot last;
    for (const auto frames : std::array<std::uint32_t, 4>{1, 64, 257, 1024})
        last = block(partitioned, frames);
    const auto one = block(continuous, 1346);
    REQUIRE(last.ranges[0].timeline_tick_end == one.ranges[0].timeline_tick_end);
    REQUIRE(last.ranges[0].monotonic_end == one.ranges[0].monotonic_end);
}

TEST_CASE("variable blocks equal one continuous block across a tempo ramp") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{2 * kTicksPerQuarter}, 180.0},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    auto setup = config(2048);
    setup.initially_playing = true;
    MasterTransport partitioned;
    MasterTransport continuous;
    REQUIRE(partitioned.prepare(map, setup) == TransportError::None);
    REQUIRE(continuous.prepare(map, setup) == TransportError::None);

    TransportSnapshot last;
    for (const auto frames : std::array<std::uint32_t, 4>{1, 64, 257, 1024})
        last = block(partitioned, frames);
    const auto one = block(continuous, 1346);
    REQUIRE(last.ranges[0].timeline_tick_end == one.ranges[0].timeline_tick_end);
    REQUIRE(last.ranges[0].monotonic_end == one.ranges[0].monotonic_end);
}

TEST_CASE("tempo query and transport cross a ramp analytically") {
    const std::array points{
        TempoPoint{{0}, 60.0, TempoCurve::LinearInTicks},
        TempoPoint{{kTicksPerQuarter}, 180.0},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    REQUIRE(map.tempo_at_tick({0}) == Catch::Approx(60.0));
    REQUIRE(map.tempo_at_tick({kTicksPerQuarter / 2}) == Catch::Approx(120.0));
    REQUIRE(map.tempo_at_tick({kTicksPerQuarter}) == Catch::Approx(180.0));

    auto setup = config(64'000);
    setup.initially_playing = true;
    setup.initial_position = {kTicksPerQuarter / 2 - 1};
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto snapshot = block(transport, 32'000);
    REQUIRE(snapshot.ranges[0].timeline_tick_start.value < kTicksPerQuarter / 2);
    REQUIRE(snapshot.ranges[0].timeline_tick_end.value > kTicksPerQuarter / 2);
    REQUIRE(snapshot.tempo_bpm == Catch::Approx(
        map.tempo_at_tick(snapshot.ranges[0].timeline_tick_start)));
    REQUIRE(snapshot.tempo_bpm == snapshot.ranges[0].tempo_bpm);
}

TEST_CASE("one loop wrap yields exactly two contiguous block ranges") {
    const auto map = constant_map();
    const LoopRegion loop{true, {0}, {kTicksPerQuarter}};
    auto setup = config();
    setup.initially_playing = true;
    setup.loop = loop;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto snapshot = block(transport, 1024);
    REQUIRE(snapshot.range_count == 2);
    REQUIRE(snapshot.ranges[0].sample_offset == 0);
    REQUIRE(snapshot.ranges[0].frame_count == 500);
    REQUIRE(snapshot.ranges[1].sample_offset == 500);
    REQUIRE(snapshot.ranges[1].frame_count == 524);
    REQUIRE(snapshot.ranges[0].frame_count + snapshot.ranges[1].frame_count == 1024);
    REQUIRE_FALSE(snapshot.ranges[0].discontinuity);
    REQUIRE(snapshot.ranges[1].discontinuity);
    REQUIRE(snapshot.ranges[1].discontinuity_reason ==
            TransportDiscontinuityReason::LoopWrap);
    REQUIRE(snapshot.ranges[1].timeline_sample_start == map.ticks_to_samples(loop.start));
    REQUIRE(snapshot.ranges[1].monotonic_start == snapshot.ranges[0].monotonic_end);
}

TEST_CASE("loop split ranges carry their own start tempo") {
    const std::array points{
        TempoPoint{{0}, 60.0},
        TempoPoint{{kTicksPerQuarter}, 180.0},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    const LoopRegion loop{true, {0}, {2 * kTicksPerQuarter}};
    auto setup = config();
    setup.initially_playing = true;
    setup.loop = loop;
    setup.initial_position = map.samples_to_ticks(
        {map.ticks_to_samples(loop.end).value - 500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto snapshot = block(transport, 1024);
    REQUIRE(snapshot.range_count == 2);
    REQUIRE(snapshot.ranges[0].tempo_bpm == Catch::Approx(180.0));
    REQUIRE(snapshot.ranges[1].tempo_bpm == Catch::Approx(60.0));
    REQUIRE(snapshot.ranges[1].tempo_changed);
    REQUIRE(snapshot.tempo_bpm == snapshot.ranges[0].tempo_bpm);
    const auto second =
        format::project_process_context(snapshot, snapshot.ranges[1]);
    REQUIRE(second.tempo_bpm == Catch::Approx(60.0));
}

TEST_CASE("negative preroll advances and extreme positions stay total") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    setup.initial_position = {-kTicksPerQuarter};
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto preroll = block(transport, 64);
    REQUIRE(preroll.ranges[0].timeline_sample_start.value < 0);
    REQUIRE(preroll.ranges[0].timeline_tick_end > preroll.ranges[0].timeline_tick_start);

    REQUIRE(transport.seek({std::numeric_limits<std::int64_t>::max()}) ==
            TransportError::None);
    const auto maximum = block(transport, 1024);
    REQUIRE(maximum.ranges[0].timeline_sample_start.value ==
            map.ticks_to_samples({std::numeric_limits<std::int64_t>::max()}).value);
    REQUIRE(maximum.ranges[0].timeline_tick_start.value ==
            std::numeric_limits<std::int64_t>::max());
    REQUIRE(maximum.ranges[0].timeline_tick_end.value ==
            std::numeric_limits<std::int64_t>::max());
}

TEST_CASE("invalid meter loops and frame counts fail without advancing") {
    const auto map = constant_map();
    MasterTransport transport;
    auto bad_meter = config();
    bad_meter.meter = {4, 3};
    REQUIRE(transport.prepare(map, bad_meter) == TransportError::InvalidMeter);

    auto impossible_block = config(std::numeric_limits<std::uint32_t>::max());
    REQUIRE(transport.prepare(map, impossible_block) == TransportError::InvalidFrameCount);

    auto bad_loop = config();
    bad_loop.loop = {true, {0}, {0}};
    REQUIRE(transport.prepare(map, bad_loop) == TransportError::InvalidLoop);

    auto subsample_loop = config();
    subsample_loop.loop = {true, {0}, {1}};
    REQUIRE(transport.prepare(map, subsample_loop) == TransportError::InvalidLoop);

    auto short_loop = config();
    short_loop.loop = {true, {0}, map.samples_to_ticks({1000})};
    REQUIRE(transport.prepare(map, short_loop) ==
            TransportError::LoopTooShortForMaximumBlock);

    auto setup = config();
    setup.initially_playing = true;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    TransportSnapshot untouched;
    REQUIRE(transport.begin_block(0, untouched) == TransportError::InvalidFrameCount);
    REQUIRE(transport.begin_block(1025, untouched) == TransportError::InvalidFrameCount);
    const auto first = block(transport, 1);
    REQUIRE(first.block_index == 0);
    REQUIRE(first.ranges[0].timeline_sample_start == SamplePosition{0});
    REQUIRE(transport.set_loop({true, {0}, map.samples_to_ticks({100})}) ==
            TransportError::LoopTooShortForMaximumBlock);
    REQUIRE(transport.set_meter({7, 3}) == TransportError::InvalidMeter);
}

TEST_CASE("control SeqLock remains coherent under concurrent publication") {
    const auto map = constant_map();
    auto setup = config(64);
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    std::atomic<bool> done{false};
    std::atomic<bool> writer_failed{false};
    std::thread writer([&] {
        for (int i = 0; i < 10'000; ++i) {
            if (transport.set_meter((i & 1) ? MeterSignature{3, 8}
                                             : MeterSignature{7, 16}) !=
                TransportError::None) {
                writer_failed.store(true, std::memory_order_relaxed);
                break;
            }
        }
        done.store(true, std::memory_order_release);
    });

    bool reader_failed = false;
    while (!done.load(std::memory_order_acquire)) {
        const auto snapshot = block(transport, 1);
        const bool initial = snapshot.meter == MeterSignature{4, 4};
        const bool first = snapshot.meter == MeterSignature{3, 8};
        const bool second = snapshot.meter == MeterSignature{7, 16};
        if (!(initial || first || second)) {
            reader_failed = true;
            break;
        }
    }
    writer.join();
    REQUIRE_FALSE(reader_failed);
    REQUIRE_FALSE(writer_failed.load(std::memory_order_relaxed));
}

TEST_CASE("begin_block is declared and probed as lock and allocation free") {
    STATIC_REQUIRE(MasterTransport::begin_block_rt_safety_class ==
                   audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    TransportSnapshot snapshot;
    TransportError error = TransportError::NotPrepared;
    std::size_t allocation_count = 0;
    {
        test::ScopedRtProcessProbe probe;
        error = transport.begin_block(1024, snapshot);
        allocation_count = probe.allocation_count();
    }
    REQUIRE(error == TransportError::None);
    REQUIRE(allocation_count == 0);
}

TEST_CASE("change metadata matches first block and playhead diff semantics") {
    const std::array points{
        TempoPoint{{0}, 60.0},
        TempoPoint{{kTicksPerQuarter}, 180.0},
    };
    const auto map = require_compiled_tempo_map(points, RationalRate{48'000, 1});
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto first = block(transport, 64);
    REQUIRE(first.transport_started);
    REQUIRE_FALSE(first.transport_changed);
    REQUIRE_FALSE(first.time_sig_changed);
    REQUIRE_FALSE(first.ranges[0].tempo_changed);

    REQUIRE(transport.set_meter({7, 8}) == TransportError::None);
    const auto meter = block(transport, 64);
    REQUIRE(meter.time_sig_changed);
    REQUIRE_FALSE(meter.transport_changed);
    const auto meter_context =
        format::project_process_context(meter, meter.ranges[0]);
    REQUIRE(meter_context.time_sig_changed);

    REQUIRE(transport.set_loop({true, {0}, {2 * kTicksPerQuarter}}) ==
            TransportError::None);
    const auto loop = block(transport, 64);
    REQUIRE(loop.transport_changed);
    REQUIRE_FALSE(loop.transport_started);
    REQUIRE_FALSE(loop.time_sig_changed);
    const auto loop_context =
        format::project_process_context(loop, loop.ranges[0]);
    REQUIRE(loop_context.transport_changed);

    REQUIRE(transport.set_loop({true, {kTicksPerQuarter},
                                      {3 * kTicksPerQuarter}}) ==
            TransportError::None);
    const auto moved_loop = block(transport, 64);
    REQUIRE(moved_loop.transport_changed);
    REQUIRE(moved_loop.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopConfiguration);

    REQUIRE(transport.set_loop({false, {}, {}}) == TransportError::None);
    const auto disabled_loop = block(transport, 64);
    REQUIRE(disabled_loop.transport_changed);
    REQUIRE(disabled_loop.ranges[0].discontinuity_reason ==
            TransportDiscontinuityReason::LoopConfiguration);

    REQUIRE(transport.set_playing(false) == TransportError::None);
    const auto stopped = block(transport, 64);
    REQUIRE(stopped.transport_changed);
    REQUIRE_FALSE(stopped.transport_started);

    REQUIRE(transport.seek({kTicksPerQuarter}) == TransportError::None);
    const auto new_tempo = block(transport, 64);
    REQUIRE(new_tempo.ranges[0].tempo_changed);
    const auto tempo_context =
        format::project_process_context(new_tempo, new_tempo.ranges[0]);
    REQUIRE(tempo_context.tempo_changed);
}

TEST_CASE("format projection preserves absolute bars across meter changes") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    REQUIRE(transport.seek({4 * kTicksPerQuarter}) == TransportError::None);
    (void)block(transport, 1);
    REQUIRE(transport.set_meter({3, 4}) == TransportError::None);
    const auto changed = block(transport, 1);
    REQUIRE(format::project_process_context(changed, changed.ranges[0]).bar == 1);

    REQUIRE(transport.seek({6 * kTicksPerQuarter}) == TransportError::None);
    const auto later = block(transport, 1);
    const auto context = format::project_process_context(later, later.ranges[0]);
    REQUIRE(context.time_sig_numerator == 3);
    REQUIRE(context.bar == 1);

    auto prestart_setup = config();
    prestart_setup.initial_position = {6 * kTicksPerQuarter};
    MasterTransport prestart;
    REQUIRE(prestart.prepare(map, prestart_setup) == TransportError::None);
    REQUIRE(prestart.set_meter({3, 4}) == TransportError::None);
    const auto first = block(prestart, 1);
    REQUIRE(format::project_process_context(first, first.ranges[0]).bar == 2);
}

TEST_CASE("format projection maps common fields and keeps transitions range-local") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    setup.meter = {3, 4};
    setup.loop = {true, {0}, {kTicksPerQuarter}};
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto snapshot = block(transport, 1024);

    format::PlaybackContextProjectionOptions options;
    options.process_mode = format::ProcessMode::Offline;
    options.render_speed_hint = format::RenderSpeedHint::FasterThanRealtime;
    options.is_bypassed = true;
    options.is_tail_drain = true;
    options.host_time_ns = 42;
    options.frame_rate = format::FrameRate::fps_24;

    const auto first = format::project_process_context(snapshot, snapshot.ranges[0], options);
    REQUIRE(first.sample_rate == Catch::Approx(48'000.0));
    REQUIRE(first.num_samples == 500);
    REQUIRE(first.position_samples == 23'500);
    REQUIRE(first.position_beats == Catch::Approx(
        static_cast<double>(snapshot.ranges[0].timeline_tick_start.value) /
        static_cast<double>(kTicksPerQuarter)));
    REQUIRE(first.tempo_bpm == Catch::Approx(snapshot.ranges[0].tempo_bpm));
    REQUIRE(first.time_sig_numerator == 3);
    REQUIRE(first.time_sig_denominator == 4);
    REQUIRE(first.is_looping);
    REQUIRE(first.loop_start_beats == Catch::Approx(0.0));
    REQUIRE(first.loop_end_beats == Catch::Approx(1.0));
    REQUIRE(first.transport_started);
    REQUIRE_FALSE(first.transport_changed);
    REQUIRE_FALSE(first.tempo_changed);
    REQUIRE_FALSE(first.time_sig_changed);
    REQUIRE_FALSE(first.reset_requested);
    REQUIRE_FALSE(first.transport_jump);
    REQUIRE(first.is_offline());
    REQUIRE(first.render_speed_hint == format::RenderSpeedHint::FasterThanRealtime);
    REQUIRE_FALSE(first.is_recording);
    REQUIRE(first.is_bypassed);
    REQUIRE(first.is_tail_drain);
    REQUIRE(first.host_time_ns == 42);
    REQUIRE(first.frame_rate == format::FrameRate::fps_24);

    const auto second = format::project_process_context(snapshot, snapshot.ranges[1]);
    REQUIRE(second.transport_jump);
    REQUIRE(second.tempo_changed == snapshot.ranges[1].tempo_changed);
    REQUIRE_FALSE(second.transport_changed);
    REQUIRE_FALSE(second.transport_started);
    REQUIRE_FALSE(second.time_sig_changed);
    REQUIRE(second.host_time_ns == 0);
    REQUIRE(second.frame_rate == format::FrameRate::unknown);
}

TEST_CASE("seek projection is a jump and a start never implies reset") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);
    const auto started = block(transport, 1);
    const auto started_context =
        format::project_process_context(started, started.ranges[0]);
    REQUIRE(started_context.transport_started);
    REQUIRE_FALSE(started_context.reset_requested);
    REQUIRE_FALSE(started_context.transport_jump);

    REQUIRE(transport.seek({kTicksPerQuarter}) == TransportError::None);
    const auto seeked = block(transport, 1);
    const auto seek_context =
        format::project_process_context(seeked, seeked.ranges[0]);
    REQUIRE(seek_context.transport_jump);
    REQUIRE(seek_context.reset_requested);
}

TEST_CASE("projection preserves unavailable-field sentinels when looping is disabled") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    const auto snapshot = block(transport, 64);
    const auto context =
        format::project_process_context(snapshot, snapshot.ranges[0]);
    REQUIRE_FALSE(context.is_looping);
    REQUIRE(context.loop_start_beats == 0.0);
    REQUIRE(context.loop_end_beats == 0.0);
    REQUIRE(context.host_time_ns == 0);
    REQUIRE(context.frame_rate == format::FrameRate::unknown);

    auto oversized_range = snapshot.ranges[0];
    oversized_range.frame_count = std::numeric_limits<std::uint32_t>::max();
    const auto clamped = format::project_process_context(snapshot, oversized_range);
    REQUIRE(clamped.num_samples == std::numeric_limits<int>::max());
}

TEST_CASE("projection clamps extreme bar indices before integer conversion") {
    TransportSnapshot snapshot;
    snapshot.sample_rate = {48'000, 1};
    snapshot.meter = {1, 1 << 30};
    snapshot.range_count = 1;

    snapshot.ranges[0].bar_start =
        {std::numeric_limits<std::int64_t>::max()};
    const auto positive =
        format::project_process_context(snapshot, snapshot.ranges[0]);
    REQUIRE(positive.bar == std::numeric_limits<std::int64_t>::max());

    snapshot.ranges[0].bar_start =
        {std::numeric_limits<std::int64_t>::min()};
    const auto negative =
        format::project_process_context(snapshot, snapshot.ranges[0]);
    REQUIRE(negative.bar == std::numeric_limits<std::int64_t>::min());
}
