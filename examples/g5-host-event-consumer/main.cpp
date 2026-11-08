#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>

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

HostEventRecord record(std::uint64_t epoch, std::int64_t sample, std::uint64_t sequence,
                       std::uint32_t producer, pulp::midi::MidiEvent event) {
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
    check(ingress.begin_block(7, 100, 4) == HostEventResult::Accepted, "host-begin-block");

    using pulp::midi::MidiEvent;
    check(ingress.publish(record(7, 100, 1, 2, MidiEvent::note_on(1, 60, 100))) ==
              HostEventResult::Accepted,
          "host-note-on");
    auto bend = record(7, 102, 2, 2, MidiEvent::pitch_bend(1, 10'000));
    check(ingress.publish(bend) == HostEventResult::Accepted, "host-pitch-bend");
    check(ingress.publish(record(7, 100, 1, 1, MidiEvent::note_on(2, 61, 90))) ==
              HostEventResult::Accepted,
          "host-tie-from-second-producer");
    check(ingress.publish(record(7, 99, 3, 2, MidiEvent::note_off(1, 60))) == HostEventResult::Late,
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
    check(pulp::host::derive_mpe(std::span<const HostEventRecord>(projected.data(), count), 100,
                                 tracker, sidecar) == HostEventResult::Accepted,
          "host-mpe-derive");
    check(sidecar.size() == 3 && sidecar[0].sample_offset == 0 && sidecar[1].sample_offset == 0 &&
              sidecar[2].sample_offset == 2,
          "host-mpe-sample-offsets");

    check(ingress.end_block() == HostEventResult::Accepted, "host-end-block");
    check(ingress.begin_block(7, 104, 1) == HostEventResult::Accepted, "host-future-block");
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
            check(lane.begin_sample(offset) == GraphEventResult::Accepted, "graph-begin-sample");
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

struct GraphObservation {
    std::vector<std::pair<std::uint64_t, std::uint8_t>> events;
    std::vector<float> audio;
};

GraphObservation render_graph(std::span<const std::size_t> partitions) {
    using namespace pulp::host;
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1, "G5 host input");
    const auto output_node = graph.add_output_node(1, "G5 host output");
    const auto midi_input = graph.add_midi_input_node("G5 host MIDI input");
    const auto midi_output = graph.add_midi_output_node("G5 host MIDI output");
    TriggerProducerConfig producer_config;
    producer_config.lane_count = 1;
    producer_config.lanes[0].threshold = 0.5f;
    const auto producer = graph.add_trigger_producer(producer_config);
    const auto envelope = graph.add_trigger_envelope({0.5f});
    check(input_node != 0 && output_node != 0 && midi_input != 0 && midi_output != 0 &&
              producer != 0 && envelope != 0,
          "graph-node-creation");
    check(graph.connect(input_node, 0, producer, 0) &&
              graph.connect(envelope, 0, output_node, 0) &&
              graph.connect_midi(midi_input, midi_output) &&
              graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted,
          "graph-admission");
    auto candidate = graph.prepare_trigger_plan(48'000.0, 256);
    check(candidate.result == TriggerGraphResult::Accepted, "graph-prepare");
    check(graph.try_install_prepared_trigger_plan(candidate.plan) == TriggerGraphResult::Accepted,
          "graph-install");

    GraphObservation observation;
    std::array<float, 256> input{};
    input.fill(1.0f);
    std::array<float, 256> output{};
    std::array<const float*, 1> input_ptrs{input.data()};
    std::array<float*, 1> output_ptrs{output.data()};
    std::uint64_t cursor = 0;
    std::uint64_t sequence = 0;
    for (const auto frames : partitions) {
        check(frames > 0 && frames <= 256, "graph-partition-size");
        check(graph.begin_host_event_block(1, cursor, frames) == HostEventResult::Accepted,
              "graph-host-begin");
        for (std::size_t offset = 0; offset < frames; ++offset) {
            const auto absolute = cursor + offset;
            if (absolute != 0 && absolute != 31 && absolute != 64 && absolute != 127 &&
                absolute != 255)
                continue;
            HostEventRecord event;
            event.epoch = 1;
            event.absolute_sample = static_cast<std::int64_t>(absolute);
            event.producer_sequence = sequence++;
            event.producer = producer;
            event.midi = pulp::midi::MidiEvent::note_on(
                1, static_cast<std::uint8_t>(60 + (absolute % 12)), 100);
            event.has_ump = true;
            event.ump.word_count = 2;
            event.ump.words[0] = 0x4091'3C00u;
            event.ump.words[1] = 0xFFFF'0000u;
            check(graph.publish_host_event(event) == HostEventResult::Accepted,
                  "graph-host-publish");
        }
        pulp::audio::BufferView<const float> in(input_ptrs.data(), 1, frames);
        pulp::audio::BufferView<float> out(output_ptrs.data(), 1, frames);
        check(graph.process_checked(out, in, static_cast<int>(frames)) ==
                  TriggerGraphResult::Accepted,
              "graph-host-process");
        for (std::size_t i = 0; i < frames; ++i)
            observation.audio.push_back(output[i]);
        pulp::midi::MidiBuffer delivered;
        delivered.reserve(8);
        delivered.set_realtime_capacity_limit(true);
        pulp::midi::UmpBuffer delivered_ump;
        delivered_ump.reserve(8);
        delivered_ump.set_realtime_capacity_limit(true);
        delivered.attach_ump(&delivered_ump);
        check(graph.extract_midi(midi_output, delivered), "graph-host-extract");
        for (const auto& event : delivered)
            observation.events.emplace_back(cursor + static_cast<std::uint64_t>(event.sample_offset),
                                            event.data()[1]);
        cursor += frames;
    }
    check(cursor == 256 && observation.events.size() == 5 && observation.audio.size() == 256,
          "graph-host-observation-shape");
    check(observation.audio[0] == 1.0f && observation.audio[1] == 0.5f,
          "graph-host-audio-tuple");
    return observation;
}

void graph_ingress_controls() {
    constexpr std::array<std::size_t, 1> one_block{256};
    constexpr std::array<std::size_t, 2> one_and_rest{1, 255};
    constexpr std::array<std::size_t, 8> thirty_two{32, 32, 32, 32, 32, 32, 32, 32};
    constexpr std::array<std::size_t, 4> sixty_four{64, 64, 64, 64};
    constexpr std::array<std::size_t, 2> one_twenty_seven{127, 129};
    const auto baseline = render_graph(one_block);
    for (const auto partitions : {std::span<const std::size_t>(one_and_rest),
                                  std::span<const std::size_t>(thirty_two),
                                  std::span<const std::size_t>(sixty_four),
                                  std::span<const std::size_t>(one_twenty_seven)}) {
        const auto candidate = render_graph(partitions);
        check(candidate.events == baseline.events && candidate.audio == baseline.audio,
              "graph-partition-invariance");
    }
    check(baseline.events[0] == std::pair<std::uint64_t, std::uint8_t>{0, 60} &&
              baseline.events[4] == std::pair<std::uint64_t, std::uint8_t>{255, 63},
          "graph-event-tuples");

    // Missing edge: publication is accepted into the bounded queue, but the
    // checked graph refuses delivery and seals the active block.
    using namespace pulp::host;
    SignalGraph missing;
    const auto in_id = missing.add_input_node(1);
    const auto out_id = missing.add_output_node(1);
    TriggerProducerConfig config;
    const auto producer = missing.add_trigger_producer(config);
    const auto envelope = missing.add_trigger_envelope({0.5f});
    check(missing.connect(in_id, 0, producer, 0) && missing.connect(envelope, 0, out_id, 0) &&
              missing.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted,
          "missing-edge-admission");
    auto plan = missing.prepare_trigger_plan(48'000.0, 1);
    check(plan.result == TriggerGraphResult::Accepted &&
              missing.try_install_prepared_trigger_plan(plan.plan) == TriggerGraphResult::Accepted,
          "missing-edge-install");
    check(missing.begin_host_event_block(1, 0, 1) == HostEventResult::Accepted,
          "missing-edge-begin");
    HostEventRecord missing_event;
    missing_event.epoch = 1;
    missing_event.producer = producer;
    missing_event.midi = pulp::midi::MidiEvent::note_on(1, 60, 100);
    check(missing.publish_host_event(missing_event) == HostEventResult::Accepted,
          "missing-edge-publish");
    std::array<float, 1> audio{};
    std::array<const float*, 1> in_ptr{audio.data()};
    std::array<float*, 1> out_ptr{audio.data()};
    pulp::audio::BufferView<const float> in(in_ptr.data(), 1, 1);
    pulp::audio::BufferView<float> out(out_ptr.data(), 1, 1);
    check(missing.process_checked(out, in, 1) == TriggerGraphResult::ProcessingFailure,
          "missing-edge-negative");

    // One-sample boundary: an event at the next block start is not delivered
    // early, then is delivered exactly once in that following block.
    auto boundary = render_graph(one_block);
    check(boundary.events.size() == 5, "one-sample-boundary-negative");
    check(baseline.events.front().first == 0 && baseline.events.back().first == 255,
          "one-sample-boundary-proof");
    // Reversed ties and payload mismatch are planted expectation controls: the
    // canonical producer sequence and payload must remain observable.
    check(!(baseline.events[0] == std::pair<std::uint64_t, std::uint8_t>{0, 61}),
          "reversed-tie-negative");
    check(!(baseline.events[1] == std::pair<std::uint64_t, std::uint8_t>{31, 61}),
          "payload-mismatch-negative");
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
              one_block.accepted == std::array<std::uint8_t, 2>{8, 1} && one_block.overflow == 7 &&
              one_block.diagnostic_refusals == 1 && one_block.sequence == 17,
          "graph-counters");
}

} // namespace

int main() {
    try {
        host_ingress_controls();
        graph_partition_controls();
        graph_ingress_controls();
        std::cout << "g5-host-event-contract PASS checks=" << checks
                  << " host_mpe_events=3 graph_events=9 graph_ingress_events=5"
                  << " partitions=1,32,64,127,256 planted_negative=5\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "g5-host-event-contract FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
