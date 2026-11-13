#include <pulp/host/host_event_timestamps.hpp>

#include <clap/events.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using pulp::host::HostBlockBoundary;
using pulp::host::HostBlockClock;
using pulp::host::HostBlockTimeline;
using pulp::host::HostBlockWindow;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;

namespace {

constexpr std::uint32_t kClapProducer = 3;

clap_event_note_t clap_note_on(std::uint32_t time, std::int16_t key) {
    clap_event_note_t ev{};
    ev.header.size = sizeof(clap_event_note_t);
    ev.header.time = time;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_NOTE_ON;
    ev.header.flags = 0;
    ev.note_id = -1;
    ev.port_index = 0;
    ev.channel = 0;
    ev.key = key;
    ev.velocity = 1.0;
    return ev;
}

pulp::midi::MidiEvent to_midi(const clap_event_note_t& ev) {
    return pulp::midi::MidiEvent::note_on(static_cast<std::uint8_t>(ev.channel),
                                          static_cast<std::uint8_t>(ev.key),
                                          static_cast<std::uint8_t>(ev.velocity * 127.0));
}

HostBlockClock steady_block(std::int64_t steady, std::size_t frames) {
    HostBlockClock clock;
    clock.steady_sample = steady;
    clock.frames = frames;
    return clock;
}

HostBlockClock playing_block(std::int64_t steady, std::size_t frames, std::int64_t position) {
    auto clock = steady_block(steady, frames);
    clock.has_transport_sample = true;
    clock.transport_sample = position;
    clock.transport_playing = true;
    return clock;
}

// Drive one host block through the timeline and the ingress, publishing
// CLAP note-on events, and return the absolute samples the ingress projects.
std::vector<std::int64_t> run_block(HostBlockTimeline& timeline, HostEventIngress<64>& ingress,
                                    const HostBlockClock& clock,
                                    const std::vector<clap_event_note_t>& events,
                                    std::uint64_t& sequence) {
    HostBlockWindow window;
    REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
    REQUIRE(ingress.begin_block(window.epoch, window.sample_start, window.frames) ==
            HostEventResult::Accepted);
    for (const auto& ev : events) {
        HostEventRecord record;
        REQUIRE(timeline.make_record(window, ev.header.time, kClapProducer, sequence++, to_midi(ev),
                                     record) == HostEventResult::Accepted);
        REQUIRE(ingress.publish(record) == HostEventResult::Accepted);
    }
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

TEST_CASE("CLAP block times project to contiguous absolute samples", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostEventIngress<64> ingress;
    ingress.reset(1);
    std::uint64_t sequence = 0;

    const auto first = run_block(timeline, ingress, steady_block(48000, 64),
                                 {clap_note_on(0, 60), clap_note_on(63, 62)}, sequence);
    CHECK(timeline.last_boundary() == HostBlockBoundary::First);
    CHECK(timeline.current().epoch == 1);
    CHECK(timeline.current().sample_start == 0);
    CHECK(first == std::vector<std::int64_t>{0, 63});

    const auto second =
        run_block(timeline, ingress, steady_block(48064, 64), {clap_note_on(5, 64)}, sequence);
    CHECK(timeline.last_boundary() == HostBlockBoundary::Continuous);
    CHECK(timeline.current().epoch == 1);
    CHECK(timeline.current().sample_start == 64);
    CHECK(second == std::vector<std::int64_t>{69});

    HostEventRecord record;
    REQUIRE(timeline.make_record(timeline.current(), 7, kClapProducer, 99,
                                 to_midi(clap_note_on(7, 65)),
                                 record) == HostEventResult::Accepted);
    CHECK(record.epoch == 1);
    CHECK(record.absolute_sample == 71);
    CHECK(record.midi.sample_offset == 7);
}

TEST_CASE("CLAP steady_time is consumed as a sample counter", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostBlockWindow window;

    // CLAP 1.2.10 defines steady_time as the sample counter at block start.
    // Use a nonzero host value so a wall-clock interpretation cannot pass by
    // accident; a 128-frame block advances the counter by exactly 128.
    REQUIRE(timeline.advance(steady_block(12'345'600, 128), window) == HostEventResult::Accepted);
    REQUIRE(timeline.advance(steady_block(12'345'728, 128), window) == HostEventResult::Accepted);
    CHECK(timeline.last_boundary() == HostBlockBoundary::Continuous);
    CHECK(window.sample_start == 128);

    std::int64_t absolute = -1;
    CHECK(timeline.project(window, 127, absolute) == HostEventResult::Accepted);
    CHECK(absolute == 255);
}

TEST_CASE("Hosts without a steady clock still advance contiguously", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostBlockWindow window;
    REQUIRE(timeline.advance(steady_block(-1, 32), window) == HostEventResult::Accepted);
    REQUIRE(timeline.advance(steady_block(-1, 32), window) == HostEventResult::Accepted);
    CHECK(timeline.last_boundary() == HostBlockBoundary::Continuous);
    CHECK(window.sample_start == 32);
    // Gaining a steady clock mid-stream cannot be related to the counted one.
    REQUIRE(timeline.advance(steady_block(1000, 32), window) == HostEventResult::Accepted);
    CHECK(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
    CHECK(window.epoch == 2);
    CHECK(window.sample_start == 0);
}

TEST_CASE("Seek and loop discontinuities open a new epoch", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostEventIngress<64> ingress;
    ingress.reset(1);
    HostBlockWindow window;

    SECTION("a loop wrap in the transport position") {
        REQUIRE(timeline.advance(playing_block(0, 64, 1000), window) == HostEventResult::Accepted);
        REQUIRE(timeline.advance(playing_block(64, 64, 1064), window) == HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::Continuous);
        const auto old_window = window;
        // Steady clock continues; the transport wrapped back to the loop start.
        REQUIRE(timeline.advance(playing_block(128, 64, 1000), window) ==
                HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(window.sample_start == 0);

        // A record still timed against the retired epoch is refused by both
        // the projection and the ingress contract.
        HostEventRecord stale;
        CHECK(timeline.make_record(old_window, 3, kClapProducer, 1, to_midi(clap_note_on(3, 60)),
                                   stale) == HostEventResult::StaleEpoch);
        stale.epoch = old_window.epoch;
        stale.absolute_sample = static_cast<std::int64_t>(old_window.sample_start + 3);
        stale.producer = kClapProducer;
        REQUIRE(ingress.begin_block(window.epoch, window.sample_start, window.frames) ==
                HostEventResult::Accepted);
        CHECK(ingress.publish(stale) == HostEventResult::StaleEpoch);
        REQUIRE(ingress.end_block() == HostEventResult::Accepted);
    }

    SECTION("an explicit host discontinuity") {
        REQUIRE(timeline.advance(steady_block(0, 64), window) == HostEventResult::Accepted);
        auto clock = steady_block(64, 64);
        clock.discontinuity = true;
        REQUIRE(timeline.advance(clock, window) == HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
        CHECK(window.sample_start == 0);
    }

    SECTION("a backwards steady clock") {
        REQUIRE(timeline.advance(steady_block(500, 64), window) == HostEventResult::Accepted);
        REQUIRE(timeline.advance(steady_block(520, 64), window) == HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::NewEpoch);
        CHECK(window.epoch == 2);
    }

    SECTION("a forward steady gap stays in the epoch and advances by the gap") {
        REQUIRE(timeline.advance(steady_block(500, 64), window) == HostEventResult::Accepted);
        REQUIRE(timeline.advance(steady_block(700, 64), window) == HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::Gap);
        CHECK(window.epoch == 1);
        CHECK(window.sample_start == 200);
        // A transport that only starts playing is not a seek by itself.
        REQUIRE(timeline.advance(playing_block(764, 64, 9000), window) ==
                HostEventResult::Accepted);
        CHECK(timeline.last_boundary() == HostBlockBoundary::Continuous);
        CHECK(window.sample_start == 264);
    }
}

TEST_CASE("Event times outside the block are refused, never clamped", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostBlockWindow window;
    REQUIRE(timeline.advance(steady_block(0, 64), window) == HostEventResult::Accepted);

    for (const std::uint32_t time : {64u, 65u, std::numeric_limits<std::uint32_t>::max()}) {
        const auto ev = clap_note_on(time, 60);
        std::int64_t absolute = -7;
        CHECK(timeline.project(window, ev.header.time, absolute) == HostEventResult::OutOfRange);
        CHECK(absolute == -7);
        HostEventRecord record;
        record.absolute_sample = -11;
        CHECK(timeline.make_record(window, ev.header.time, kClapProducer, 1, to_midi(ev), record) ==
              HostEventResult::OutOfRange);
        CHECK(record.absolute_sample == -11);
    }

    std::int64_t last = 0;
    CHECK(timeline.project(window, 63, last) == HostEventResult::Accepted);
    CHECK(last == 63);

    // Block sizes outside the ingress window bound are refused without moving
    // the timeline.
    HostBlockWindow untouched{};
    CHECK(timeline.advance(steady_block(64, 0), untouched) == HostEventResult::OutOfRange);
    CHECK(timeline.advance(steady_block(64, pulp::host::kHostBlockMaxFrames + 1), untouched) ==
          HostEventResult::OutOfRange);
    CHECK(untouched.epoch == 0);
    CHECK(timeline.current().sample_start == 0);
}

TEST_CASE("Events projected against a previous block are late", "[graph][g5][clap]") {
    HostBlockTimeline timeline;
    HostEventIngress<64> ingress;
    ingress.reset(1);
    HostBlockWindow first;
    HostBlockWindow second;
    REQUIRE(timeline.advance(steady_block(0, 64), first) == HostEventResult::Accepted);
    REQUIRE(timeline.advance(steady_block(64, 64), second) == HostEventResult::Accepted);

    std::int64_t absolute = -1;
    CHECK(timeline.project(first, 10, absolute) == HostEventResult::Late);
    CHECK(absolute == -1);

    // The ingress enforces the same boundary for a record built from the
    // previous window's coordinates.
    HostEventRecord late;
    late.epoch = first.epoch;
    late.absolute_sample = static_cast<std::int64_t>(first.sample_start + 10);
    late.producer = kClapProducer;
    REQUIRE(ingress.begin_block(second.epoch, second.sample_start, second.frames) ==
            HostEventResult::Accepted);
    CHECK(ingress.publish(late) == HostEventResult::Late);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    // A window that was never issued is refused rather than trusted.
    auto forged = second;
    forged.sample_start += 64;
    CHECK(timeline.project(forged, 0, absolute) == HostEventResult::OutOfRange);
}

TEST_CASE("Absolute samples are invariant to host block partitioning", "[graph][g5][clap]") {
    constexpr std::int64_t kOrigin = 96000;
    constexpr std::int64_t kTotal = 1024;
    const std::vector<std::int64_t> event_positions{0,   1,   31,  32,  63,  64,  126,
                                                    127, 128, 255, 500, 777, 1023};

    auto render = [&](std::size_t block) {
        HostBlockTimeline timeline;
        HostEventIngress<64> ingress;
        ingress.reset(1);
        std::uint64_t sequence = 0;
        std::vector<std::int64_t> projected;
        for (std::int64_t steady = 0; steady < kTotal;) {
            const auto frames = static_cast<std::size_t>(
                std::min<std::int64_t>(static_cast<std::int64_t>(block), kTotal - steady));
            std::vector<clap_event_note_t> events;
            for (const auto pos : event_positions) {
                if (pos >= steady && pos < steady + static_cast<std::int64_t>(frames))
                    events.push_back(clap_note_on(static_cast<std::uint32_t>(pos - steady),
                                                  static_cast<std::int16_t>(pos % 128)));
            }
            const auto samples = run_block(
                timeline, ingress, steady_block(kOrigin + steady, frames), events, sequence);
            CHECK(timeline.current().epoch == 1);
            projected.insert(projected.end(), samples.begin(), samples.end());
            steady += static_cast<std::int64_t>(frames);
        }
        return projected;
    };

    const auto at32 = render(32);
    CHECK(at32 == event_positions);
    CHECK(render(64) == at32);
    CHECK(render(127) == at32);
}
