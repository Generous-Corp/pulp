#include <pulp/audio/buffer.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/custom_node_type.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <pulp/host/voice_bank_binding.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>
#include <pulp/state/parameter_event_queue.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
int checks = 0;
void check(bool ok, const char* rule) {
    ++checks;
    if (!ok)
        throw std::runtime_error(rule);
}

using pulp::host::GraphAuthoredEventLane;
using pulp::host::GraphEventResult;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;

HostEventRecord midi_record(std::uint64_t epoch, std::int64_t sample, std::uint64_t sequence,
                            std::uint32_t producer, pulp::midi::MidiEvent midi) {
    HostEventRecord record;
    record.epoch = epoch;
    record.absolute_sample = sample;
    record.producer_sequence = sequence;
    record.producer = producer;
    record.midi = midi;
    return record;
}

// ── Realtime boundary for the combined audio callback ─────────────────────
//
// Under RealtimeSanitizer (-fsanitize=realtime) the per-block audio-side
// calls run inside a [[clang::nonblocking]] function, so any allocation, lock
// or blocking call reached from them aborts with a report. Staging MIDI into
// the ingress, building the parameter batch and the voice records, filling the
// input ramp and every check() stay outside the boundary. P1_RTSAN_CONTROL
// plants a real malloc, mutex or sleep inside the boundary; the analyzer must
// report each one. node-malloc and voice-malloc plant the allocation inside the
// graph's custom-node callback and inside the voice render, proving the
// realtime context reaches through SignalGraph::process and
// VoiceBankBinding::render rather than only the boundary entry. Without RTSan the boundary is a plain function and the
// planted controls are not compiled.
#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define P1_RTSAN_BOUNDARY [[clang::nonblocking]]
#define P1_HAS_RTSAN 1
#endif
#endif
#ifndef P1_RTSAN_BOUNDARY
#define P1_RTSAN_BOUNDARY
#endif

#if defined(P1_HAS_RTSAN)
constexpr const char* kRealtimeBoundary = "nonblocking";
enum class AnalyzerControl { Clean, Malloc, Mutex, Sleep, NodeMalloc, VoiceMalloc };

AnalyzerControl analyzer_control() {
    const char* requested = std::getenv("P1_RTSAN_CONTROL");
    if (requested == nullptr)
        return AnalyzerControl::Clean;
    const std::string_view name(requested);
    if (name == "malloc")
        return AnalyzerControl::Malloc;
    if (name == "mutex")
        return AnalyzerControl::Mutex;
    if (name == "sleep")
        return AnalyzerControl::Sleep;
    if (name == "node-malloc")
        return AnalyzerControl::NodeMalloc;
    if (name == "voice-malloc")
        return AnalyzerControl::VoiceMalloc;
    check(name == "clean", "rtsan-control-name");
    return AnalyzerControl::Clean;
}

// Real intercepted operations. The volatile function pointer keeps Release
// optimization from deleting the malloc/free pair.
void inject_analyzer_control(AnalyzerControl control) {
    switch (control) {
    case AnalyzerControl::Clean:
    case AnalyzerControl::NodeMalloc:
    case AnalyzerControl::VoiceMalloc:
        break;
    case AnalyzerControl::Malloc: {
        void* (*volatile allocate)(std::size_t) = &std::malloc;
        std::free(allocate(16));
        break;
    }
    case AnalyzerControl::Mutex: {
        std::mutex mutex;
        mutex.lock();
        mutex.unlock();
        break;
    }
    case AnalyzerControl::Sleep:
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        break;
    }
}
#else
constexpr const char* kRealtimeBoundary = "none";
enum class AnalyzerControl { Clean, NodeMalloc, VoiceMalloc };
AnalyzerControl analyzer_control() {
    return AnalyzerControl::Clean;
}
#endif

AnalyzerControl planted_control = AnalyzerControl::Clean;

// A planted allocation reached from a callee of the boundary.
void inject_callee_control([[maybe_unused]] AnalyzerControl site) {
#if defined(P1_HAS_RTSAN)
    if (planted_control == site) {
        void* (*volatile allocate)(std::size_t) = &std::malloc;
        std::free(allocate(16));
    }
#endif
}
std::size_t realtime_boundary_blocks = 0;

std::array<int, 4> host_and_parameter_controls(std::size_t partition) {
    HostEventIngress<8> ingress;
    check(ingress.begin_block(7, 0, partition) == HostEventResult::Accepted, "midi-block-begin");
    auto note = midi_record(7, 2, 1, 4, pulp::midi::MidiEvent::note_on(1, 60, 100));
    auto tie = midi_record(7, 2, 1, 3, pulp::midi::MidiEvent::note_on(1, 61, 100));
    check(ingress.publish(note) == HostEventResult::Accepted &&
              ingress.publish(tie) == HostEventResult::Accepted,
          "midi-publish");
    // Duplicate detection covers staged records only: project() removes the
    // records it delivers, so the refusal must be observed before projection.
    check(ingress.publish(midi_record(7, 2, 1, 3, pulp::midi::MidiEvent::note_on(1, 62, 100))) ==
              HostEventResult::InvalidSequence,
          "midi-duplicate-sequence-refusal");
    std::array<HostEventRecord, 8> projected{};
    std::size_t count = 0;
    check(ingress.project(projected, count) == HostEventResult::Accepted && count == 2,
          "midi-project");
    check(projected[0].producer == 3 && projected[1].producer == 4, "midi-stable-producer-order");
    check(ingress.publish(midi_record(6, 2, 3, 1, pulp::midi::MidiEvent::note_on(1, 62, 100))) ==
              HostEventResult::StaleEpoch,
          "midi-stale-epoch-refusal");
    check(ingress.end_block() == HostEventResult::Accepted, "midi-block-end");

    check(ingress.begin_block(7, partition, partition) == HostEventResult::Accepted,
          "midi-second-block-begin");
    check(ingress.publish(midi_record(7, static_cast<std::int64_t>(partition) - 1, 4, 1,
                                      pulp::midi::MidiEvent::note_off(1, 60))) ==
              HostEventResult::Late,
          "midi-late-refusal");
    check(ingress.publish(midi_record(7, static_cast<std::int64_t>(partition) + 1, 2, 5,
                                      pulp::midi::MidiEvent::note_on(1, 64, 100))) ==
              HostEventResult::Accepted,
          "midi-second-block-publish");
    count = 0;
    check(ingress.project(projected, count) == HostEventResult::Accepted && count == 1 &&
              projected[0].absolute_sample == partition + 1,
          "midi-second-block-project");
    check(ingress.end_block() == HostEventResult::Accepted, "midi-second-block-end");

    pulp::state::ParameterEventQueue parameters;
    parameters.push({.param_id = 11, .sample_offset = 3, .value = 0.75f});
    parameters.push({.param_id = 11, .sample_offset = 1, .value = 0.25f});
    parameters.sort();
    check(parameters.events()[0].sample_offset == 1 && parameters.events()[1].sample_offset == 3,
          "parameter-stable-sort");
    for (std::size_t i = 0; i < parameters.capacity(); ++i)
        parameters.push({.param_id = 12, .sample_offset = 0, .value = 1.0f});
    check(parameters.overflowed() && parameters.dropped_event_count() == 2,
          "parameter-overflow-refusal");
    parameters.clear();
    parameters.push({.param_id = 11, .sample_offset = 2, .value = 0.9f});
    parameters.push({.param_id = 11, .sample_offset = 0, .value = 0.1f});
    parameters.sort();
    check(parameters.events()[0].sample_offset == 0 && parameters.events()[1].sample_offset == 2 &&
              parameters.events()[0].value == 0.1f && parameters.events()[1].value == 0.9f,
          "parameter-second-block-order");
    return {2, 1, static_cast<int>(parameters.events()[0].sample_offset),
            static_cast<int>(parameters.events()[1].sample_offset)};
}

void trigger_two_block_controls() {
    using namespace pulp::host;
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    TriggerProducerConfig config;
    config.lane_count = 1;
    config.lanes[0].payload.token = 17;
    const auto producer = graph.add_trigger_producer(config);
    const auto envelope = graph.add_trigger_envelope({0.5f});
    check(graph.connect(input, 0, producer, 0) && graph.connect(envelope, 0, output, 0),
          "trigger-audio-connect");
    check(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted,
          "trigger-edge-admission");
    auto prepared = graph.prepare_trigger_plan(48'000.0, 4);
    check(prepared.result == TriggerGraphResult::Accepted &&
              graph.try_install_prepared_trigger_plan(prepared.plan) ==
                  TriggerGraphResult::Accepted,
          "trigger-plan-install");
    std::array<float, 4> source{0.0f, 1.0f, 0.0f, 0.0f};
    std::array<float, 4> output_samples{};
    const float* input_ptr = source.data();
    float* output_ptr = output_samples.data();
    pulp::audio::BufferView<const float> input_view(&input_ptr, 1, source.size());
    pulp::audio::BufferView<float> output_view(&output_ptr, 1, output_samples.size());
    check(graph.process_checked(output_view, input_view, 4) == TriggerGraphResult::Accepted,
          "trigger-first-block");
    // trigger_trace() is a view of the most recent block only; it expires at
    // the next render boundary, so each block's record is copied out here.
    const auto first_trace = graph.trigger_trace();
    check(first_trace.size() == 1, "trigger-first-block-trace");
    const TriggerRecord first = first_trace[0];
    source = {0.0f, 0.0f, 1.0f, 0.0f};
    check(graph.process_checked(output_view, input_view, 4) == TriggerGraphResult::Accepted,
          "trigger-second-block");
    const auto second_trace = graph.trigger_trace();
    check(second_trace.size() == 1, "trigger-second-block-trace");
    const TriggerRecord second = second_trace[0];
    check(first.sample == 1 && second.sample == 6, "trigger-cross-block-samples");
    check(first.sequence == 0 && second.sequence == 1 && first.epoch == second.epoch,
          "trigger-sequence-epoch");
    check(graph.try_reset_events() == TriggerGraphResult::Accepted, "trigger-reset");
}

// ── Graph-authored event lane ──────────────────────────────────────────────
// The trigger row uses the public graph-authored lane: a CustomNodeType
// producer emits through GraphEventWriter::emit_at and a connected consumer
// reads GraphEventReader::events. Results are recorded inside the callback and
// asserted after rendering so a failed rule is reported, not thrown from the
// audio callback. Callback-side observation writes into preallocated fixed
// arrays; the vectors below are filled from them after rendering.

struct GraphLaneObservation {
    std::vector<pulp::host::GraphAuthoredEvent> events;
    std::vector<std::size_t> delivered_block;
    std::vector<std::uint64_t> delivered_block_start;
    std::vector<float> audio;
    std::array<GraphEventResult, 9> staged{};
    GraphEventResult current = GraphEventResult::Unbound;
    GraphEventResult overflow = GraphEventResult::Unbound;
    GraphEventResult future = GraphEventResult::Unbound;
    GraphEventResult late = GraphEventResult::Unbound;
};

constexpr std::uint64_t kLaneCurrentSample = 1;
constexpr std::uint64_t kLaneFutureSample = 4;
constexpr std::uint64_t kLaneLateSample = 0;
constexpr std::uint64_t kLaneFarFuture = 2 + pulp::host::GraphAuthoredEventLane<>::kPendingCapacity;

GraphLaneObservation render_graph_event_lane(const std::vector<std::size_t>& schedule) {
    using namespace pulp::host;
    struct State {
        std::size_t producer_calls = 0;
        std::size_t consumer_block = 0;
        std::uint64_t consumer_cursor = 0;
        std::array<GraphAuthoredEvent, 16> delivered{};
        std::array<std::size_t, 16> delivered_block{};
        std::array<std::uint64_t, 16> delivered_block_start{};
        std::size_t delivered_count = 0;
        bool delivered_dropped = false;
        GraphLaneObservation observation;
    };
    auto state = std::make_shared<State>();
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "p1.public.graph-event-lane";
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [state](auto& out, const auto& in, int frames,
                                  const CustomNodeEventBlock& events) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        auto& obs = state->observation;
        if (events.graph_writer != nullptr) {
            if (state->producer_calls == 0) {
                obs.current = events.graph_writer->emit_at(kLaneCurrentSample, 10);
                for (std::uint16_t i = 0; i < 8; ++i)
                    obs.staged[i] = events.graph_writer->emit_at(kLaneFutureSample, 20 + i);
                obs.overflow = events.graph_writer->emit_at(kLaneFutureSample, 99);
                obs.future = events.graph_writer->emit_at(kLaneFarFuture, 77);
            } else if (state->producer_calls == 1) {
                obs.late = events.graph_writer->emit_at(kLaneLateSample, 98);
            }
            ++state->producer_calls;
        }
        if (events.graph_reader != nullptr) {
            for (const auto& event : events.graph_reader->events()) {
                if (state->delivered_count == state->delivered.size()) {
                    state->delivered_dropped = true;
                    continue;
                }
                state->delivered[state->delivered_count] = event;
                state->delivered_block[state->delivered_count] = state->consumer_block;
                state->delivered_block_start[state->delivered_count] = state->consumer_cursor;
                ++state->delivered_count;
                if (event.sample >= state->consumer_cursor &&
                    event.sample - state->consumer_cursor < static_cast<std::uint64_t>(frames))
                    out.channel_ptr(0)[event.sample - state->consumer_cursor] =
                        static_cast<float>(event.payload);
            }
            state->consumer_cursor += static_cast<std::uint64_t>(frames);
            ++state->consumer_block;
        }
    };
    check(graph.register_custom_node_type(type), "graph-event-register");
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    const auto producer = graph.add_custom_node(type.type_id);
    const auto consumer = graph.add_custom_node(type.type_id);
    check(graph.connect(input, 0, producer, 0) && graph.connect(producer, 0, consumer, 0) &&
              graph.connect(consumer, 0, output, 0),
          "graph-event-connect");
    check(graph.connect_graph_event({producer, consumer}) == GraphEventBindingResult::Accepted,
          "graph-event-edge-admission");
    check(graph.prepare(48000.0, 8), "graph-event-prepare");
    std::array<float, 8> source{};
    std::array<float, 8> destination{};
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    for (const auto frames : schedule) {
        destination.fill(0.0f);
        pulp::audio::BufferView<const float> in(&source_ptr, 1, frames);
        pulp::audio::BufferView<float> out(&destination_ptr, 1, frames);
        graph.process(out, in, static_cast<int>(frames));
        for (std::size_t i = 0; i < frames; ++i)
            state->observation.audio.push_back(destination[i]);
    }
    check(!state->delivered_dropped, "graph-event-observation-capacity");
    auto& obs = state->observation;
    for (std::size_t i = 0; i < state->delivered_count; ++i) {
        obs.events.push_back(state->delivered[i]);
        obs.delivered_block.push_back(state->delivered_block[i]);
        obs.delivered_block_start.push_back(state->delivered_block_start[i]);
    }
    return obs;
}

std::size_t graph_event_lane_controls(const std::vector<std::size_t>& schedule,
                                      GraphLaneObservation& observation) {
    observation = render_graph_event_lane(schedule);
    const auto& obs = observation;
    check(obs.current == GraphEventResult::Accepted, "graph-event-current-accept");
    for (const auto result : std::span(obs.staged.data(), 8))
        check(result == GraphEventResult::Accepted, "graph-event-future-stage");
    check(obs.overflow == GraphEventResult::Overflow, "graph-event-overflow-refusal");
    check(obs.future == GraphEventResult::Future, "graph-event-future-refusal");
    check(obs.late == GraphEventResult::Late, "graph-event-late-refusal");
    check(obs.events.size() == 9, "graph-event-exactly-once");
    check(obs.events[0].sample == kLaneCurrentSample && obs.events[0].payload == 10 &&
              obs.delivered_block[0] == 0,
          "graph-event-current-block-delivery");
    for (std::size_t i = 1; i < obs.events.size(); ++i) {
        const auto& event = obs.events[i];
        // Bounded-future records are staged in block 0 and delivered only in
        // the later block whose window contains their absolute sample.
        check(event.sample == kLaneFutureSample && event.payload == 20 + (i - 1) &&
                  obs.delivered_block[i] > 0 && obs.delivered_block_start[i] > 0 &&
                  obs.delivered_block_start[i] <= event.sample,
              "graph-event-bounded-future-delivery");
        check(event.sequence > obs.events[i - 1].sequence && event.epoch == obs.events[0].epoch,
              "graph-event-tie-order");
    }
    check(obs.audio.size() == 8 && obs.audio[kLaneCurrentSample] == 10.0f &&
              obs.audio[kLaneFutureSample] == 27.0f,
          "graph-event-audio-oracle");
    return obs.events.size();
}

void graph_event_reset_controls() {
    using namespace pulp::host;
    struct State {
        std::size_t producer_calls = 0;
        GraphEventResult staged_current = GraphEventResult::Unbound;
        GraphEventResult staged_future = GraphEventResult::Unbound;
        GraphEventResult post_reset = GraphEventResult::Unbound;
        std::array<GraphAuthoredEvent, 8> delivered{};
        std::size_t delivered_count = 0;
    };
    auto state = std::make_shared<State>();
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "p1.public.graph-event-reset";
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [state](auto& out, const auto& in, int frames,
                                  const CustomNodeEventBlock& events) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        if (events.graph_writer != nullptr) {
            if (state->producer_calls == 0) {
                state->staged_current = events.graph_writer->emit_at(0, 1);
                state->staged_future = events.graph_writer->emit_at(3, 2);
            } else if (state->producer_calls == 2) {
                state->post_reset = events.graph_writer->emit_at(2, 3);
            }
            ++state->producer_calls;
        }
        if (events.graph_reader != nullptr)
            for (const auto& event : events.graph_reader->events())
                if (state->delivered_count < state->delivered.size())
                    state->delivered[state->delivered_count++] = event;
    };
    check(graph.register_custom_node_type(type), "graph-event-reset-register");
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    const auto producer = graph.add_custom_node(type.type_id);
    const auto consumer = graph.add_custom_node(type.type_id);
    check(graph.connect(input, 0, producer, 0) && graph.connect(producer, 0, consumer, 0) &&
              graph.connect(consumer, 0, output, 0) &&
              graph.connect_graph_event({producer, consumer}) == GraphEventBindingResult::Accepted,
          "graph-event-reset-admission");
    check(graph.prepare(48000.0, 2), "graph-event-reset-prepare");
    std::array<float, 2> source{}, destination{};
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    pulp::audio::BufferView<const float> in(&source_ptr, 1, source.size());
    pulp::audio::BufferView<float> out(&destination_ptr, 1, source.size());
    graph.process(out, in, 2);
    pulp::format::ProcessContext reset;
    reset.reset_requested = true;
    reset.position_samples = 0;
    graph.process(out, in, 2, reset);
    graph.process(out, in, 2);
    const std::span delivered(state->delivered.data(), state->delivered_count);
    check(state->staged_current == GraphEventResult::Accepted &&
              state->staged_future == GraphEventResult::Accepted,
          "graph-event-reset-stage");
    // The record staged for sample 3 before the reset is retired with its
    // epoch; the post-reset block that covers sample 3 never delivers it.
    check(std::none_of(delivered.begin(), delivered.end(),
                       [](const GraphAuthoredEvent& event) { return event.payload == 2; }),
          "graph-event-reset-cancels-pending");
    check(state->post_reset == GraphEventResult::Accepted && delivered.size() == 2 &&
              delivered[0].payload == 1 && delivered[1].payload == 3,
          "graph-event-reset-post-emit");
    check(delivered[1].epoch == delivered[0].epoch + 1 && delivered[1].sample == 2,
          "graph-event-reset-epoch-advance");
}

// ── Parameter mailbox through a PluginSlot node ────────────────────────────

// The probe records each delivered event into a preallocated fixed array with
// its block index and absolute sample, so the process() callback does not
// allocate. Per-block vectors are assembled after rendering.
class ParameterProbeSlot final : public pulp::host::PluginSlot {
  public:
    static constexpr std::uint32_t kParamId = 11;
    static constexpr std::size_t kEntryCapacity = 2048;
    struct Entry {
        std::size_t block = 0;
        std::uint64_t absolute = 0;
        pulp::host::ParameterEvent event{};
    };
    const pulp::host::PluginInfo& info() const override {
        return info_;
    }
    bool is_loaded() const override {
        return true;
    }
    bool prepare(double, int) override {
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& events, int frames) override {
        for (const auto& event : events) {
            if (count_ == entries_.size()) {
                dropped_ = true;
                continue;
            }
            entries_[count_++] = {block_count_,
                                  cursor_ + static_cast<std::uint64_t>(event.sample_offset), event};
        }
        ++block_count_;
        cursor_ += static_cast<std::uint64_t>(frames);
        for (std::size_t c = 0; c < out.num_channels(); ++c)
            for (int i = 0; i < frames; ++i)
                out.channel_ptr(c)[i] = 0.0f;
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        pulp::host::HostParamInfo p;
        p.id = kParamId;
        p.name = "probe";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.flags.automatable = true;
        return {p};
    }
    float get_parameter(std::uint32_t) const override {
        return 0.0f;
    }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override {
        return false;
    }
    std::vector<std::uint8_t> save_state() const override {
        return {};
    }
    bool restore_state(const std::vector<std::uint8_t>&) override {
        return false;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return 0;
    }
    int tail_samples() const override {
        return 0;
    }
    std::size_t block_count() const {
        return block_count_;
    }
    bool dropped() const {
        return dropped_;
    }
    std::span<const Entry> entries() const {
        return {entries_.data(), count_};
    }
    std::vector<pulp::host::ParameterEvent> block(std::size_t index) const {
        std::vector<pulp::host::ParameterEvent> result;
        for (const auto& entry : entries())
            if (entry.block == index)
                result.push_back(entry.event);
        return result;
    }

  private:
    pulp::host::PluginInfo info_ = [] {
        pulp::host::PluginInfo info{};
        info.name = "P1ParameterProbe";
        info.format = pulp::host::PluginFormat::CLAP;
        info.num_inputs = 0;
        info.num_outputs = 1;
        info.category = "Fx";
        return info;
    }();
    std::array<Entry, kEntryCapacity> entries_{};
    std::size_t count_ = 0;
    std::size_t block_count_ = 0;
    std::uint64_t cursor_ = 0;
    bool dropped_ = false;
};

struct ParameterObservation {
    std::vector<std::vector<std::pair<int, float>>> blocks;
    bool overflow_refused = false;
    std::size_t retained_prefix = 0;
};

ParameterObservation render_parameter_mailbox(std::size_t frames) {
    using pulp::host::ParameterEventQueue;
    pulp::host::SignalGraph graph;
    auto slot = std::make_unique<ParameterProbeSlot>();
    auto* probe = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 0, 1, "p1-parameter-probe");
    check(graph.prepare(48000.0, 8), "parameter-mailbox-prepare");
    std::array<float, 8> output{};
    float* output_ptr = output.data();
    pulp::audio::BufferView<float> out(&output_ptr, 1, frames);
    pulp::audio::BufferView<const float> in;
    const auto id = ParameterProbeSlot::kParamId;

    // Block 0: an older injection is superseded by the latest one before the
    // callback. The latest carries two equal offsets in push order.
    ParameterEventQueue older;
    check(older.push({.param_id = id, .sample_offset = 1, .value = 0.2f}),
          "parameter-mailbox-push");
    ParameterEventQueue latest;
    check(latest.push({.param_id = id, .sample_offset = 2, .value = 0.5f}) &&
              latest.push({.param_id = id, .sample_offset = 2, .value = 0.6f}) &&
              latest.push({.param_id = id, .sample_offset = 0, .value = 0.1f}),
          "parameter-mailbox-push");
    check(graph.inject_parameter_events(plugin, older) &&
              graph.inject_parameter_events(plugin, latest),
          "parameter-mailbox-inject");
    graph.process(out, in, static_cast<int>(frames));
    // Block 1: nothing injected; a one-shot mailbox delivers nothing.
    graph.process(out, in, static_cast<int>(frames));
    // Block 2: a fresh injection is delivered once.
    ParameterEventQueue next;
    check(next.push({.param_id = id, .sample_offset = 3, .value = 0.9f}), "parameter-mailbox-push");
    check(graph.inject_parameter_events(plugin, next), "parameter-mailbox-inject");
    graph.process(out, in, static_cast<int>(frames));

    ParameterObservation observation;
    // Source-side overflow: the queue drops its last push and the graph
    // refuses the injection while publishing the retained prefix.
    ParameterEventQueue overflowing;
    for (std::size_t i = 0; i <= overflowing.capacity(); ++i)
        (void)overflowing.push({.param_id = id, .sample_offset = 0, .value = 0.3f});
    observation.overflow_refused = !graph.inject_parameter_events(plugin, overflowing);
    graph.process(out, in, static_cast<int>(frames));

    check(probe->block_count() == 4 && !probe->dropped(), "parameter-mailbox-block-count");
    for (std::size_t b = 0; b < 3; ++b) {
        std::vector<std::pair<int, float>> values;
        for (const auto& event : probe->block(b)) {
            check(event.param_id == id, "parameter-mailbox-param-id");
            values.emplace_back(event.sample_offset, event.value);
        }
        observation.blocks.push_back(std::move(values));
    }
    observation.retained_prefix = probe->block(3).size();
    return observation;
}

void parameter_mailbox_controls(const ParameterObservation& obs) {
    const auto& b = obs.blocks;
    check(b[0].size() == 3, "parameter-mailbox-latest-wins");
    check(b[0][0] == std::pair<int, float>{0, 0.1f}, "parameter-mailbox-sorted-delivery");
    check(b[0][1] == std::pair<int, float>{2, 0.5f} && b[0][2] == std::pair<int, float>{2, 0.6f},
          "parameter-mailbox-equal-offset-order");
    check(b[1].empty(), "parameter-mailbox-one-shot");
    check(b[2].size() == 1 && b[2][0] == std::pair<int, float>{3, 0.9f},
          "parameter-mailbox-second-delivery");
    check(obs.overflow_refused, "parameter-mailbox-source-overflow-refusal");
    check(obs.retained_prefix == pulp::host::ParameterEventQueue::kCapacity,
          "parameter-mailbox-retained-prefix");
}

// ── Voice intent through the installed voice-bank binding ──────────────────

// ReleaseFrames sets how long a released note keeps its slot. The combined
// control holds every slot for the whole run so slot and generation stay
// observable after any block size.
template <unsigned ReleaseFrames> class P1VoiceT final : public pulp::midi::MpeSynthVoice {
  public:
    static constexpr unsigned kReleaseFrames = ReleaseFrames;
    void on_note_on(const pulp::midi::MpeNoteState& note) override {
        pulp::midi::MpeSynthVoice::on_note_on(note);
        release_frame_ = 0;
        set_smoothing(0.0f);
    }
    void on_note_off() override {
        pulp::midi::MpeSynthVoice::on_note_off();
        release_frame_ = 0;
    }
    void reset() override {
        pulp::midi::MpeSynthVoice::reset();
        release_frame_ = 0;
    }
    void render(float* out, int frames) override {
        render_stereo(out, nullptr, frames);
    }
    void render_stereo(float* left, float* right, int frames) {
        inject_callee_control(AnalyzerControl::VoiceMalloc);
        for (int i = 0; i < frames; ++i) {
            float sample = 0.0f;
            if (active()) {
                advance_smoothers();
                const float level = static_cast<float>(note_number()) / 128.0f;
                sample = releasing() ? level * static_cast<float>(kReleaseFrames - release_frame_) /
                                           static_cast<float>(kReleaseFrames)
                                     : level;
                if (releasing() && ++release_frame_ == kReleaseFrames)
                    finish_release();
            }
            left[i] = sample;
            if (right)
                right[i] = sample;
        }
    }

  private:
    unsigned release_frame_ = 0;
};

using P1Voice = P1VoiceT<8>;
using P1HeldVoice = P1VoiceT<4096>;

template <typename Voice> class P1VoiceProcessor final : public pulp::format::Processor {
  public:
    explicit P1VoiceProcessor(Voice& voice) : voice_(voice) {}
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "P1 Voice";
        d.manufacturer = "Pulp Test";
        d.bundle_id = "com.pulp.test.p1-voice";
        d.version = "1.0.0";
        d.category = pulp::format::PluginCategory::Instrument;
        d.output_buses = {{"Stereo", 2, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "level", .range = pulp::state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        voice_.render_stereo(output.channel_ptr(0), output.channel_ptr(1),
                             static_cast<int>(output.num_samples()));
    }

  private:
    Voice& voice_;
};

pulp::host::SubgraphTemplate voice_definition() {
    using namespace pulp::host;
    SubgraphTemplate t;
    t.identity = "p1.voice.v1";
    t.binding_path = {"voice"};
    SubgraphNodeSpec node;
    node.key = "osc";
    node.type = NodeType::Plugin;
    node.channels = 2;
    node.processor_type_id = "p1.voice";
    node.processor_type_version = 1;
    node.processor_descriptor_version = "1.0.0";
    t.nodes.push_back(std::move(node));
    t.exposed_ports = {{"audio_out.left", "osc", 0, true, PortKind::Audio},
                       {"audio_out.right", "osc", 1, true, PortKind::Audio}};
    t.parameters = {{"level", "osc", 1.0f, 0.0f, 2.0f, 1, "level"}};
    return t;
}

pulp::host::VoiceTemplateSchema voice_schema() {
    using namespace pulp::host;
    VoiceTemplateSchema result;
    result.template_namespace = "p1";
    result.template_name = "voice";
    result.template_revision = 1;
    auto port = [](const char* name, VoiceBindingType type, VoiceBindingDirection direction,
                   VoiceBindingShape shape, VoiceBindingRate rate, std::uint32_t ordinal) {
        VoiceBindingSpec spec;
        spec.path = {"voice"};
        spec.target = name;
        spec.target_kind = VoiceBindingTarget::Port;
        spec.type = type;
        spec.direction = direction;
        spec.shape = shape;
        spec.rate = rate;
        spec.ordinal = ordinal;
        return spec;
    };
    result.bindings = {
        port("note_in", VoiceBindingType::Event, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Event, 0),
        port("pitch", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 1),
        port("pressure", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 2),
        port("timbre", VoiceBindingType::Control, VoiceBindingDirection::Input,
             VoiceBindingShape::Scalar, VoiceBindingRate::Sample, 3),
        port("audio_out", VoiceBindingType::Audio, VoiceBindingDirection::Output,
             VoiceBindingShape::Stereo, VoiceBindingRate::Sample, 4),
    };
    VoiceBindingSpec level;
    level.path = {"voice"};
    level.target = "level";
    level.target_kind = VoiceBindingTarget::Parameter;
    level.type = VoiceBindingType::Control;
    level.direction = VoiceBindingDirection::Input;
    level.shape = VoiceBindingShape::Scalar;
    level.rate = VoiceBindingRate::Block;
    result.bindings.push_back(level);
    return result;
}

template <typename Voice = P1Voice>
std::unique_ptr<pulp::host::VoiceBankBinding> make_voice_bank(int max_frames = 64) {
    using namespace pulp::host;
    auto owner = std::make_unique<MpeVoiceBankOwner<Voice>>(
        [](Voice& voice, std::uint32_t) -> std::unique_ptr<pulp::format::Processor> {
            return std::make_unique<P1VoiceProcessor<Voice>>(voice);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    VoiceBankBindingResult status;
    auto bank = VoiceBankBinding::prepare(voice_schema(), voice_definition(), std::move(owner),
                                          "audio_out", 48000, max_frames, status);
    check(static_cast<bool>(bank), "voice-bank-prepare");
    return bank;
}

struct SlotIntent {
    std::uint32_t slot = 0;
    pulp::midi::MpeNoteState state{};
    bool releasing = false;
};

bool find_live_slot(const pulp::host::VoiceBankBinding& bank, SlotIntent& found) {
    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        pulp::midi::MpeNoteState note{};
        bool releasing = false;
        if (bank.owner().slot_note(slot, note, releasing)) {
            found = {slot, note, releasing};
            return true;
        }
    }
    return false;
}

struct VoiceObservation {
    std::vector<float> audio;
    std::uint32_t first_slot = 0;
    std::uint32_t second_slot = 0;
    pulp::midi::MpeNoteGeneration first_generation = 0;
    pulp::midi::MpeNoteGeneration second_generation = 0;
};

constexpr std::uint64_t kVoiceNoteOn = 2;
constexpr std::uint64_t kVoiceNoteOff = 10;
constexpr std::uint64_t kVoiceReuse = 26;
constexpr std::uint64_t kVoiceFrames = 32;

VoiceObservation render_voice_bank(std::size_t partition) {
    using namespace pulp::host;
    using Kind = pulp::midi::MpeExpressionEvent::Kind;
    auto bank = make_voice_bank();
    VoiceObservation obs;
    SlotIntent first;
    std::uint64_t sequence = 1;
    const auto publish_if = [&](std::uint64_t start, std::size_t frames, std::uint64_t at,
                                pulp::midi::MidiEvent message) {
        if (start <= at && at < start + frames) {
            HostEventRecord record;
            record.epoch = bank->epoch();
            record.absolute_sample = static_cast<std::int64_t>(at);
            record.producer = 1;
            record.producer_sequence = sequence++;
            record.midi = message;
            check(bank->publish(record) == HostEventResult::Accepted, "voice-bank-publish");
        }
    };
    for (std::uint64_t start = 0; start < kVoiceFrames; start += partition) {
        const auto frames =
            static_cast<std::size_t>(std::min<std::uint64_t>(partition, kVoiceFrames - start));
        check(bank->begin_block(start, frames) == HostEventResult::Accepted, "voice-bank-begin");
        publish_if(start, frames, kVoiceNoteOn, pulp::midi::MidiEvent::note_on(1, 60, 100));
        publish_if(start, frames, kVoiceNoteOff, pulp::midi::MidiEvent::note_off(1, 60));
        publish_if(start, frames, kVoiceReuse, pulp::midi::MidiEvent::note_on(1, 64, 90));
        pulp::audio::Buffer<float> buffer(2, frames);
        auto view = buffer.view();
        check(bank->render(view, start, frames) == VoiceBankBindingCode::Ok, "voice-bank-render");
        for (std::size_t i = 0; i < frames; ++i)
            obs.audio.push_back(view.channel(0)[i]);
        const auto end = start + frames;
        if (end == 8) {
            // Note-on intent landed on one slot with a live generation.
            check(find_live_slot(*bank, first) && first.state.note == 60 &&
                      first.state.note_id != 0 && !first.releasing,
                  "voice-bank-note-on-slot");
            obs.first_slot = first.slot;
            obs.first_generation = first.state.note_id;
            auto wrong = first.state;
            wrong.note_id = first.state.note_id + 1;
            check(bank->dispatch_deferred(bank->epoch(), first.slot, wrong.note_id, Kind::Pressure,
                                          wrong) == VoiceBankBindingCode::StaleGeneration,
                  "voice-bank-wrong-generation-refusal");
            check(bank->dispatch_deferred(bank->epoch(), (first.slot + 1) % 8, first.state.note_id,
                                          Kind::Pressure,
                                          first.state) == VoiceBankBindingCode::StaleGeneration,
                  "voice-bank-wrong-slot-refusal");
        } else if (end == 16) {
            // Note-off intent from a later block puts the same generation into
            // release; a second release for it is refused.
            SlotIntent releasing;
            check(find_live_slot(*bank, releasing) && releasing.slot == first.slot &&
                      releasing.state.note_id == first.state.note_id && releasing.releasing,
                  "voice-bank-release-intent");
            check(bank->dispatch_deferred(bank->epoch(), first.slot, first.state.note_id,
                                          Kind::NoteOff,
                                          first.state) == VoiceBankBindingCode::StaleGeneration,
                  "voice-bank-double-release-refusal");
        } else if (end == 24) {
            SlotIntent none;
            check(!find_live_slot(*bank, none), "voice-bank-slot-retired");
            auto pressure = first.state;
            pressure.pressure = 0.5f;
            check(bank->dispatch_deferred(bank->epoch(), first.slot, first.state.note_id,
                                          Kind::Pressure,
                                          pressure) == VoiceBankBindingCode::StaleGeneration,
                  "voice-bank-retired-generation-refusal");
        }
    }
    SlotIntent second;
    check(find_live_slot(*bank, second) && second.state.note == 64 &&
              second.state.note_id != first.state.note_id && !second.releasing,
          "voice-bank-reuse-generation");
    obs.second_slot = second.slot;
    obs.second_generation = second.state.note_id;
    // The retired generation stays stale on the slot reused by the new note.
    auto old_on_reused = first.state;
    old_on_reused.pressure = 0.5f;
    check(bank->dispatch_deferred(bank->epoch(), second.slot, first.state.note_id,
                                  pulp::midi::MpeExpressionEvent::Kind::Pressure,
                                  old_on_reused) == VoiceBankBindingCode::StaleGeneration,
          "voice-bank-stale-generation-refusal");
    auto live = second.state;
    live.pressure = 0.5f;
    check(bank->dispatch_deferred(bank->epoch(), second.slot, second.state.note_id,
                                  pulp::midi::MpeExpressionEvent::Kind::Pressure,
                                  live) == VoiceBankBindingCode::Ok,
          "voice-bank-live-generation-accept");
    check(bank->dispatch_deferred(bank->epoch(), second.slot, second.state.note_id,
                                  pulp::midi::MpeExpressionEvent::Kind::NoteOff,
                                  second.state) == VoiceBankBindingCode::Ok,
          "voice-bank-deferred-release-accept");
    SlotIntent released;
    check(find_live_slot(*bank, released) && released.releasing &&
              released.state.note_id == second.state.note_id,
          "voice-bank-deferred-release-state");
    check(bank->dispatch_deferred(bank->epoch() + 1, second.slot, second.state.note_id,
                                  pulp::midi::MpeExpressionEvent::Kind::Pressure,
                                  live) == VoiceBankBindingCode::StaleEpoch,
          "voice-bank-stale-epoch-refusal");
    return obs;
}

void voice_bank_controls(const VoiceObservation& obs) {
    check(obs.audio.size() == kVoiceFrames, "voice-bank-audio-length");
    check(obs.audio[kVoiceNoteOn - 1] == 0.0f && obs.audio[kVoiceNoteOn] > 0.0f,
          "voice-bank-note-on-sample");
    check(obs.audio[kVoiceNoteOff + P1Voice::kReleaseFrames] == 0.0f &&
              obs.audio[kVoiceNoteOff + 1] < obs.audio[kVoiceNoteOff],
          "voice-bank-release-audio");
    check(obs.audio[kVoiceReuse - 1] == 0.0f && obs.audio[kVoiceReuse] > 0.0f,
          "voice-bank-reuse-sample");
}

// ── Combined epoch: four families on one block grid ────────────────────────
// One test-owned epoch and absolute sample origin drive all four families
// through the same block boundaries in one control loop. A control-side
// adapter stages MIDI in a HostEventIngress, projects each block, and converts
// the absolute positions to block-relative offsets for SignalGraph::inject_midi
// into a MIDI input node. Parameter records are converted to block-relative
// ParameterEventQueue offsets for inject_parameter_events into a PluginSlot
// node in the same graph. A graph-authored lane producer in that graph emits at
// absolute targets, and voice intent is published to a VoiceBankBinding over
// the same blocks. The runtime has no shared epoch: each native epoch is
// mapped to the test epoch and must stay constant for the whole run.

constexpr std::uint64_t kCombinedEpoch = 7;
constexpr std::uint64_t kCombinedFrames = 256;
constexpr std::size_t kCombinedMaxBlock = 128;

enum class Family : std::uint8_t { Midi = 0, Parameter = 1, Trigger = 2, Voice = 3 };
constexpr std::array<const char*, 4> kFamilyNames{"midi", "parameter", "trigger", "voice"};

// One canonical observation. `key` is the family's identity and ordering key:
//   midi      {producer, producer_sequence, status << 8 | note}
//   parameter {param_id, push index, 0}
//   trigger   {lane sequence, payload, producer}
//   voice     {slot, generation, 1 for note-on / 0 for release}
struct TraceRecord {
    Family family = Family::Midi;
    std::uint64_t epoch = 0;
    std::uint64_t absolute = 0;
    std::array<std::uint64_t, 3> key{};
    friend bool operator==(const TraceRecord&, const TraceRecord&) = default;
};

// The key position each family orders equal-time records by.
constexpr std::array<std::size_t, 4> kOrderKey{1, 1, 0, 1};

struct MidiScheduleEntry {
    std::uint64_t absolute;
    std::uint32_t producer;
    std::uint64_t sequence;
    std::uint8_t note;
};
constexpr std::array<MidiScheduleEntry, 8> kMidiSchedule{{{2, 4, 1, 60},
                                                          {2, 3, 1, 61},
                                                          {31, 3, 2, 62},
                                                          {32, 4, 2, 63},
                                                          {127, 3, 3, 64},
                                                          {128, 4, 3, 65},
                                                          {200, 3, 4, 66},
                                                          {255, 4, 4, 67}}};
// HostEventIngress orders equal samples by producer, so producer 3 precedes 4.
constexpr std::array<std::uint8_t, 8> kMidiExpectedNotes{61, 60, 62, 63, 64, 65, 66, 67};

struct ParameterScheduleEntry {
    std::uint64_t absolute;
    std::uint32_t index;
};
constexpr std::array<ParameterScheduleEntry, 7> kParameterSchedule{
    {{5, 0}, {31, 1}, {32, 2}, {32, 3}, {100, 4}, {127, 5}, {254, 6}}};
constexpr float kParameterStep = 1.0f / 16.0f;

struct TriggerScheduleEntry {
    std::uint64_t target;
    std::uint16_t payload;
};
// Each trigger is emitted kTriggerLead samples before its target, so small
// blocks stage it as bounded-future and large blocks deliver it in the
// producer's own block.
constexpr std::uint64_t kTriggerLead = 3;
constexpr std::array<TriggerScheduleEntry, 8> kTriggerSchedule{
    {{1, 40}, {4, 41}, {4, 42}, {33, 43}, {63, 44}, {64, 45}, {130, 46}, {250, 47}}};
constexpr std::uint64_t trigger_emit_sample(const TriggerScheduleEntry& entry) {
    return entry.target >= kTriggerLead ? entry.target - kTriggerLead : 0;
}

struct VoiceScheduleEntry {
    std::uint64_t absolute;
    bool note_on;
    std::uint8_t note;
};
constexpr std::array<VoiceScheduleEntry, 4> kVoiceSchedule{
    {{3, true, 60}, {40, true, 64}, {70, false, 60}, {200, false, 64}}};

struct CombinedRun {
    std::vector<TraceRecord> trace;
    std::size_t blocks = 0;
    std::size_t trigger_future_staged = 0;
    std::size_t trigger_current_block = 0;
    // Native epoch of each family, mapped to kCombinedEpoch. The parameter
    // mailbox is block-relative and has no epoch, so its entry is 0.
    std::array<std::uint64_t, 4> native_epoch{};
    std::array<float, kCombinedFrames> voice_audio{};
    // Graph output across the whole run; the input is a deterministic ramp, so
    // this is a non-silent audio comparison for the routed executor.
    std::vector<float> graph_audio;
    // Executor telemetry read after the run: blocks the serial executor took,
    // and blocks that were eligible yet fell back to the legacy walk.
    std::uint64_t executor_blocks = 0;
    std::uint64_t parallel_levels = 0;
    std::uint64_t routing_fallbacks = 0;
    bool parallel_requested_accepted = false;
};

enum class Backend { Reference, Routed, ParallelAfterBind };
constexpr std::array<const char*, 3> kBackendNames{"reference-walk", "routed-executor",
                                                   "parallel-after-bind"};

// Callback-side state. The audio callbacks write only into these fixed
// arrays; nothing in the callback path grows a container.
struct CombinedCallbackState {
    struct MidiSeen {
        std::uint64_t absolute = 0;
        std::uint8_t status = 0;
        std::uint8_t note = 0;
    };
    std::size_t next_trigger = 0;
    std::uint64_t producer_cursor = 0;
    std::array<GraphEventResult, kTriggerSchedule.size()> emit_results{};
    std::array<bool, kTriggerSchedule.size()> emit_future{};
    std::uint64_t consumer_cursor = 0;
    std::array<pulp::host::GraphAuthoredEvent, 16> triggers{};
    std::array<std::uint64_t, 16> trigger_block_start{};
    std::array<std::uint64_t, 16> trigger_block_end{};
    std::size_t trigger_count = 0;
    std::array<MidiSeen, 16> midi{};
    std::size_t midi_count = 0;
    bool dropped = false;
};

// Everything one combined audio block touches. Spans and buffers are owned and
// preallocated by the caller before the first block.
struct CombinedBlockIo {
    pulp::host::SignalGraph& graph;
    HostEventIngress<32>& ingress;
    std::span<HostEventRecord> projected;
    pulp::midi::MidiBuffer& block_midi;
    pulp::host::NodeId midi_input;
    pulp::host::NodeId plugin;
    const pulp::state::ParameterEventQueue& parameters;
    pulp::audio::BufferView<float>& out;
    const pulp::audio::BufferView<const float>& in;
    pulp::host::VoiceBankBinding& bank;
    std::span<const HostEventRecord> voice_records;
    pulp::audio::BufferView<float>& voice_out;
    std::uint64_t start = 0;
    std::size_t frames = 0;
};

// Result codes recorded inside the boundary and checked after it returns.
struct CombinedBlockResult {
    HostEventResult midi_project = HostEventResult::Accepted;
    HostEventResult midi_end = HostEventResult::Accepted;
    bool midi_adapter_capacity = true;
    bool midi_injected = true;
    bool parameters_injected = true;
    HostEventResult voice_begin = HostEventResult::Accepted;
    std::size_t voice_published = 0;
    pulp::host::VoiceBankBindingCode voice_render = pulp::host::VoiceBankBindingCode::Ok;
};

// The audio side of one combined block: MIDI projection and injection, the
// parameter mailbox, the graph callback (which runs the graph-authored lane),
// and the voice-bank publish and render.
CombinedBlockResult combined_audio_block(const CombinedBlockIo& io,
                                         AnalyzerControl control) noexcept P1_RTSAN_BOUNDARY {
#if defined(P1_HAS_RTSAN)
    inject_analyzer_control(control);
#else
    (void)control;
#endif
    CombinedBlockResult result;
    std::size_t count = 0;
    result.midi_project = io.ingress.project(io.projected, count);
    io.block_midi.clear();
    for (std::size_t i = 0; i < count; ++i) {
        auto event = io.projected[i].midi;
        event.sample_offset = static_cast<std::int32_t>(
            static_cast<std::uint64_t>(io.projected[i].absolute_sample) - io.start);
        result.midi_adapter_capacity &= io.block_midi.add(event);
    }
    if (count > 0)
        result.midi_injected = io.graph.inject_midi(io.midi_input, io.block_midi);
    result.midi_end = io.ingress.end_block();

    if (!io.parameters.empty())
        result.parameters_injected = io.graph.inject_parameter_events(io.plugin, io.parameters);

    io.graph.process(io.out, io.in, static_cast<int>(io.frames));

    result.voice_begin = io.bank.begin_block(io.start, io.frames);
    for (const auto& record : io.voice_records)
        result.voice_published += io.bank.publish(record) == HostEventResult::Accepted ? 1u : 0u;
    result.voice_render = io.bank.render(io.voice_out, io.start, io.frames);
    return result;
}

CombinedRun render_combined(const std::vector<std::size_t>& schedule,
                            Backend backend = Backend::Routed) {
    using namespace pulp::host;
    auto state = std::make_shared<CombinedCallbackState>();
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "p1.public.combined-epoch";
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [state](auto& out, const auto& in, int frames,
                                  const CustomNodeEventBlock& events) {
        inject_callee_control(AnalyzerControl::NodeMalloc);
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        auto& s = *state;
        const auto block_frames = static_cast<std::uint64_t>(frames);
        if (events.graph_writer != nullptr) {
            const auto block_end = s.producer_cursor + block_frames;
            while (s.next_trigger < kTriggerSchedule.size() &&
                   trigger_emit_sample(kTriggerSchedule[s.next_trigger]) < block_end) {
                const auto& entry = kTriggerSchedule[s.next_trigger];
                s.emit_results[s.next_trigger] =
                    events.graph_writer->emit_at(entry.target, entry.payload);
                s.emit_future[s.next_trigger] = entry.target >= block_end;
                ++s.next_trigger;
            }
            s.producer_cursor = block_end;
        }
        if (events.graph_reader != nullptr) {
            const auto block_end = s.consumer_cursor + block_frames;
            for (const auto& event : events.graph_reader->events()) {
                if (s.trigger_count == s.triggers.size()) {
                    s.dropped = true;
                    continue;
                }
                s.triggers[s.trigger_count] = event;
                s.trigger_block_start[s.trigger_count] = s.consumer_cursor;
                s.trigger_block_end[s.trigger_count] = block_end;
                ++s.trigger_count;
            }
            if (events.in != nullptr) {
                for (const auto& event : *events.in) {
                    if (s.midi_count == s.midi.size()) {
                        s.dropped = true;
                        continue;
                    }
                    s.midi[s.midi_count++] = {s.consumer_cursor +
                                                  static_cast<std::uint64_t>(event.sample_offset),
                                              event.data()[0], event.data()[1]};
                }
            }
            s.consumer_cursor = block_end;
        }
    };
    check(graph.register_custom_node_type(type), "combined-register");
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    const auto producer = graph.add_custom_node(type.type_id);
    const auto consumer = graph.add_custom_node(type.type_id);
    const auto midi_input = graph.add_midi_input_node("P1 combined MIDI input");
    auto slot = std::make_unique<ParameterProbeSlot>();
    auto* probe = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 0, 1, "p1-combined-parameter");
    check(graph.connect(input, 0, producer, 0) && graph.connect(producer, 0, consumer, 0) &&
              graph.connect(consumer, 0, output, 0) && graph.connect_midi(midi_input, consumer) &&
              graph.connect_graph_event({producer, consumer}) == GraphEventBindingResult::Accepted,
          "combined-admission");
    // Backend selection happens before prepare(). Parallel routing is
    // requested after the graph-authored lane is bound; the runtime refuses it
    // there, so the request is recorded rather than forced.
    graph.set_canonical_executor_routing_enabled(backend != Backend::Reference);
    graph.set_parallel_routing_enabled(backend == Backend::ParallelAfterBind);
    CombinedRun run;
    run.parallel_requested_accepted = graph.parallel_routing_enabled();
    check(graph.canonical_executor_routing_enabled() == (backend != Backend::Reference),
          "combined-backend-selected");
    check(graph.prepare(48000.0, static_cast<int>(kCombinedMaxBlock)), "combined-prepare");
    auto bank = make_voice_bank<P1HeldVoice>(static_cast<int>(kCombinedMaxBlock));
    const auto voice_epoch = bank->epoch();

    // Control-side adapter storage, preallocated before the first block.
    HostEventIngress<32> ingress;
    std::array<HostEventRecord, 32> projected{};
    pulp::midi::MidiBuffer block_midi;
    block_midi.reserve(32);
    block_midi.set_realtime_capacity_limit(true);
    pulp::state::ParameterEventQueue parameters;
    std::array<float, kCombinedMaxBlock> source{}, destination{}, left{}, right{};
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    std::array<float*, 2> voice_ptrs{left.data(), right.data()};
    std::array<HostEventRecord, kVoiceSchedule.size()> voice_records{};

    // Slot state is read after each block's render, so a note-on whose
    // release lands in the same block is already releasing when observed.
    struct VoiceSeen {
        std::size_t entry;
        SlotIntent intent;
        bool released_in_block;
    };
    std::vector<VoiceSeen> voice_seen;
    std::size_t next_parameter = 0;
    std::size_t next_voice = 0;
    std::uint64_t voice_sequence = 1;
    std::uint64_t start = 0;
    for (const auto frames : schedule) {
        check(frames > 0 && frames <= kCombinedMaxBlock, "combined-block-size");
        const auto end = start + frames;

        // MIDI: absolute records staged once, projected per block, converted
        // to block-relative offsets and injected into the MIDI input node.
        check(ingress.begin_block(kCombinedEpoch, start, frames) == HostEventResult::Accepted,
              "combined-midi-begin");
        if (run.blocks == 0) {
            for (const auto& entry : kMidiSchedule)
                check(ingress.publish(midi_record(
                          kCombinedEpoch, static_cast<std::int64_t>(entry.absolute), entry.sequence,
                          entry.producer, pulp::midi::MidiEvent::note_on(1, entry.note, 100))) ==
                          HostEventResult::Accepted,
                      "combined-midi-stage");
            check(ingress.publish(midi_record(kCombinedEpoch - 1, 3, 9, 3,
                                              pulp::midi::MidiEvent::note_on(1, 70, 100))) ==
                      HostEventResult::StaleEpoch,
                  "combined-midi-stale-epoch-refusal");
        }
        // Parameter: absolute records converted to block-relative offsets.
        parameters.clear();
        while (next_parameter < kParameterSchedule.size() &&
               kParameterSchedule[next_parameter].absolute < end) {
            const auto& entry = kParameterSchedule[next_parameter++];
            check(
                parameters.push({.param_id = ParameterProbeSlot::kParamId,
                                 .sample_offset = static_cast<std::int32_t>(entry.absolute - start),
                                 .value = static_cast<float>(entry.index) * kParameterStep}),
                "combined-parameter-push");
        }
        pulp::audio::BufferView<const float> in(&source_ptr, 1, frames);
        pulp::audio::BufferView<float> out(&destination_ptr, 1, frames);
        for (std::size_t i = 0; i < frames; ++i)
            source[i] = 0.001f * static_cast<float>((start + i) % 977);

        // Voice intent on the same block boundaries, published by the audio
        // owner inside the boundary below.
        check(bank->epoch() == voice_epoch, "combined-epoch-map");
        const auto first_voice = next_voice;
        std::size_t voice_count = 0;
        while (next_voice < kVoiceSchedule.size() && kVoiceSchedule[next_voice].absolute < end) {
            const auto& entry = kVoiceSchedule[next_voice++];
            auto& record = voice_records[voice_count++];
            record = HostEventRecord{};
            record.epoch = voice_epoch;
            record.absolute_sample = static_cast<std::int64_t>(entry.absolute);
            record.producer = 1;
            record.producer_sequence = voice_sequence++;
            record.midi = entry.note_on ? pulp::midi::MidiEvent::note_on(1, entry.note, 100)
                                        : pulp::midi::MidiEvent::note_off(1, entry.note);
        }
        pulp::audio::BufferView<float> voice_view(voice_ptrs.data(), 2, frames);

        const CombinedBlockIo io{graph,
                                 ingress,
                                 projected,
                                 block_midi,
                                 midi_input,
                                 plugin,
                                 parameters,
                                 out,
                                 in,
                                 *bank,
                                 {voice_records.data(), voice_count},
                                 voice_view,
                                 start,
                                 frames};
        const auto block = combined_audio_block(io, planted_control);
        ++realtime_boundary_blocks;

        check(block.midi_project == HostEventResult::Accepted, "combined-midi-project");
        check(block.midi_adapter_capacity, "combined-midi-adapter-capacity");
        check(block.midi_injected, "combined-midi-inject");
        check(block.midi_end == HostEventResult::Accepted, "combined-midi-end");
        check(block.parameters_injected, "combined-parameter-inject");
        check(block.voice_begin == HostEventResult::Accepted, "combined-voice-begin");
        check(block.voice_published == voice_count, "combined-voice-publish");
        check(block.voice_render == VoiceBankBindingCode::Ok, "combined-voice-render");
        run.graph_audio.insert(run.graph_audio.end(), destination.begin(),
                               destination.begin() + static_cast<std::ptrdiff_t>(frames));
        std::copy_n(left.begin(), frames,
                    run.voice_audio.begin() + static_cast<std::ptrdiff_t>(start));
        for (std::size_t k = first_voice; k < next_voice; ++k) {
            SlotIntent found;
            bool located = false;
            for (std::uint32_t s = 0; s < 8 && !located; ++s) {
                pulp::midi::MpeNoteState note{};
                bool releasing = false;
                if (bank->owner().slot_note(s, note, releasing) &&
                    note.note == kVoiceSchedule[k].note) {
                    found = {s, note, releasing};
                    located = true;
                }
            }
            check(located, "combined-voice-slot");
            bool released_in_block = false;
            for (std::size_t j = first_voice; j < next_voice; ++j)
                released_in_block |=
                    !kVoiceSchedule[j].note_on && kVoiceSchedule[j].note == kVoiceSchedule[k].note;
            voice_seen.push_back({k, found, released_in_block});
        }
        start = end;
        ++run.blocks;
    }
    check(start == kCombinedFrames, "combined-schedule-length");
    {
        const auto stats = graph.routing_executor_stats();
        run.executor_blocks = stats.blocks_processed;
        run.parallel_levels = stats.parallel_levels_dispatched;
        run.routing_fallbacks = graph.routed_walk_fallbacks();
    }

    const auto& s = *state;
    check(!s.dropped && !probe->dropped(), "combined-observation-capacity");

    // MIDI
    check(s.midi_count == kMidiSchedule.size(), "combined-midi-count");
    for (std::size_t i = 0; i < s.midi_count; ++i) {
        const auto& seen = s.midi[i];
        check(seen.note == kMidiExpectedNotes[i], "combined-midi-tie-order");
        const auto entry = std::find_if(kMidiSchedule.begin(), kMidiSchedule.end(),
                                        [&](const auto& e) { return e.note == seen.note; });
        check(entry != kMidiSchedule.end() && entry->absolute == seen.absolute,
              "combined-midi-absolute");
        run.trace.push_back(
            {Family::Midi,
             kCombinedEpoch,
             seen.absolute,
             {entry->producer, entry->sequence, (std::uint64_t{seen.status} << 8) | seen.note}});
    }

    // Parameter
    const auto entries = probe->entries();
    check(entries.size() == kParameterSchedule.size(), "combined-parameter-count");
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto index =
            static_cast<std::uint64_t>(entries[i].event.value / kParameterStep + 0.5f);
        check(index == kParameterSchedule[i].index &&
                  entries[i].absolute == kParameterSchedule[i].absolute,
              "combined-parameter-order");
        run.trace.push_back({Family::Parameter,
                             kCombinedEpoch,
                             entries[i].absolute,
                             {entries[i].event.param_id, index, 0}});
    }

    // Trigger
    for (std::size_t i = 0; i < kTriggerSchedule.size(); ++i) {
        check(s.emit_results[i] == GraphEventResult::Accepted, "combined-trigger-emit");
        ++(s.emit_future[i] ? run.trigger_future_staged : run.trigger_current_block);
    }
    check(s.trigger_count == kTriggerSchedule.size(), "combined-trigger-count");
    const auto trigger_epoch = s.triggers[0].epoch;
    run.native_epoch = {kCombinedEpoch, 0, trigger_epoch, voice_epoch};
    for (std::size_t i = 0; i < s.trigger_count; ++i) {
        const auto& event = s.triggers[i];
        check(event.epoch == trigger_epoch, "combined-epoch-map");
        check(event.sample == kTriggerSchedule[i].target &&
                  event.payload == kTriggerSchedule[i].payload,
              "combined-trigger-order");
        check(s.trigger_block_start[i] <= event.sample && event.sample < s.trigger_block_end[i],
              "combined-trigger-block-window");
        run.trace.push_back({Family::Trigger,
                             kCombinedEpoch,
                             event.sample,
                             {event.sequence, event.payload, event.producer}});
    }

    // Voice
    check(voice_seen.size() == kVoiceSchedule.size(), "combined-voice-count");
    for (const auto& seen : voice_seen) {
        const auto& entry = kVoiceSchedule[seen.entry];
        if (entry.note_on) {
            check(seen.intent.state.note_id != 0 &&
                      (!seen.intent.releasing || seen.released_in_block),
                  "combined-voice-note-on");
        } else {
            const auto on = std::find_if(voice_seen.begin(), voice_seen.end(), [&](const auto& v) {
                return kVoiceSchedule[v.entry].note_on &&
                       kVoiceSchedule[v.entry].note == entry.note;
            });
            check(on != voice_seen.end() && seen.intent.releasing &&
                      seen.intent.slot == on->intent.slot &&
                      seen.intent.state.note_id == on->intent.state.note_id,
                  "combined-voice-release-intent");
        }
        run.trace.push_back(
            {Family::Voice,
             kCombinedEpoch,
             entry.absolute,
             {seen.intent.slot, seen.intent.state.note_id, entry.note_on ? 1u : 0u}});
    }
    // The rendered audio places each intent at its absolute sample, not at the
    // start of the block that carried it.
    const auto& audio = run.voice_audio;
    check(audio[2] == 0.0f && audio[3] > 0.0f && audio[40] > audio[39] && audio[71] < audio[69] &&
              audio[201] < audio[199],
          "combined-voice-onset-sample");
    // A live generation on another slot is stale for the first note's slot.
    const auto& first_on = voice_seen[0].intent;
    const auto& second_on = voice_seen[1].intent;
    check(first_on.slot != second_on.slot || first_on.state.note_id != second_on.state.note_id,
          "combined-voice-distinct-identity");
    check(bank->dispatch_deferred(bank->epoch(), first_on.slot, second_on.state.note_id,
                                  pulp::midi::MpeExpressionEvent::Kind::Pressure,
                                  first_on.state) == VoiceBankBindingCode::StaleGeneration,
          "combined-voice-stale-generation-refusal");

    std::stable_sort(run.trace.begin(), run.trace.end(),
                     [](const TraceRecord& a, const TraceRecord& b) {
                         if (a.absolute != b.absolute)
                             return a.absolute < b.absolute;
                         return a.family < b.family;
                     });
    return run;
}

// FNV-1a over each family's canonical records, in canonical order. The digest
// is order-sensitive, so a changed order key or a reordered pair changes it.
std::array<std::uint64_t, 4> family_digests(const std::vector<TraceRecord>& trace) {
    std::array<std::uint64_t, 4> digest{};
    digest.fill(1469598103934665603ull);
    const auto mix = [](std::uint64_t& h, std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (i * 8)) & 0xffu;
            h *= 1099511628211ull;
        }
    };
    for (const auto& record : trace) {
        auto& h = digest[static_cast<std::size_t>(record.family)];
        mix(h, record.epoch);
        mix(h, record.absolute);
        for (const auto value : record.key)
            mix(h, value);
    }
    return digest;
}

std::vector<std::size_t> differing_records(const std::vector<TraceRecord>& a,
                                           const std::vector<TraceRecord>& b) {
    std::vector<std::size_t> diff;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (!(a[i] == b[i]))
            diff.push_back(i);
    return diff;
}

struct MutationReceipt {
    std::array<std::uint64_t, 4> baseline{};
    std::array<std::array<std::uint64_t, 4>, 4> sequence_mutated{};
    std::array<std::uint64_t, 4> order_swapped{};
};

// Planted negatives: change one record's order key in each family, and swap the
// two equal-sample MIDI records. Exactly the touched family's digest changes,
// the other three stay identical, and a record-by-record diff names only the
// touched records, so every other record stays explainable.
MutationReceipt sequence_mutation_controls(const std::vector<TraceRecord>& trace) {
    MutationReceipt receipt;
    receipt.baseline = family_digests(trace);
    for (std::size_t f = 0; f < 4; ++f) {
        auto mutated = trace;
        const auto target = std::find_if(mutated.begin(), mutated.end(), [&](const auto& r) {
            return static_cast<std::size_t>(r.family) == f;
        });
        check(target != mutated.end(), "sequence-mutation-target");
        target->key[kOrderKey[f]] += 1;
        const auto digests = family_digests(mutated);
        receipt.sequence_mutated[f] = digests;
        check(digests[f] != receipt.baseline[f], "sequence-mutation-detected");
        for (std::size_t other = 0; other < 4; ++other)
            if (other != f)
                check(digests[other] == receipt.baseline[other], "sequence-mutation-isolated");
        const auto diff = differing_records(trace, mutated);
        check(diff.size() == 1 && diff[0] == static_cast<std::size_t>(target - mutated.begin()),
              "sequence-mutation-explained");
    }
    auto swapped = trace;
    std::size_t first = swapped.size();
    for (std::size_t i = 0; i + 1 < swapped.size(); ++i)
        if (swapped[i].family == Family::Midi && swapped[i + 1].family == Family::Midi &&
            swapped[i].absolute == swapped[i + 1].absolute) {
            first = i;
            break;
        }
    check(first < swapped.size(), "order-mutation-target");
    std::swap(swapped[first], swapped[first + 1]);
    receipt.order_swapped = family_digests(swapped);
    check(receipt.order_swapped[0] != receipt.baseline[0], "order-mutation-detected");
    for (std::size_t other = 1; other < 4; ++other)
        check(receipt.order_swapped[other] == receipt.baseline[other], "order-mutation-isolated");
    const auto diff = differing_records(trace, swapped);
    check(diff == std::vector<std::size_t>{first, first + 1}, "order-mutation-explained");
    return receipt;
}

// Names the first channel on which two backends disagree, or "" when the
// canonical trace, per-family digests, voice audio, graph audio and epoch map
// are all identical.
const char* backend_parity_diff(const CombinedRun& a, const CombinedRun& b) {
    if (!(a.trace == b.trace))
        return "backend-trace";
    if (family_digests(a.trace) != family_digests(b.trace))
        return "backend-digest";
    if (!(a.voice_audio == b.voice_audio))
        return "backend-voice-audio";
    if (!(a.graph_audio == b.graph_audio))
        return "backend-graph-audio";
    if (!(a.native_epoch == b.native_epoch))
        return "backend-epoch-map";
    return "";
}

struct ParityReceipt {
    std::size_t comparisons = 0;
    std::uint64_t reference_executor_blocks = 0;
    std::uint64_t routed_executor_blocks = 0;
    std::uint64_t routed_blocks_total = 0;
    std::uint64_t routing_fallbacks = 0;
    bool parallel_refused = false;
    std::uint64_t parallel_levels = 0;
    std::array<std::uint64_t, 4> perturbed_digests{};
    std::size_t audio_perturbed_index = 0;
};

// Runs every schedule on the reference walk and the routed executor (and the
// parallel request) and requires identical output. Planted negatives perturb
// one record, then one audio sample, on the routed side only; the comparator
// must name each.
ParityReceipt executor_parity_controls(const std::vector<std::vector<std::size_t>>& schedules) {
    ParityReceipt receipt;
    for (const auto& schedule : schedules) {
        const auto reference = render_combined(schedule, Backend::Reference);
        const auto routed = render_combined(schedule, Backend::Routed);
        const auto parallel = render_combined(schedule, Backend::ParallelAfterBind);
        check(reference.executor_blocks == 0, "parity-reference-walk-only");
        check(routed.executor_blocks == schedule.size(), "parity-routed-executor-dispatched");
        check(routed.routing_fallbacks == 0 && parallel.routing_fallbacks == 0,
              "parity-no-routed-fallback");
        check(!parallel.parallel_requested_accepted, "parity-parallel-refused-after-bind");
        check(parallel.parallel_levels == 0, "parity-parallel-not-dispatched");
        check(std::string(backend_parity_diff(reference, routed)).empty(),
              "parity-reference-vs-routed");
        check(std::string(backend_parity_diff(reference, parallel)).empty(),
              "parity-reference-vs-parallel-request");
        check(std::string(backend_parity_diff(routed, parallel)).empty(),
              "parity-routed-vs-parallel-request");
        receipt.comparisons += 3;
        receipt.reference_executor_blocks += reference.executor_blocks;
        receipt.routed_executor_blocks += routed.executor_blocks;
        receipt.routed_blocks_total += schedule.size();
        receipt.routing_fallbacks += routed.routing_fallbacks + parallel.routing_fallbacks;
        receipt.parallel_refused = !parallel.parallel_requested_accepted;
        receipt.parallel_levels += parallel.parallel_levels;
    }
    const auto reference = render_combined(schedules[0], Backend::Reference);
    const auto routed = render_combined(schedules[0], Backend::Routed);
    check(std::string(backend_parity_diff(reference, routed)).empty(), "parity-control-baseline");
    auto record_perturbed = routed;
    const auto target = std::find_if(record_perturbed.trace.begin(), record_perturbed.trace.end(),
                                     [](const auto& r) { return r.family == Family::Parameter; });
    check(target != record_perturbed.trace.end(), "parity-perturb-target");
    target->key[kOrderKey[1]] += 1;
    check(std::string(backend_parity_diff(reference, record_perturbed)) == "backend-trace",
          "parity-planted-record-detected");
    receipt.perturbed_digests = family_digests(record_perturbed.trace);
    const auto base_digests = family_digests(reference.trace);
    check(receipt.perturbed_digests[1] != base_digests[1] &&
              receipt.perturbed_digests[0] == base_digests[0] &&
              receipt.perturbed_digests[2] == base_digests[2] &&
              receipt.perturbed_digests[3] == base_digests[3],
          "parity-planted-record-isolated");
    auto audio_perturbed = routed;
    receipt.audio_perturbed_index = audio_perturbed.graph_audio.size() / 2;
    audio_perturbed.graph_audio[receipt.audio_perturbed_index] += 1.0f;
    check(std::string(backend_parity_diff(reference, audio_perturbed)) == "backend-graph-audio",
          "parity-planted-audio-detected");
    auto voice_perturbed = routed;
    voice_perturbed.voice_audio[7] += 1.0f;
    check(std::string(backend_parity_diff(reference, voice_perturbed)) == "backend-voice-audio",
          "parity-planted-voice-audio-detected");
    return receipt;
}

std::string hex(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof buffer, "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

void write_trace(const std::string& path, const CombinedRun& run, const MutationReceipt& receipt) {
    std::ofstream file(path);
    check(static_cast<bool>(file), "trace-write");
    file << "{\"test_epoch\":" << kCombinedEpoch << ",\"native_epoch\":{";
    for (std::size_t f = 0; f < 4; ++f)
        file << (f ? "," : "") << '"' << kFamilyNames[f] << "\":" << run.native_epoch[f];
    file << "}}\n";
    for (const auto& r : run.trace)
        file << "{\"family\":\"" << kFamilyNames[static_cast<std::size_t>(r.family)]
             << "\",\"epoch\":" << r.epoch << ",\"absolute\":" << r.absolute << ",\"key\":["
             << r.key[0] << ',' << r.key[1] << ',' << r.key[2] << "]}\n";
    file << "{\"digests\":{";
    for (std::size_t f = 0; f < 4; ++f)
        file << (f ? "," : "") << '"' << kFamilyNames[f] << "\":\"" << hex(receipt.baseline[f])
             << '"';
    file << "}}\n";
    const auto digest_list = [&](const std::array<std::uint64_t, 4>& d) {
        file << '[';
        for (std::size_t f = 0; f < 4; ++f)
            file << (f ? "," : "") << '"' << hex(d[f]) << '"';
        file << ']';
    };
    file << "{\"sequence_mutation\":{";
    for (std::size_t f = 0; f < 4; ++f) {
        file << (f ? "," : "") << '"' << kFamilyNames[f] << "\":";
        digest_list(receipt.sequence_mutated[f]);
    }
    file << "},\"midi_order_swap\":";
    digest_list(receipt.order_swapped);
    file << "}\n";
    check(static_cast<bool>(file), "trace-write");
}

} // namespace

int main(int argc, char** argv) {
    try {
        planted_control = analyzer_control();
        const auto partition4 = host_and_parameter_controls(4);
        const auto partition8 = host_and_parameter_controls(8);
        check(partition4 == partition8, "partition-invariant-event-output");
        trigger_two_block_controls();

        GraphLaneObservation lane_a;
        GraphLaneObservation lane_b;
        const auto graph_events = graph_event_lane_controls({2, 2, 4}, lane_a);
        graph_event_lane_controls({3, 5}, lane_b);
        check(lane_a.events == lane_b.events && lane_a.audio == lane_b.audio,
              "graph-event-partition-invariance");
        graph_event_reset_controls();

        const auto parameters4 = render_parameter_mailbox(4);
        const auto parameters8 = render_parameter_mailbox(8);
        parameter_mailbox_controls(parameters4);
        parameter_mailbox_controls(parameters8);
        check(parameters4.blocks == parameters8.blocks, "parameter-mailbox-partition-invariance");

        const auto voice4 = render_voice_bank(4);
        const auto voice8 = render_voice_bank(8);
        voice_bank_controls(voice4);
        voice_bank_controls(voice8);
        check(voice4.audio == voice8.audio && voice4.first_slot == voice8.first_slot &&
                  voice4.second_slot == voice8.second_slot &&
                  voice4.first_generation == voice8.first_generation &&
                  voice4.second_generation == voice8.second_generation,
              "voice-bank-partition-invariance");

        // One shared epoch and sample origin across all four families, at
        // uniform and irregular block schedules over the same 256 samples.
        const std::vector<std::vector<std::size_t>> combined_schedules{
            std::vector<std::size_t>(256, 1),
            std::vector<std::size_t>(8, 32),
            std::vector<std::size_t>(4, 64),
            {127, 127, 2},
            {3, 61, 1, 127, 64}};
        const auto combined = render_combined(combined_schedules[0]);
        std::size_t future_staged = combined.trigger_future_staged;
        std::size_t current_block = combined.trigger_current_block;
        for (std::size_t i = 1; i < combined_schedules.size(); ++i) {
            const auto other = render_combined(combined_schedules[i]);
            check(other.trace == combined.trace && other.voice_audio == combined.voice_audio,
                  "combined-partition-invariance");
            check(family_digests(other.trace) == family_digests(combined.trace),
                  "combined-partition-invariance");
            check(other.native_epoch == combined.native_epoch, "combined-epoch-map");
            future_staged += other.trigger_future_staged;
            current_block += other.trigger_current_block;
        }
        check(future_staged > 0 && current_block > 0, "combined-trigger-current-and-future");
        const auto receipt = sequence_mutation_controls(combined.trace);
        const auto parity = executor_parity_controls(combined_schedules);
        check(parity.comparisons == 3 * combined_schedules.size(), "parity-comparison-count");
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--trace")
                write_trace(argv[i + 1], combined, receipt);

        std::cout << "p1-event-admission PASS checks=" << checks
                  << " partitions=4,8 graph_event_schedules=2+2+4,3+5 graph_events=" << graph_events
                  << " voice_slot_reuse=" << (voice4.first_slot == voice4.second_slot ? 1 : 0)
                  << " combined_schedules=1,32,64,127+127+2,3+61+1+127+64 combined_records="
                  << combined.trace.size() << " digests=midi:" << hex(receipt.baseline[0])
                  << ",parameter:" << hex(receipt.baseline[1])
                  << ",trigger:" << hex(receipt.baseline[2])
                  << ",voice:" << hex(receipt.baseline[3])
                  << " sequence_mutations=5 executor_parity=reference-walk,routed-executor"
                  << " parity_comparisons=" << parity.comparisons
                  << " routed_executor_blocks=" << parity.routed_executor_blocks << "/"
                  << parity.routed_blocks_total << " routing_fallbacks=" << parity.routing_fallbacks
                  << " parallel_after_bind=" << (parity.parallel_refused ? "refused" : "accepted")
                  << " parity_planted_negatives=3 realtime_boundary=" << kRealtimeBoundary
                  << " realtime_boundary_blocks=" << realtime_boundary_blocks
                  << " consumer_gate=NOT_QUALIFIED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p1-event-admission FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
