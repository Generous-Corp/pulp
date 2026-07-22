#include <pulp/playback/clip_launch.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline/clip_launch.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace pulp;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

CompiledTempoMap constant_map(double bpm = 120.0, RationalRate rate = {48'000, 1}) {
    const std::array points{TempoPoint{{0}, bpm}};
    return require_compiled_tempo_map(points, rate);
}

MasterTransportConfig config(std::uint32_t maximum = 8192) {
    MasterTransportConfig result;
    result.max_buffer_size = maximum;
    return result;
}

TransportSnapshot block(MasterTransport& transport, std::uint32_t frames) {
    TransportSnapshot snapshot;
    REQUIRE(transport.begin_block(frames, snapshot) == TransportError::None);
    return snapshot;
}

// A single self-consistent range mirroring the transport's own invariants: the
// timeline tick span is the tempo map's image of the sample span, and the
// monotonic span equals the timeline tick span.
TransportRange make_range(const CompiledTempoMap& map, std::uint32_t sample_offset,
                          SamplePosition timeline_sample_start, std::uint32_t frame_count,
                          MonotonicBeat monotonic_start) {
    TransportRange range;
    range.sample_offset = sample_offset;
    range.frame_count = frame_count;
    range.timeline_sample_start = timeline_sample_start;
    range.timeline_tick_start = map.samples_to_ticks(timeline_sample_start);
    const SamplePosition end_sample{timeline_sample_start.value + frame_count};
    range.timeline_tick_end = map.samples_to_ticks(end_sample);
    range.monotonic_start = monotonic_start;
    range.monotonic_end = monotonic_start + (range.timeline_tick_end - range.timeline_tick_start);
    return range;
}

// The monotonic beat corresponding to `n` timeline samples past a range's start.
// Reasoning is via the tick delta from monotonic_start (never absolute monotonic
// == timeline, which do not coincide), so resolve_launch_sample must return the
// range's sample_offset + n exactly.
MonotonicBeat target_after_samples(const CompiledTempoMap& map, const TransportRange& range,
                                   std::int64_t n) {
    const auto end_tick = map.samples_to_ticks({range.timeline_sample_start.value + n});
    return range.monotonic_start + (end_tick - range.timeline_tick_start);
}

} // namespace

TEST_CASE("next_launch_boundary rounds up to the grid at or after the position") {
    const LaunchQuantize beat{TickDuration{kTicksPerQuarter}, TickPosition{0}};

    // Exactly on a boundary resolves to that boundary (ceil, not strictly after).
    REQUIRE(next_launch_boundary({{2 * kTicksPerQuarter}}, beat).position ==
            TickPosition{2 * kTicksPerQuarter});
    // Just past a boundary rounds to the next one.
    REQUIRE(next_launch_boundary({{2 * kTicksPerQuarter + 1}}, beat).position ==
            TickPosition{3 * kTicksPerQuarter});
    // Just before a boundary rounds up to it.
    REQUIRE(next_launch_boundary({{2 * kTicksPerQuarter - 1}}, beat).position ==
            TickPosition{2 * kTicksPerQuarter});

    // Immediate quantization is a no-op.
    REQUIRE(next_launch_boundary({{12'345}}, timeline::launch_immediate()).position ==
            TickPosition{12'345});

    // Phase shifts the whole grid.
    const LaunchQuantize phased{TickDuration{kTicksPerQuarter}, TickPosition{100}};
    REQUIRE(next_launch_boundary({{100}}, phased).position == TickPosition{100});
    REQUIRE(next_launch_boundary({{101}}, phased).position ==
            TickPosition{100 + kTicksPerQuarter});
}

TEST_CASE("launch quantize helpers build the expected grids") {
    REQUIRE(timeline::launch_every_quarters(1).grid == TickDuration{kTicksPerQuarter});
    REQUIRE(timeline::launch_every_quarters(4).grid == TickDuration{4 * kTicksPerQuarter});
    // A bar in 4/4 is four quarter notes.
    REQUIRE(timeline::launch_every_bars(1, {4, 4}).grid == TickDuration{4 * kTicksPerQuarter});
    // A bar in 6/8 is three quarter notes.
    REQUIRE(timeline::launch_every_bars(1, {6, 8}).grid == TickDuration{3 * kTicksPerQuarter});
    REQUIRE(timeline::launch_immediate().immediate());
}

TEST_CASE("resolve_launch_sample maps a mid-block target to an exact offset") {
    const auto map = constant_map();
    TransportSnapshot snapshot;
    snapshot.range_count = 1;
    snapshot.frame_count = 4096;
    snapshot.ranges[0] = make_range(map, 0, {0}, 4096, {{0}});

    // A target 1000 timeline samples into the range resolves to sample offset 1000.
    const auto target = target_after_samples(map, snapshot.ranges[0], 1000);
    const auto offset = resolve_launch_sample(snapshot, map, target);
    REQUIRE(offset.has_value());
    REQUIRE(*offset == 1000);
}

TEST_CASE("resolve_launch_sample returns nullopt when the boundary is beyond the block") {
    const auto map = constant_map();
    TransportSnapshot snapshot;
    snapshot.range_count = 1;
    snapshot.frame_count = 512;
    snapshot.ranges[0] = make_range(map, 0, {0}, 512, {{0}});

    const MonotonicBeat future = snapshot.ranges[0].monotonic_end + TickDuration{kTicksPerQuarter};
    REQUIRE_FALSE(resolve_launch_sample(snapshot, map, future).has_value());
}

TEST_CASE("resolve_launch_sample defers a boundary that rounds onto the block end") {
    const auto map = constant_map();
    TransportSnapshot snapshot;
    snapshot.range_count = 1;
    snapshot.frame_count = 512;
    snapshot.ranges[0] = make_range(map, 0, {0}, 512, {{0}});

    // A resolved offset must always stay inside the block, even for a target in
    // the range's final tick where tick->sample rounding could reach the end
    // sample; the guard defers such a boundary rather than emit an offset ==
    // frame_count. Sweep the last few ticks to cover both roundings.
    const auto span = (snapshot.ranges[0].monotonic_end - snapshot.ranges[0].monotonic_start).value;
    for (std::int64_t back = 1; back <= 4; ++back) {
        const MonotonicBeat target = snapshot.ranges[0].monotonic_start + TickDuration{span - back};
        const auto offset = resolve_launch_sample(snapshot, map, target);
        REQUIRE((!offset.has_value() || *offset < snapshot.frame_count));
    }
}

TEST_CASE("resolve_launch_sample fires immediately when the boundary is already behind") {
    const auto map = constant_map();
    TransportSnapshot snapshot;
    snapshot.range_count = 1;
    snapshot.frame_count = 512;
    snapshot.ranges[0] = make_range(map, 0, {480'000}, 512, {{kTicksPerQuarter}});

    // Target one whole beat before this range began: it was missed, fire now.
    const MonotonicBeat past{{0}};
    const auto offset = resolve_launch_sample(snapshot, map, past);
    REQUIRE(offset.has_value());
    REQUIRE(*offset == 0);
}

// The DoD requirement: a launch whose boundary lands in the post-wrap range of a
// split block must resolve against THAT range's timeline base, sample-accurately.
TEST_CASE("resolve_launch_sample is sample-accurate across a loop wrap") {
    const auto map = constant_map();

    // range0 ends the loop near sample 480000 (one beat = 24000 samples window);
    // range1 restarts at the loop head (sample 0). Monotonic is contiguous.
    TransportSnapshot snapshot;
    snapshot.range_count = 2;
    snapshot.frame_count = 1024;
    snapshot.ranges[0] = make_range(map, 0, {480'000 - 500}, 500, {{7 * kTicksPerQuarter}});
    snapshot.ranges[1] = make_range(map, 500, {0}, 524, snapshot.ranges[0].monotonic_end);
    REQUIRE(snapshot.ranges[1].monotonic_start == snapshot.ranges[0].monotonic_end);

    SECTION("target exactly at the wrap fires at the range boundary") {
        const MonotonicBeat at_wrap = snapshot.ranges[1].monotonic_start;
        const auto offset = resolve_launch_sample(snapshot, map, at_wrap);
        REQUIRE(offset.has_value());
        // The boundary belongs to the post-wrap range (half-open): offset == its start.
        REQUIRE(*offset == 500);
    }

    SECTION("target strictly inside the post-wrap range uses the wrapped base") {
        // 300 timeline samples into range1 (the loop head), so the answer must be
        // relative to range1's own base: sample_offset 500 + 300 == 800. Resolving
        // against range0's pre-wrap base would land elsewhere, so 800 proves the
        // resolver picked the wrapped range.
        const auto target = target_after_samples(map, snapshot.ranges[1], 300);
        const auto offset = resolve_launch_sample(snapshot, map, target);
        REQUIRE(offset.has_value());
        REQUIRE(*offset == 800);
    }

    SECTION("one sample past the wrap is off-by-one accurate") {
        const auto target = target_after_samples(map, snapshot.ranges[1], 1);
        const auto offset = resolve_launch_sample(snapshot, map, target);
        REQUIRE(offset.has_value());
        REQUIRE(*offset == 501);
    }
}

TEST_CASE("LaunchHandle arms, waits for the boundary, then plays") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    // Start just under one beat into playback so the first beat boundary is close.
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_every_quarters(1));
    REQUIRE(handle.state() == LaunchState::Armed);

    // 400-frame block does not reach the boundary at played sample 500: stay armed.
    auto snapshot = block(transport, 400);
    auto event = handle.process(snapshot, map);
    REQUIRE(event.kind == LaunchEventKind::None);
    REQUIRE(handle.state() == LaunchState::Armed);

    // The next block crosses the boundary; it fires and transitions to Playing.
    snapshot = block(transport, 400);
    event = handle.process(snapshot, map);
    REQUIRE(event.kind == LaunchEventKind::Start);
    REQUIRE(handle.state() == LaunchState::Playing);
    // 500 total played samples, 400 already consumed -> offset 100 in this block.
    REQUIRE(event.sample_offset == 100);

    // A Playing handle emits nothing further on its own.
    snapshot = block(transport, 400);
    REQUIRE(handle.process(snapshot, map).kind == LaunchEventKind::None);
}

TEST_CASE("LaunchHandle launch is sample-accurate across a real loop wrap") {
    const auto map = constant_map();
    const LoopRegion loop{true, {0}, {kTicksPerQuarter}}; // one beat = 24000 samples

    auto run_to_launch = [&](std::uint32_t block_frames) -> std::uint64_t {
        auto setup = config();
        setup.initially_playing = true;
        setup.loop = loop;
        setup.initial_position = map.samples_to_ticks({23'500});
        MasterTransport transport;
        REQUIRE(transport.prepare(map, setup) == TransportError::None);

        LaunchHandle handle;
        // Armed before the first block, so the target is anchored identically for
        // every block size: the next beat boundary at played sample 500.
        handle.arm(timeline::launch_every_quarters(1));

        std::uint64_t consumed = 0;
        for (int guard = 0; guard < 10'000; ++guard) {
            const auto snapshot = block(transport, block_frames);
            const auto event = handle.process(snapshot, map);
            if (event.kind == LaunchEventKind::Start)
                return consumed + event.sample_offset;
            consumed += snapshot.frame_count;
        }
        FAIL("launch never fired");
        return 0;
    };

    // Frame-granular run: every block is atomic (no split), so its absolute fire
    // sample is the ground truth. A large-block run splits at the wrap and must
    // resolve to the identical absolute sample.
    const auto truth = run_to_launch(1);
    REQUIRE(truth == 500);
    REQUIRE(run_to_launch(1024) == truth);
    REQUIRE(run_to_launch(333) == truth);
    REQUIRE(run_to_launch(500) == truth);
    // A block boundary landing exactly on the wrap/launch point.
    REQUIRE(run_to_launch(250) == truth);
}

TEST_CASE("LaunchHandle immediate launch fires at the first playing block") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_immediate());
    const auto snapshot = block(transport, 256);
    const auto event = handle.process(snapshot, map);
    REQUIRE(event.kind == LaunchEventKind::Start);
    REQUIRE(event.sample_offset == 0);
    REQUIRE(handle.state() == LaunchState::Playing);
}

TEST_CASE("LaunchHandle armed while stopped waits for playback to begin") {
    const auto map = constant_map();
    auto setup = config(); // initially stopped
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_immediate());

    // While stopped the monotonic clock is frozen: no launch.
    auto snapshot = block(transport, 256);
    REQUIRE(handle.process(snapshot, map).kind == LaunchEventKind::None);
    REQUIRE(handle.state() == LaunchState::Armed);

    REQUIRE(transport.set_playing(true) == TransportError::None);
    snapshot = block(transport, 256);
    const auto event = handle.process(snapshot, map);
    REQUIRE(event.kind == LaunchEventKind::Start);
    REQUIRE(event.sample_offset == 0);
}

TEST_CASE("LaunchHandle stop is quantized to the boundary") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_immediate());
    auto snapshot = block(transport, 100);
    REQUIRE(handle.process(snapshot, map).kind == LaunchEventKind::Start);
    REQUIRE(handle.state() == LaunchState::Playing);

    // Request a stop quantized to the next beat (played sample 500, 100 consumed).
    handle.stop(timeline::launch_every_quarters(1));
    REQUIRE(handle.state() == LaunchState::Stopping);

    snapshot = block(transport, 300); // reaches played sample 400: not yet
    REQUIRE(handle.process(snapshot, map).kind == LaunchEventKind::None);
    REQUIRE(handle.state() == LaunchState::Stopping);

    snapshot = block(transport, 300); // crosses played sample 500
    const auto event = handle.process(snapshot, map);
    REQUIRE(event.kind == LaunchEventKind::Stop);
    REQUIRE(handle.state() == LaunchState::Stopped);
    REQUIRE(event.sample_offset == 100); // 500 - 400 consumed
}

TEST_CASE("LaunchHandle::process does not allocate on the audio thread") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_every_quarters(1));
    const auto snapshot = block(transport, 1024);

    test::ScopedRtProcessProbe probe;
    const auto event = handle.process(snapshot, map);
    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(event.kind == LaunchEventKind::Start);
}
