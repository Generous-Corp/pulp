// LV2 timing inputs mapped onto HostBlockTimeline.
//
// LV2 has no monotonic sample counter, so every LV2 block is classified on
// its `time:Position` transport alone. Where the LV2 headers are available the
// event-time helper is driven from real atom sequence and event structs.

#include <pulp/host/host_event_timestamps.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#if __has_include(<lv2/atom/atom.h>)
#include <lv2/atom/atom.h>
#define PULP_TEST_HAS_LV2_HEADERS 1
#endif

using pulp::host::HostBlockBoundary;
using pulp::host::HostBlockClock;
using pulp::host::HostBlockTimeline;
using pulp::host::HostBlockWindow;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;
using pulp::host::Lv2BlockTiming;

namespace {

constexpr std::uint32_t kLv2Producer = 7;
// Stand-ins for host-mapped URIDs; any non-zero values distinct from each
// other behave the same.
constexpr std::uint32_t kFrameTimeUrid = 41;
constexpr std::uint32_t kBeatTimeUrid = 42;

Lv2BlockTiming lv2_block(std::uint32_t frames, std::int64_t position, double speed = 1.0) {
    Lv2BlockTiming t;
    t.sample_count = frames;
    t.has_frame = true;
    t.frame = position;
    t.has_speed = true;
    t.speed = speed;
    return t;
}

HostBlockBoundary step(HostBlockTimeline& timeline, const Lv2BlockTiming& timing,
                       HostBlockWindow& window) {
    REQUIRE(timeline.advance(pulp::host::clock_from_lv2(timing), window) ==
            HostEventResult::Accepted);
    return timeline.last_boundary();
}

std::vector<std::int64_t> drain(HostEventIngress<64>& ingress) {
    std::array<HostEventRecord, 64> out{};
    std::size_t count = 0;
    REQUIRE(ingress.project(out, count) == HostEventResult::Accepted);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
    std::vector<std::int64_t> samples;
    for (std::size_t i = 0; i < count; ++i)
        samples.push_back(out[i].absolute_sample);
    return samples;
}

} // namespace

// ---------------------------------------------------------------------------
// LV2 header correspondence
// ---------------------------------------------------------------------------

#if defined(PULP_TEST_HAS_LV2_HEADERS)
TEST_CASE("LV2 atom event times convert through the real sequence structs", "[graph][g5][lv2]") {
    LV2_Atom_Sequence seq{};
    LV2_Atom_Event ev{};
    ev.time.frames = 17;

    std::uint32_t block_time = 0;
    seq.body.unit = 0; // frames, the default
    CHECK(pulp::host::lv2_event_block_time(seq.body.unit, kFrameTimeUrid, ev.time.frames,
                                           block_time) == HostEventResult::Accepted);
    CHECK(block_time == 17);

    seq.body.unit = kFrameTimeUrid;
    block_time = 0;
    CHECK(pulp::host::lv2_event_block_time(seq.body.unit, kFrameTimeUrid, ev.time.frames,
                                           block_time) == HostEventResult::Accepted);
    CHECK(block_time == 17);

    // A beat-time sequence stores a double in the same union; reading it as
    // frames would place the event at an arbitrary sample.
    seq.body.unit = kBeatTimeUrid;
    ev.time.beats = 2.5;
    block_time = 999;
    CHECK(pulp::host::lv2_event_block_time(seq.body.unit, kFrameTimeUrid, ev.time.frames,
                                           block_time) == HostEventResult::OutOfRange);
    CHECK(block_time == 999);
}
#endif

// ---------------------------------------------------------------------------
// Clock mapping
// ---------------------------------------------------------------------------

TEST_CASE("LV2 timing maps onto a counter-free block clock", "[graph][g5][lv2]") {
    using pulp::host::clock_from_lv2;

    const auto playing = clock_from_lv2(lv2_block(64, 96000));
    CHECK(playing.steady_sample == -1);
    CHECK(playing.frames == 64);
    CHECK(playing.has_transport_sample);
    CHECK(playing.transport_sample == 96000);
    CHECK(playing.transport_playing);
    CHECK_FALSE(playing.discontinuity);

    const auto stopped = clock_from_lv2(lv2_block(64, 96000, 0.0));
    CHECK(stopped.has_transport_sample);
    CHECK_FALSE(stopped.transport_playing);

    // Varispeed and reverse playback are not sample-locked to the block.
    CHECK_FALSE(clock_from_lv2(lv2_block(64, 96000, 0.5)).transport_playing);
    CHECK_FALSE(clock_from_lv2(lv2_block(64, 96000, -1.0)).transport_playing);

    auto no_speed = lv2_block(64, 96000);
    no_speed.has_speed = false;
    CHECK(clock_from_lv2(no_speed).has_transport_sample);
    CHECK_FALSE(clock_from_lv2(no_speed).transport_playing);

    Lv2BlockTiming bare;
    bare.sample_count = 32;
    bare.has_speed = true;
    bare.speed = 1.0;
    const auto unpositioned = clock_from_lv2(bare);
    CHECK(unpositioned.steady_sample == -1);
    CHECK_FALSE(unpositioned.has_transport_sample);
    CHECK_FALSE(unpositioned.transport_playing);

    auto restart = lv2_block(64, 0);
    restart.discontinuity = true;
    CHECK(clock_from_lv2(restart).discontinuity);

    // A zero-length run() maps to zero frames, which advance() refuses.
    HostBlockTimeline timeline;
    HostBlockWindow window;
    CHECK(timeline.advance(clock_from_lv2(lv2_block(0, 0)), window) == HostEventResult::OutOfRange);
}

// ---------------------------------------------------------------------------
// Block classification
// ---------------------------------------------------------------------------

TEST_CASE("LV2 blocks classify contiguous, seek, loop, and stopped", "[graph][g5][lv2]") {
    HostBlockTimeline timeline;
    HostBlockWindow window;

    SECTION("contiguous playing blocks, including a block-size change") {
        CHECK(step(timeline, lv2_block(32, 1000), window) == HostBlockBoundary::First);
        CHECK(step(timeline, lv2_block(64, 1032), window) == HostBlockBoundary::Continuous);
        CHECK(window.epoch == 1);
        CHECK(window.sample_start == 32);
        CHECK(step(timeline, lv2_block(16, 1096), window) == HostBlockBoundary::Continuous);
        CHECK(window.sample_start == 96);
    }

    SECTION("a position advanced by the new block's length is a seek") {
        // The previous block was 32 frames long; a latch advanced by this
        // block's 64 frames lands 32 samples late and must not read as
        // continuous.
        CHECK(step(timeline, lv2_block(32, 1000), window) == HostBlockBoundary::First);
        CHECK(step(timeline, lv2_block(64, 1064), window) == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(window.sample_start == 0);
    }

    SECTION("relocation and loop wrap while playing open a new epoch") {
        CHECK(step(timeline, lv2_block(64, 1000), window) == HostBlockBoundary::First);
        CHECK(step(timeline, lv2_block(64, 50000), window) == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(step(timeline, lv2_block(64, 50064), window) == HostBlockBoundary::Continuous);
        CHECK(step(timeline, lv2_block(64, 0), window) == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 3);
        CHECK(window.sample_start == 0);
    }

    SECTION("a stopped transport is never compared") {
        CHECK(step(timeline, lv2_block(64, 1000, 0.0), window) == HostBlockBoundary::First);
        CHECK(step(timeline, lv2_block(64, 1000, 0.0), window) == HostBlockBoundary::Continuous);
        CHECK(step(timeline, lv2_block(64, 8000, 0.0), window) == HostBlockBoundary::Continuous);
        // Starting playback from the parked position is not a seek either.
        CHECK(step(timeline, lv2_block(64, 8000), window) == HostBlockBoundary::Continuous);
        CHECK(window.epoch == 1);
        CHECK(window.sample_start == 192);
    }

    SECTION("varispeed playback is not compared") {
        CHECK(step(timeline, lv2_block(64, 1000, 0.5), window) == HostBlockBoundary::First);
        CHECK(step(timeline, lv2_block(64, 1032, 0.5), window) == HostBlockBoundary::Continuous);
        CHECK(window.epoch == 1);
    }

    SECTION("no position at all stays contiguous") {
        Lv2BlockTiming bare;
        bare.sample_count = 64;
        CHECK(step(timeline, bare, window) == HostBlockBoundary::First);
        CHECK(step(timeline, bare, window) == HostBlockBoundary::Continuous);
        CHECK(window.sample_start == 64);
    }

    SECTION("an adapter discontinuity opens a new epoch") {
        CHECK(step(timeline, lv2_block(64, 1000), window) == HostBlockBoundary::First);
        auto restarted = lv2_block(64, 1064);
        restarted.discontinuity = true;
        CHECK(step(timeline, restarted, window) == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
    }
}

// ---------------------------------------------------------------------------
// Event times and refusals
// ---------------------------------------------------------------------------

TEST_CASE("LV2 event times are refused, never clamped", "[graph][g5][lv2]") {
    using pulp::host::lv2_event_block_time;

    std::uint32_t t = 999;
    for (const std::int64_t refused : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::min(),
                                       std::int64_t{std::numeric_limits<std::uint32_t>::max()} + 1,
                                       std::numeric_limits<std::int64_t>::max()}) {
        CHECK(lv2_event_block_time(0, kFrameTimeUrid, refused, t) == HostEventResult::OutOfRange);
        CHECK(t == 999);
    }
    // A non-zero unit is only frames when it matches the mapped frameTime URID.
    CHECK(lv2_event_block_time(kBeatTimeUrid, kFrameTimeUrid, 3, t) == HostEventResult::OutOfRange);
    CHECK(lv2_event_block_time(kFrameTimeUrid, 0, 3, t) == HostEventResult::OutOfRange);
    CHECK(t == 999);
    CHECK(lv2_event_block_time(0, 0, 3, t) == HostEventResult::Accepted);
    CHECK(t == 3);

    // An offset past the block is accepted by the conversion and refused by
    // the projection.
    HostBlockTimeline timeline;
    HostBlockWindow window;
    REQUIRE(step(timeline, lv2_block(64, 1000), window) == HostBlockBoundary::First);
    std::int64_t absolute = -1;
    REQUIRE(lv2_event_block_time(0, kFrameTimeUrid, 64, t) == HostEventResult::Accepted);
    CHECK(timeline.project(window, t, absolute) == HostEventResult::OutOfRange);
    REQUIRE(lv2_event_block_time(0, kFrameTimeUrid, 63, t) == HostEventResult::Accepted);
    CHECK(timeline.project(window, t, absolute) == HostEventResult::Accepted);
    CHECK(absolute == 63);

    // After a seek the old window is a retired epoch; after a contiguous block
    // it is late.
    const auto first = window;
    REQUIRE(step(timeline, lv2_block(64, 1064), window) == HostBlockBoundary::Continuous);
    CHECK(timeline.project(first, 0, absolute) == HostEventResult::Late);
    const auto second = window;
    REQUIRE(step(timeline, lv2_block(64, 90000), window) == HostBlockBoundary::NewEpoch);
    CHECK(timeline.project(second, 0, absolute) == HostEventResult::StaleEpoch);
}

// ---------------------------------------------------------------------------
// Partition invariance through the ingress
// ---------------------------------------------------------------------------

TEST_CASE("LV2 absolute samples are invariant to block partitioning", "[graph][g5][lv2]") {
    constexpr std::int64_t kTransportOrigin = 2000;
    constexpr std::int64_t kTotal = 1024;
    const std::vector<std::int64_t> positions{0, 1, 31, 32, 63, 64, 126, 127, 128, 255, 500, 1023};

    auto render = [&](std::size_t block) {
        HostBlockTimeline timeline;
        HostEventIngress<64> ingress;
        ingress.reset(1);
        std::uint64_t sequence = 0;
        std::vector<std::int64_t> projected;
        for (std::int64_t at = 0; at < kTotal;) {
            const auto frames =
                std::min<std::int64_t>(static_cast<std::int64_t>(block), kTotal - at);
            HostBlockWindow window;
            REQUIRE(
                timeline.advance(pulp::host::clock_from_lv2(lv2_block(
                                     static_cast<std::uint32_t>(frames), kTransportOrigin + at)),
                                 window) == HostEventResult::Accepted);
            CHECK(window.epoch == 1);
            REQUIRE(ingress.begin_block(window.epoch, window.sample_start, window.frames) ==
                    HostEventResult::Accepted);
            for (const auto pos : positions) {
                if (pos < at || pos >= at + frames)
                    continue;
                std::uint32_t block_time = 0;
                REQUIRE(pulp::host::lv2_event_block_time(0, kFrameTimeUrid, pos - at, block_time) ==
                        HostEventResult::Accepted);
                HostEventRecord record;
                REQUIRE(timeline.make_record(window, block_time, kLv2Producer, sequence++,
                                             pulp::midi::MidiEvent::note_on(
                                                 0, static_cast<std::uint8_t>(pos % 128), 100),
                                             record) == HostEventResult::Accepted);
                REQUIRE(ingress.publish(record) == HostEventResult::Accepted);
            }
            const auto samples = drain(ingress);
            projected.insert(projected.end(), samples.begin(), samples.end());
            at += frames;
        }
        return projected;
    };

    const auto at32 = render(32);
    CHECK(at32 == positions);
    CHECK(render(64) == at32);
    CHECK(render(127) == at32);
}
