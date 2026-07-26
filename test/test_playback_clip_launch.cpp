#include <pulp/playback/clip_launch.hpp>
#include <pulp/playback/transport.hpp>
#include <pulp/timeline/clip_launch.hpp>

#include "harness/scoped_rt_process_probe.hpp"
#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

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

using timeline::FollowActionKind;
using timeline::FollowDraw;
using timeline::FollowOutcome;
using timeline::Slot;

// A lane of `count` slots with ids 1..count, every slot filled. Slot i's clip id
// is distinct from its slot id so an accidental swap of the two is visible.
std::vector<Slot> make_lane(std::size_t count) {
    std::vector<Slot> lane(count);
    for (std::size_t index = 0; index < count; ++index) {
        lane[index].id = timeline::ItemId{index + 1};
        lane[index].clip_id = timeline::ItemId{100 + index + 1};
    }
    return lane;
}

// The default follow grid used by resolution tests: one beat, fire on the first
// period. Resolution never reads the grid, but a set must be `enabled()` to
// resolve at all.
timeline::FollowActionSet follow(FollowActionKind kind, std::uint32_t repetitions = 1) {
    return timeline::follow_action(kind, timeline::launch_every_quarters(1).grid, repetitions);
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

TEST_CASE("follow action resolves the deterministic navigation kinds") {
    auto lane = make_lane(4);
    const std::span<const Slot> view{lane};

    SECTION("Stop names the acting slot") {
        lane[1].follow = follow(FollowActionKind::Stop);
        const auto resolved = timeline::resolve_follow_action(view, 1, {});
        REQUIRE(resolved.outcome == FollowOutcome::Stop);
        REQUIRE(resolved.slot_index == 1);
        REQUIRE(resolved.slot_id == lane[1].id);
    }

    SECTION("Again replays the acting slot") {
        lane[2].follow = follow(FollowActionKind::Again);
        const auto resolved = timeline::resolve_follow_action(view, 2, {});
        REQUIRE(resolved.outcome == FollowOutcome::Play);
        REQUIRE(resolved.slot_index == 2);
    }

    SECTION("Next advances and wraps past the end") {
        lane[1].follow = follow(FollowActionKind::Next);
        REQUIRE(timeline::resolve_follow_action(view, 1, {}).slot_index == 2);
        lane[3].follow = follow(FollowActionKind::Next);
        REQUIRE(timeline::resolve_follow_action(view, 3, {}).slot_index == 0);
    }

    SECTION("Previous retreats and wraps past the start") {
        lane[2].follow = follow(FollowActionKind::Previous);
        REQUIRE(timeline::resolve_follow_action(view, 2, {}).slot_index == 1);
        lane[0].follow = follow(FollowActionKind::Previous);
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).slot_index == 3);
    }

    SECTION("First and Last address the ends") {
        lane[2].follow = follow(FollowActionKind::First);
        REQUIRE(timeline::resolve_follow_action(view, 2, {}).slot_index == 0);
        lane[1].follow = follow(FollowActionKind::Last);
        REQUIRE(timeline::resolve_follow_action(view, 1, {}).slot_index == 3);
    }

    SECTION("Jump addresses a slot by id") {
        auto set = follow(FollowActionKind::Jump);
        set.choices[0].target = lane[3].id;
        lane[0].follow = set;
        const auto resolved = timeline::resolve_follow_action(view, 0, {});
        REQUIRE(resolved.outcome == FollowOutcome::Play);
        REQUIRE(resolved.slot_index == 3);
        REQUIRE(resolved.slot_id == lane[3].id);
    }
}

TEST_CASE("follow action skips empty slots and stays inert when unsatisfiable") {
    auto lane = make_lane(4);
    lane[1].clip_id = {}; // empty
    lane[2].clip_id = {}; // empty
    const std::span<const Slot> view{lane};

    SECTION("Next and Previous step over the empty slots") {
        lane[0].follow = follow(FollowActionKind::Next);
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).slot_index == 3);
        lane[3].follow = follow(FollowActionKind::Previous);
        REQUIRE(timeline::resolve_follow_action(view, 3, {}).slot_index == 0);
    }

    SECTION("Last skips a trailing empty slot") {
        auto tail = make_lane(3);
        tail[2].clip_id = {};
        tail[0].follow = follow(FollowActionKind::Last);
        REQUIRE(timeline::resolve_follow_action(tail, 0, {}).slot_index == 1);
    }

    SECTION("a jump to an unknown id is inert rather than a stop") {
        auto set = follow(FollowActionKind::Jump);
        set.choices[0].target = timeline::ItemId{9'999};
        lane[0].follow = set;
        const auto resolved = timeline::resolve_follow_action(view, 0, {});
        REQUIRE(resolved.outcome == FollowOutcome::None);
        REQUIRE(resolved.slot_index == timeline::FollowResolution::kNoSlot);
    }

    SECTION("a jump to an empty slot is inert") {
        auto set = follow(FollowActionKind::Jump);
        set.choices[0].target = lane[1].id;
        lane[0].follow = set;
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).outcome == FollowOutcome::None);
    }

    SECTION("navigation in a lane with no filled slot is inert") {
        std::vector<Slot> empty_lane(3);
        empty_lane[0].id = timeline::ItemId{1};
        empty_lane[0].follow = follow(FollowActionKind::Next);
        REQUIRE(timeline::resolve_follow_action(empty_lane, 0, {}).outcome == FollowOutcome::None);
    }

    SECTION("a disabled set never resolves") {
        // A zero period, no candidates, and zero repetitions each disable it.
        lane[0].follow = timeline::follow_action(FollowActionKind::Next, TickDuration{0});
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).outcome == FollowOutcome::None);
        lane[0].follow = follow(FollowActionKind::Next, 0);
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).outcome == FollowOutcome::None);
        lane[0].follow = follow(FollowActionKind::Next);
        lane[0].follow.choice_count = 0;
        REQUIRE(timeline::resolve_follow_action(view, 0, {}).outcome == FollowOutcome::None);
    }

    SECTION("an out-of-range acting index is inert") {
        REQUIRE(timeline::resolve_follow_action(view, 99, {}).outcome == FollowOutcome::None);
    }
}

TEST_CASE("random follow actions are seeded and reproducible") {
    auto lane = make_lane(5);
    lane[0].follow = follow(FollowActionKind::Any);
    lane[1].follow = follow(FollowActionKind::Other);
    const std::span<const Slot> view{lane};

    constexpr std::uint64_t kSeed = 0xC0FFEE'1234ULL;

    // The same (seed, slot, draw index) triple always resolves the same way, and
    // a replay of the whole decision sequence is identical — the determinism
    // contract for a random follow action.
    std::vector<std::size_t> first_pass;
    std::vector<std::size_t> replay;
    for (std::uint64_t draw = 0; draw < 64; ++draw) {
        first_pass.push_back(
            timeline::resolve_follow_action(view, 0, {kSeed, lane[0].id, draw}).slot_index);
        replay.push_back(
            timeline::resolve_follow_action(view, 0, {kSeed, lane[0].id, draw}).slot_index);
    }
    REQUIRE(first_pass == replay);

    SECTION("Any reaches every filled slot including the acting one") {
        std::array<int, 5> hits{};
        for (std::size_t index : first_pass)
            hits[index]++;
        for (int count : hits)
            REQUIRE(count > 0);
    }

    SECTION("Other never picks the acting slot") {
        for (std::uint64_t draw = 0; draw < 256; ++draw) {
            const auto resolved =
                timeline::resolve_follow_action(view, 1, {kSeed, lane[1].id, draw});
            REQUIRE(resolved.outcome == FollowOutcome::Play);
            REQUIRE(resolved.slot_index != 1);
        }
    }

    SECTION("Other in a single-filled-slot lane replays that slot") {
        auto solo = make_lane(3);
        solo[1].clip_id = {};
        solo[2].clip_id = {};
        solo[0].follow = follow(FollowActionKind::Other);
        const auto resolved = timeline::resolve_follow_action(solo, 0, {kSeed, solo[0].id, 7});
        REQUIRE(resolved.outcome == FollowOutcome::Play);
        REQUIRE(resolved.slot_index == 0);
    }

    SECTION("a different seed produces a different sequence") {
        std::vector<std::size_t> other_seed;
        for (std::uint64_t draw = 0; draw < 64; ++draw)
            other_seed.push_back(
                timeline::resolve_follow_action(view, 0, {kSeed + 1, lane[0].id, draw})
                    .slot_index);
        REQUIRE(other_seed != first_pass);
    }

    SECTION("two slots drawing at the same index do not share a stream") {
        // Each decision hashes its own slot id, so no slot's draw depends on how
        // many other slots resolved first — evaluation order cannot change it.
        std::vector<std::size_t> from_slot_0;
        std::vector<std::size_t> from_slot_3;
        lane[3].follow = follow(FollowActionKind::Any);
        for (std::uint64_t draw = 0; draw < 64; ++draw) {
            from_slot_0.push_back(
                timeline::resolve_follow_action(view, 0, {kSeed, lane[0].id, draw}).slot_index);
            from_slot_3.push_back(
                timeline::resolve_follow_action(view, 3, {kSeed, lane[3].id, draw}).slot_index);
        }
        REQUIRE(from_slot_0 != from_slot_3);
    }
}

TEST_CASE("weighted follow-action candidates are drawn in proportion") {
    auto lane = make_lane(3);
    timeline::FollowActionSet set;
    set.grid = timeline::launch_every_quarters(1).grid;
    set.choices[0] = {FollowActionKind::Again, {}, 3};
    set.choices[1] = {FollowActionKind::Next, {}, 1};
    set.choice_count = 2;
    lane[0].follow = set;
    const std::span<const Slot> view{lane};

    int again = 0;
    int next = 0;
    constexpr int kDraws = 4'000;
    for (std::uint64_t draw = 0; draw < kDraws; ++draw) {
        const auto resolved = timeline::resolve_follow_action(view, 0, {7, lane[0].id, draw});
        REQUIRE(resolved.outcome == FollowOutcome::Play);
        (resolved.slot_index == 0 ? again : next)++;
    }
    REQUIRE(again + next == kDraws);
    // 3:1 weighting: the heavier candidate takes roughly three quarters. A wide
    // band keeps this an assertion about the weighting, not about the hash.
    REQUIRE(again > next * 2);
    REQUIRE(again < next * 4);

    SECTION("a zero weight makes a candidate unreachable") {
        lane[0].follow.choices[1].weight = 0;
        for (std::uint64_t draw = 0; draw < 256; ++draw)
            REQUIRE(timeline::resolve_follow_action(view, 0, {7, lane[0].id, draw}).slot_index ==
                    0);
    }

    SECTION("an all-zero-weight set is inert") {
        lane[0].follow.choices[0].weight = 0;
        lane[0].follow.choices[1].weight = 0;
        REQUIRE(timeline::resolve_follow_action(view, 0, {7, lane[0].id, 0}).outcome ==
                FollowOutcome::None);
    }
}

TEST_CASE("FollowActionTimer fires one grid period after the launch it follows") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_every_quarters(1));
    FollowActionTimer timer;

    std::uint64_t consumed = 0;
    std::uint64_t launched_at = 0;
    std::uint64_t followed_at = 0;
    for (int guard = 0; guard < 1'000 && followed_at == 0; ++guard) {
        const auto snapshot = block(transport, 1024);
        const auto event = handle.process(snapshot, map);
        if (event.kind == LaunchEventKind::Start) {
            launched_at = consumed + event.sample_offset;
            REQUIRE(handle.has_last_start());
            // The grid is anchored to the beat the launch resolved to, not to the
            // block that carried it.
            REQUIRE(handle.last_start().position == TickPosition{kTicksPerQuarter});
            timer.arm(handle.last_start(), follow(FollowActionKind::Next));
            REQUIRE(timer.armed());
        }
        const auto follow_event = timer.process(snapshot, map);
        if (follow_event.fired)
            followed_at = consumed + follow_event.sample_offset;
        consumed += snapshot.frame_count;
    }

    REQUIRE(launched_at == 500);
    // One beat at 120 bpm / 48 kHz is 24000 samples past the launch.
    REQUIRE(followed_at == 24'500);
    REQUIRE_FALSE(timer.armed());
    REQUIRE(timer.fire_count() == 1);
}

TEST_CASE("FollowActionTimer grid is anchored to the launch, not the transport grid") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    // An off-grid start: the monotonic clock begins 23500 samples in, which is
    // not a whole beat, and an immediate launch inherits that off-grid position.
    setup.initial_position = map.samples_to_ticks({23'500});
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    LaunchHandle handle;
    handle.arm(timeline::launch_immediate());
    FollowActionTimer timer;

    std::uint64_t consumed = 0;
    std::uint64_t followed_at = 0;
    for (int guard = 0; guard < 1'000 && followed_at == 0; ++guard) {
        const auto snapshot = block(transport, 1024);
        if (handle.process(snapshot, map).kind == LaunchEventKind::Start)
            timer.arm(handle.last_start(), follow(FollowActionKind::Next));
        const auto follow_event = timer.process(snapshot, map);
        if (follow_event.fired)
            followed_at = consumed + follow_event.sample_offset;
        consumed += snapshot.frame_count;
    }

    // A full beat after the off-grid launch. A grid anchored to the monotonic
    // origin instead would have fired at played sample 500 — the transport's own
    // next beat — so this number distinguishes the two anchorings.
    REQUIRE(followed_at == 24'000);
}

TEST_CASE("FollowActionTimer counts repetitions before it fires") {
    const auto map = constant_map();
    auto setup = config();
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    FollowActionTimer timer;
    timer.arm({{0}}, follow(FollowActionKind::Next, 3));

    std::uint64_t consumed = 0;
    std::uint64_t fired_at = 0;
    for (int guard = 0; guard < 1'000 && fired_at == 0; ++guard) {
        const auto snapshot = block(transport, 512);
        const auto event = timer.process(snapshot, map);
        if (event.fired)
            fired_at = consumed + event.sample_offset;
        consumed += snapshot.frame_count;
    }
    // Three one-beat periods: the first two boundaries are consumed silently.
    REQUIRE(fired_at == 3 * 24'000);
    REQUIRE(timer.fire_count() == 1);

    SECTION("a disabled set leaves the timer disarmed") {
        FollowActionTimer inert;
        inert.arm({{0}}, timeline::follow_action(FollowActionKind::Next, TickDuration{0}));
        REQUIRE_FALSE(inert.armed());
        const auto snapshot = block(transport, 4096);
        REQUIRE_FALSE(inert.process(snapshot, map).fired);
    }

    SECTION("repetition boundaries falling inside one block are consumed there") {
        // A block long enough to span all three one-beat periods must still fire
        // exactly once, at the third boundary.
        auto wide_setup = config(1 << 17);
        wide_setup.initially_playing = true;
        MasterTransport wide;
        REQUIRE(wide.prepare(map, wide_setup) == TransportError::None);
        FollowActionTimer bulk;
        bulk.arm({{0}}, follow(FollowActionKind::Next, 3));
        const auto snapshot = block(wide, 100'000);
        const auto event = bulk.process(snapshot, map);
        REQUIRE(event.fired);
        REQUIRE(event.sample_offset == 3 * 24'000);
        REQUIRE_FALSE(bulk.armed());
    }
}

TEST_CASE("FollowActionTimer fire is sample-accurate across a real loop wrap") {
    const auto map = constant_map();
    // A three-beat loop: the follow action's intermediate boundaries and its fire
    // land on different sides of the wrap.
    const LoopRegion loop{true, {0}, {3 * kTicksPerQuarter}};

    auto run_to_follow = [&](std::uint32_t block_frames) -> std::uint64_t {
        auto setup = config();
        setup.initially_playing = true;
        setup.loop = loop;
        setup.initial_position = map.samples_to_ticks({23'500});
        MasterTransport transport;
        REQUIRE(transport.prepare(map, setup) == TransportError::None);

        LaunchHandle handle;
        // Launching immediately from an off-grid position puts the follow grid
        // off the transport's own beat grid, so the fire sample also proves the
        // grid stayed anchored to the launch across the wrap.
        handle.arm(timeline::launch_immediate());
        FollowActionTimer timer;

        std::uint64_t consumed = 0;
        for (int guard = 0; guard < 400'000; ++guard) {
            const auto snapshot = block(transport, block_frames);
            const auto event = handle.process(snapshot, map);
            if (event.kind == LaunchEventKind::Start)
                timer.arm(handle.last_start(), follow(FollowActionKind::Next, 3));
            const auto follow_event = timer.process(snapshot, map);
            if (follow_event.fired)
                return consumed + follow_event.sample_offset;
            consumed += snapshot.frame_count;
        }
        FAIL("follow action never fired");
        return 0;
    };

    // Frame-granular blocks never split, so their absolute fire sample is ground
    // truth; every larger block size splits differently at the wrap and must
    // agree exactly. The launch lands at played sample 0, and the third one-beat
    // period is 72000 samples later — one wrap (at played sample 48500) behind.
    const auto truth = run_to_follow(1);
    REQUIRE(truth == 72'000);
    REQUIRE(run_to_follow(1024) == truth);
    REQUIRE(run_to_follow(333) == truth);
    REQUIRE(run_to_follow(500) == truth);
    // A block boundary landing exactly on the loop wrap.
    REQUIRE(run_to_follow(250) == truth);
}

TEST_CASE("follow-action resolution does not allocate on the audio thread") {
    const auto map = constant_map();
    // One block long enough to contain the whole one-beat follow period, so the
    // fire and its resolution both happen inside the probe.
    auto setup = config(32'768);
    setup.initially_playing = true;
    MasterTransport transport;
    REQUIRE(transport.prepare(map, setup) == TransportError::None);

    auto lane = make_lane(5);
    lane[0].follow = follow(FollowActionKind::Any);
    const std::span<const Slot> view{lane};

    FollowActionTimer timer;
    timer.arm({{0}}, lane[0].follow);
    const auto snapshot = block(transport, 32'768);

    timeline::FollowResolution resolved;
    {
        test::ScopedRtProcessProbe probe;
        const auto event = timer.process(snapshot, map);
        if (event.fired)
            resolved = timeline::resolve_follow_action(view, 0, {42, lane[0].id,
                                                                 timer.fire_count()});
        REQUIRE(probe.allocation_count() == 0);
        REQUIRE(event.fired);
    }
    REQUIRE(resolved.outcome == FollowOutcome::Play);
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
