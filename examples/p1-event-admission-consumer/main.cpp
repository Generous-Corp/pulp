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
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
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
// audio callback.

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
                obs.events.push_back(event);
                obs.delivered_block.push_back(state->consumer_block);
                obs.delivered_block_start.push_back(state->consumer_cursor);
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
    return state->observation;
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
        std::vector<GraphAuthoredEvent> delivered;
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
                state->delivered.push_back(event);
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
    check(state->staged_current == GraphEventResult::Accepted &&
              state->staged_future == GraphEventResult::Accepted,
          "graph-event-reset-stage");
    // The record staged for sample 3 before the reset is retired with its
    // epoch; the post-reset block that covers sample 3 never delivers it.
    check(std::none_of(state->delivered.begin(), state->delivered.end(),
                       [](const GraphAuthoredEvent& event) { return event.payload == 2; }),
          "graph-event-reset-cancels-pending");
    check(state->post_reset == GraphEventResult::Accepted && state->delivered.size() == 2 &&
              state->delivered[0].payload == 1 && state->delivered[1].payload == 3,
          "graph-event-reset-post-emit");
    check(state->delivered[1].epoch == state->delivered[0].epoch + 1 &&
              state->delivered[1].sample == 2,
          "graph-event-reset-epoch-advance");
}

// ── Parameter mailbox through a PluginSlot node ────────────────────────────

class ParameterProbeSlot final : public pulp::host::PluginSlot {
  public:
    static constexpr std::uint32_t kParamId = 11;
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
        blocks_.emplace_back(events.begin(), events.end());
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
    const std::vector<std::vector<pulp::host::ParameterEvent>>& blocks() const {
        return blocks_;
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
    std::vector<std::vector<pulp::host::ParameterEvent>> blocks_;
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

    const auto& blocks = probe->blocks();
    check(blocks.size() == 4, "parameter-mailbox-block-count");
    for (std::size_t b = 0; b < 3; ++b) {
        std::vector<std::pair<int, float>> values;
        for (const auto& event : blocks[b]) {
            check(event.param_id == id, "parameter-mailbox-param-id");
            values.emplace_back(event.sample_offset, event.value);
        }
        observation.blocks.push_back(std::move(values));
    }
    observation.retained_prefix = blocks[3].size();
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

class P1Voice final : public pulp::midi::MpeSynthVoice {
  public:
    static constexpr unsigned kReleaseFrames = 8;
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

class P1VoiceProcessor final : public pulp::format::Processor {
  public:
    explicit P1VoiceProcessor(P1Voice& voice) : voice_(voice) {}
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
    P1Voice& voice_;
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

using P1VoiceOwner = pulp::host::MpeVoiceBankOwner<P1Voice>;

std::unique_ptr<pulp::host::VoiceBankBinding> make_voice_bank() {
    using namespace pulp::host;
    auto owner = std::make_unique<P1VoiceOwner>(
        [](P1Voice& voice, std::uint32_t) -> std::unique_ptr<pulp::format::Processor> {
            return std::make_unique<P1VoiceProcessor>(voice);
        },
        std::array<VoiceBankInputEndpoint, 4>{
            {{"note_in", 0}, {"pitch", 1}, {"pressure", 2}, {"timbre", 3}}});
    VoiceBankBindingResult status;
    auto bank = VoiceBankBinding::prepare(voice_schema(), voice_definition(), std::move(owner),
                                          "audio_out", 48000, 64, status);
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

} // namespace

int main() {
    try {
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

        std::cout << "p1-event-admission PASS checks=" << checks
                  << " partitions=4,8 graph_event_schedules=2+2+4,3+5 graph_events=" << graph_events
                  << " voice_slot_reuse=" << (voice4.first_slot == voice4.second_slot ? 1 : 0)
                  << " consumer_gate=NOT_QUALIFIED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p1-event-admission FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
