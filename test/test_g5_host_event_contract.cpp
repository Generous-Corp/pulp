#include <pulp/host/graph_event_contract.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using pulp::host::GraphAuthoredEvent;
using pulp::host::GraphAuthoredEventLane;
using pulp::host::GraphEventResult;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;

namespace pulp::host::detail {
struct GraphEventLaneTestAccess {
    template <std::size_t MaxFrames, std::size_t MaxAcceptedPerSample>
    static void set_cursor(GraphAuthoredEventLane<MaxFrames, MaxAcceptedPerSample>& lane,
                           std::uint64_t cursor) {
        lane.test_set_cursor_(cursor);
    }
};
} // namespace pulp::host::detail

namespace {
HostEventRecord midi_event(std::uint64_t epoch, std::int64_t sample, std::uint64_t sequence,
                           std::uint32_t producer, pulp::midi::MidiEvent event) {
    HostEventRecord record;
    record.epoch = epoch;
    record.absolute_sample = sample;
    record.producer_sequence = sequence;
    record.producer = producer;
    record.midi = event;
    return record;
}

std::vector<GraphAuthoredEvent> render_one(GraphAuthoredEventLane<16>& lane,
                                           const std::array<std::size_t, 16>& partitions,
                                           std::size_t partition_count) {
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    std::vector<GraphAuthoredEvent> events;
    std::size_t absolute = 0;
    for (std::size_t block = 0; block < partition_count; ++block) {
        const auto frames = partitions[block];
        REQUIRE(lane.begin_block(1, absolute, frames) == GraphEventResult::Accepted);
        for (std::size_t offset = 0; offset < frames; ++offset) {
            REQUIRE(lane.begin_sample(offset) == GraphEventResult::Accepted);
            if (absolute == 0 || absolute == 1 || absolute == 5) {
                REQUIRE(lane.emit(1, 7, static_cast<std::uint16_t>(absolute), absolute) ==
                        GraphEventResult::Accepted);
            }
            REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
            for (const auto& event : lane.received())
                events.push_back(event);
            REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
            ++absolute;
        }
        REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    }
    return events;
}
} // namespace

TEST_CASE("G5 host ingress projects absolute MIDI records without retiming",
          "[host][graph][g5][events]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(3, 100, 8) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 102, 2, 11, pulp::midi::MidiEvent::note_on(1, 62, 90))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 100, 1, 11, pulp::midi::MidiEvent::note_on(1, 60, 90))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 110, 3, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::Accepted);

    REQUIRE(ingress.publish(midi_event(3, 99, 4, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::Late);
    REQUIRE(ingress.publish(midi_event(2, 101, 5, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::StaleEpoch);

    std::array<HostEventRecord, 2> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 2);
    REQUIRE(projected[0].absolute_sample == 100);
    REQUIRE(projected[1].absolute_sample == 102);
    REQUIRE(ingress.pending() == 1);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    REQUIRE(ingress.begin_block(3, 108, 8) == HostEventResult::Accepted);
    std::array<HostEventRecord, 2> next{};
    REQUIRE(ingress.project(next, count) == HostEventResult::Accepted);
    REQUIRE(count == 1);
    REQUIRE(next[0].absolute_sample == 110);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 host ingress rejects output-capacity and sequence mutations",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<2> ingress;
    REQUIRE(ingress.begin_block(1, 0, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 0, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 1, 1, 9, pulp::midi::MidiEvent::note_on(0, 61, 1))) ==
            HostEventResult::InvalidSequence);
    REQUIRE(ingress.publish(midi_event(1, 1, 2, 9, pulp::midi::MidiEvent::note_on(0, 61, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 2, 3, 9, pulp::midi::MidiEvent::note_on(0, 62, 1))) ==
            HostEventResult::Overflow);
    std::array<HostEventRecord, 1> too_small{};
    std::size_t count = 0;
    REQUIRE(ingress.project(too_small, count) == HostEventResult::OutputCapacity);
    REQUIRE(count == 0);
    REQUIRE(ingress.pending() == 2);
}

TEST_CASE("G5 host ingress drops staged records at an epoch boundary",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 100, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 110, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.pending() == 1);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    REQUIRE(ingress.begin_block(2, 100, 4) == HostEventResult::Accepted);
    std::array<HostEventRecord, 1> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 0);
    REQUIRE(ingress.pending() == 0);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 host ingress evicts staged records passed by a forward sample jump",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 100, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 110, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    // Seek/loop: the next block starts past the staged record.
    REQUIRE(ingress.begin_block(1, 200, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.pending() == 0);
    // Same (producer, sequence) is reusable and full capacity is available.
    for (std::uint64_t i = 1; i <= 4; ++i)
        REQUIRE(ingress.publish(midi_event(1, 200 + static_cast<std::int64_t>(i) - 1, i, 9,
                                           pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
                HostEventResult::Accepted);
    std::array<HostEventRecord, 4> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 4);
    REQUIRE(ingress.pending() == 0);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 MPE sidecar preserves projected sample offsets", "[host][graph][g5][mpe]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 40, 8) == HostEventResult::Accepted);
    auto note = midi_event(1, 43, 1, 3, pulp::midi::MidiEvent::note_on(1, 60, 100));
    REQUIRE(ingress.publish(note) == HostEventResult::Accepted);
    HostEventRecord bend = note;
    bend.absolute_sample = 45;
    bend.producer_sequence = 2;
    bend.midi = pulp::midi::MidiEvent::pitch_bend(1, 10'000);
    REQUIRE(ingress.publish(bend) == HostEventResult::Accepted);
    std::array<HostEventRecord, 4> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 2);

    pulp::midi::MpeVoiceTracker tracker;
    pulp::midi::MpeBuffer sidecar;
    sidecar.reserve(4);
    sidecar.set_realtime_capacity_limit(true);
    REQUIRE(pulp::host::derive_mpe(std::span<const HostEventRecord>(projected.data(), count), 40,
                                   tracker, sidecar) == HostEventResult::Accepted);
    REQUIRE(sidecar.size() == 2);
    REQUIRE(sidecar[0].sample_offset == 3);
    REQUIRE(sidecar[0].kind == pulp::midi::MpeExpressionEvent::Kind::NoteOn);
    REQUIRE(sidecar[1].sample_offset == 5);
    REQUIRE(sidecar[1].kind == pulp::midi::MpeExpressionEvent::Kind::PitchBend);
}

TEST_CASE("G5 graph-authored event lane is partition invariant", "[host][graph][g5][events]") {
    GraphAuthoredEventLane<16> whole;
    GraphAuthoredEventLane<16> split;
    const std::array<std::size_t, 16> one{8};
    const std::array<std::size_t, 16> parts{1, 1, 3, 1, 2};
    const auto all = render_one(whole, one, 1);
    const auto divided = render_one(split, parts, 5);
    REQUIRE(all == divided);
    REQUIRE(all.size() == 3);
    REQUIRE(all[0].sample == 0);
    REQUIRE(all[1].sample == 1);
    REQUIRE(all[2].sample == 5);
    REQUIRE(all[0].sequence == 0);
}

TEST_CASE("G5 graph-authored event lane has checked refusal and attempt budget",
          "[host][graph][g5][events][negative-control]") {
    GraphAuthoredEventLane<4> lane;
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_block(1, 8, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.emit(2, 7, 1, 8) == GraphEventResult::StaleEpoch);
    REQUIRE(lane.emit(1, 7, 1, 9) == GraphEventResult::Future);
    REQUIRE(lane.emit(1, 7, 1, 7) == GraphEventResult::Late);
    for (std::uint16_t i = 0; i < 8; ++i)
        REQUIRE(lane.emit(1, 7, i, 8) == GraphEventResult::Accepted);
    REQUIRE(lane.emit(1, 7, 99, 8) == GraphEventResult::Overflow);
    // Three refused attempts + eight accepted records + five overflows
    // exhaust the fixed 16-attempt sample budget.
    for (int i = 0; i < 4; ++i)
        REQUIRE(lane.emit(1, 7, 100, 8) == GraphEventResult::Overflow);
    REQUIRE(lane.emit(1, 7, 101, 8) == GraphEventResult::AttemptLimit);
    REQUIRE(lane.counters().attempts == 16);
    REQUIRE(lane.counters().accepted == 8);
    REQUIRE(lane.counters().sequence == 16);
    REQUIRE(lane.counters().overflow == 5);
    REQUIRE(lane.counters().diagnostic_refusals == 3);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.received().size() == 8);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    REQUIRE(lane.reset(2) == GraphEventResult::Accepted);
}

TEST_CASE("G5 graph-authored lane recovers after cursor exhaustion",
          "[host][graph][g5][events][negative-control]") {
    GraphAuthoredEventLane<1> lane;
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    pulp::host::detail::GraphEventLaneTestAccess::set_cursor(
        lane, std::numeric_limits<std::uint64_t>::max());
    REQUIRE(lane.begin_block(1, 0, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::OutOfRange);
    REQUIRE_FALSE(lane.active());
    REQUIRE(lane.reset(2) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_block(2, 0, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    REQUIRE(lane.counters().cursor == 1);
}
