#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_event_contract.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <pulp/host/voice_identity.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>
#include <pulp/state/parameter_event_queue.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

void host_and_parameter_controls(std::size_t partition) {
    HostEventIngress<8> ingress;
    check(ingress.begin_block(7, 0, partition) == HostEventResult::Accepted, "midi-block-begin");
    auto note = midi_record(7, 2, 1, 4, pulp::midi::MidiEvent::note_on(1, 60, 100));
    auto tie = midi_record(7, 2, 1, 3, pulp::midi::MidiEvent::note_on(1, 61, 100));
    check(ingress.publish(note) == HostEventResult::Accepted &&
              ingress.publish(tie) == HostEventResult::Accepted,
          "midi-publish");
    std::array<HostEventRecord, 8> projected{};
    std::size_t count = 0;
    check(ingress.project(projected, count) == HostEventResult::Accepted && count == 2,
          "midi-project");
    check(projected[0].producer == 3 && projected[1].producer == 4, "midi-stable-producer-order");
    check(ingress.publish(midi_record(6, 2, 3, 1, pulp::midi::MidiEvent::note_on(1, 62, 100))) ==
              HostEventResult::StaleEpoch,
          "midi-stale-epoch-refusal");
    check(ingress.publish(midi_record(7, -1, 4, 1, pulp::midi::MidiEvent::note_off(1, 60))) ==
              HostEventResult::Late,
          "midi-late-refusal");
    check(ingress.end_block() == HostEventResult::Accepted, "midi-block-end");

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
    source = {0.0f, 0.0f, 1.0f, 0.0f};
    check(graph.process_checked(output_view, input_view, 4) == TriggerGraphResult::Accepted,
          "trigger-second-block");
    const auto trace = graph.trigger_trace();
    check(trace.size() == 2 && trace[0].sample == 1 && trace[1].sample == 6,
          "trigger-cross-block-samples");
    check(trace[0].sequence == 0 && trace[1].sequence == 1 && trace[0].epoch == trace[1].epoch,
          "trigger-sequence-epoch");
    check(graph.try_reset_events() == TriggerGraphResult::Accepted, "trigger-reset");
}

void voice_intent_adapter_controls() {
    // This is deliberately a test-owned adapter: it carries an authored G-4
    // identity beside the existing MPE event, without adding a runtime queue.
    struct VoiceIntent {
        pulp::host::VoiceIdentityKey identity;
        pulp::midi::MpeExpressionEvent event;
        std::uint64_t absolute_sample = 0;
    };
    VoiceIntent intent;
    intent.identity.template_namespace = "pulp.test";
    intent.identity.template_name = "p1-voice";
    intent.identity.template_revision = 1;
    intent.identity.bank_schema_version = 1;
    intent.identity.slot_ordinal = 0;
    intent.identity.path = {"voice", "note"};
    intent.identity.has_port = true;
    intent.identity.port = {.direction = pulp::host::VoicePortDirection::Input,
                            .kind = pulp::host::VoicePortKind::Event,
                            .ordinal = 0};
    const auto encoded = pulp::host::canonical_voice_identity(intent.identity);
    check(encoded.valid && !encoded.encoding.empty() && !encoded.sha256.empty(),
          "voice-intent-authored-identity");
    intent.event.kind = pulp::midi::MpeExpressionEvent::Kind::NoteOn;
    intent.event.state.note_id = 1;
    intent.event.state.channel = 1;
    intent.event.state.note = 60;
    intent.event.state.velocity = 100;
    intent.absolute_sample = 6;
    struct ProbeVoice final : pulp::midi::MpeSynthVoice {
        void render(float*, int frames) override {
            for (int i = 0; i < frames; ++i)
                advance_smoothers();
        }
    };
    pulp::midi::MpeVoiceAllocator<ProbeVoice> allocator(1);
    allocator.dispatch(intent.event);
    check(allocator.active_count() == 1 && allocator.voice(0).note_id() == 1,
          "voice-intent-mpe-delivery");
    auto stale = intent.event;
    stale.kind = pulp::midi::MpeExpressionEvent::Kind::Pressure;
    stale.state.note_id = 2;
    stale.state.pressure = 0.75f;
    allocator.dispatch(stale);
    check(allocator.voice(0).pressure() == 0.0f, "voice-intent-stale-generation-refusal");
    check(encoded.sha256 == pulp::host::canonical_voice_identity(intent.identity).sha256,
          "voice-intent-identity-stable");
}

} // namespace

int main() {
    try {
        host_and_parameter_controls(4);
        host_and_parameter_controls(8);
        trigger_two_block_controls();
        voice_intent_adapter_controls();
        std::cout << "p1-event-admission PASS checks=" << checks
                  << " partitions=4,8 planted_negative=5 consumer_gate=QUALIFIED\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "p1-event-admission FAIL rule=" << error.what() << '\n';
        return 1;
    }
}
