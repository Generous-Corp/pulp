#include <pulp/host/graph_event_contract.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

using pulp::host::GraphAuthoredEvent;
using pulp::host::GraphAuthoredEventLane;
using pulp::host::GraphEventResult;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;

int checks = 0;

void check(bool condition, const char* rule) {
    ++checks;
    if (!condition)
        throw std::runtime_error(rule);
}

HostEventRecord record(std::uint64_t epoch, std::int64_t sample,
                       std::uint64_t sequence, std::uint32_t producer,
                       pulp::midi::MidiEvent event) {
    HostEventRecord value;
    value.epoch = epoch;
    value.absolute_sample = sample;
    value.producer_sequence = sequence;
    value.producer = producer;
    value.midi = event;
    return value;
}

void host_ingress_controls() {
    HostEventIngress<8> ingress;
    check(ingress.begin_block(7, 100, 4) == HostEventResult::Accepted,
          "host-begin-block");

    using pulp::midi::MidiEvent;
    check(ingress.publish(record(7, 100, 1, 2, MidiEvent::note_on(1, 60, 100))) ==
              HostEventResult::Accepted,
          "host-note-on");
    auto bend = record(7, 102, 2, 2, MidiEvent::pitch_bend(1, 10'000));
    check(ingress.publish(bend) == HostEventResult::Accepted, "host-pitch-bend");
    check(ingress.publish(record(7, 100, 1, 1, MidiEvent::note_on(2, 61, 90))) ==
              HostEventResult::Accepted,
          "host-tie-from-second-producer");
    check(ingress.publish(record(7, 99, 3, 2, MidiEvent::note_off(1, 60))) ==
              HostEventResult::Late,
          "host-late-negative");
    check(ingress.publish(record(6, 101, 4, 2, MidiEvent::note_off(1, 60))) ==
              HostEventResult::StaleEpoch,
          "host-stale-epoch-negative");
    check(ingress.publish(record(7, 101, 1, 2, MidiEvent::note_off(1, 60))) ==
              HostEventResult::InvalidSequence,
          "host-duplicate-sequence-negative");
    check(ingress.publish(record(7, 104, 3, 2, MidiEvent::note_off(1, 60))) ==
              HostEventResult::Accepted,
          "host-future-staging");

    std::array<HostEventRecord, 1> too_small{};
    std::size_t count = 0;
    check(ingress.project(too_small, count) == HostEventResult::OutputCapacity,
          "host-output-capacity-negative");
    check(count == 0 && ingress.pending() == 4, "host-transactional-refusal");

    std::array<HostEventRecord, 4> projected{};
    check(ingress.project(projected, count) == HostEventResult::Accepted && count == 3,
          "host-project-count");
    check(projected[0].absolute_sample == 100 && projected[0].producer == 1,
          "host-tie-producer-order");
    check(projected[1].absolute_sample == 100 && projected[1].producer == 2,
          "host-tie-sequence-order");
    check(projected[2].absolute_sample == 102 && projected[2].producer_sequence == 2,
          "host-sample-order");

    pulp::midi::MpeVoiceTracker tracker;
    pulp::midi::MpeBuffer sidecar;
    sidecar.reserve(8);
    sidecar.set_realtime_capacity_limit(true);
    check(pulp::host::derive_mpe(std::span<const HostEventRecord>(projected.data(), count),
                                 100, tracker, sidecar) == HostEventResult::Accepted,
          "host-mpe-derive");
    check(sidecar.size() == 3 && sidecar[0].sample_offset == 0 &&
              sidecar[1].sample_offset == 0 && sidecar[2].sample_offset == 2,
          "host-mpe-sample-offsets");

    check(ingress.end_block() == HostEventResult::Accepted, "host-end-block");
    check(ingress.begin_block(7, 104, 1) == HostEventResult::Accepted,
          "host-future-block");
    std::array<HostEventRecord, 1> future{};
    check(ingress.project(future, count) == HostEventResult::Accepted && count == 1 &&
              future[0].absolute_sample == 104,
          "host-future-delivery");
    check(ingress.end_block() == HostEventResult::Accepted, "host-future-end");
}

struct Observation {
    std::vector<GraphAuthoredEvent> events;
    std::array<std::uint8_t, 2> attempts{};
    std::array<std::uint8_t, 2> accepted{};
    std::uint64_t overflow = 0;
    std::uint64_t diagnostic_refusals = 0;
    std::uint64_t sequence = 0;
};

Observation render(std::span<const std::size_t> partitions) {
    GraphAuthoredEventLane<16> lane;
    check(lane.reset(1) == GraphEventResult::Accepted, "graph-reset");
    Observation observation;
    std::uint64_t absolute = 0;
    std::size_t sample_index = 0;
    for (const auto frames : partitions) {
        check(lane.begin_block(1, absolute, frames) == GraphEventResult::Accepted,
              "graph-begin-block");
        for (std::size_t offset = 0; offset < frames; ++offset, ++sample_index) {
            check(lane.begin_sample(offset) == GraphEventResult::Accepted,
                  "graph-begin-sample");
            if (sample_index == 0) {
                check(lane.emit(2, 9, 1, absolute) == GraphEventResult::StaleEpoch,
                      "graph-stale-negative");
                for (std::uint16_t payload = 0; payload < 8; ++payload)
                    check(lane.emit(1, 9, payload, absolute) == GraphEventResult::Accepted,
                          "graph-accepted-event");
                for (std::uint16_t payload = 8; payload < 15; ++payload)
                    check(lane.emit(1, 9, payload, absolute) == GraphEventResult::Overflow,
                          "graph-overflow-negative");
                check(lane.emit(1, 9, 99, absolute) == GraphEventResult::AttemptLimit,
                      "graph-attempt-limit-negative");
            } else if (sample_index == 1) {
                check(lane.emit(1, 9, 16, absolute) == GraphEventResult::Accepted,
                      "graph-second-sample-event");
            }
            if (sample_index == 0) {
                observation.attempts[0] = lane.counters().attempts;
                observation.accepted[0] = lane.counters().accepted;
            }
            check(lane.begin_consumer() == GraphEventResult::Accepted, "graph-consumer-phase");
            for (const auto& event : lane.received())
                observation.events.push_back(event);
            check(lane.end_sample() == GraphEventResult::Accepted, "graph-end-sample");
            if (sample_index == 1) {
                observation.attempts[1] = lane.counters().attempts;
                observation.accepted[1] = lane.counters().accepted;
            }
            absolute++;
        }
        check(lane.end_block() == GraphEventResult::Accepted, "graph-end-block");
    }
    observation.overflow = lane.counters().overflow;
    observation.diagnostic_refusals = lane.counters().diagnostic_refusals;
    observation.sequence = lane.counters().sequence;
    return observation;
}

bool same_observation(const Observation& lhs, const Observation& rhs) {
    return lhs.events == rhs.events && lhs.attempts == rhs.attempts &&
           lhs.accepted == rhs.accepted && lhs.overflow == rhs.overflow &&
           lhs.diagnostic_refusals == rhs.diagnostic_refusals && lhs.sequence == rhs.sequence;
}

void graph_partition_controls() {
    constexpr std::array<std::size_t, 1> whole{2};
    constexpr std::array<std::size_t, 2> split{1, 1};
    const auto one_block = render(whole);
    const auto two_blocks = render(split);
    check(same_observation(one_block, two_blocks), "graph-partition-invariance");
    check(one_block.events.size() == 9 && one_block.events[0].sample == 0 &&
              one_block.events[0].sequence == 1 && one_block.events[8].sample == 1 &&
              one_block.events[8].sequence == 16,
          "graph-event-trace");
    check(one_block.attempts == std::array<std::uint8_t, 2>{16, 1} &&
              one_block.accepted == std::array<std::uint8_t, 2>{8, 1} &&
              one_block.overflow == 7 && one_block.diagnostic_refusals == 1 &&
              one_block.sequence == 17,
          "graph-counters");
}

} // namespace

int main() {
    try {
        host_ingress_controls();
        graph_partition_controls();
        std::cout << "g5-host-event-contract PASS checks=" << checks
                  << " host_mpe_events=3 graph_events=9 planted_negative=1\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g5-host-event-contract FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
