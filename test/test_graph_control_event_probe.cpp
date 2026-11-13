#include <catch2/catch_test_macros.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/midi/message.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

class ProbeSlot final : public pulp::host::PluginSlot {
  public:
    static constexpr std::uint32_t kParam = 7;
    std::array<float, 8> delivered{};
    std::size_t delivered_count = 0;

    explicit ProbeSlot(pulp::host::ParamRate rate, int latency = 0)
        : rate_(rate), latency_(latency) {
        info_.name = "G7 probe slot";
        info_.format = pulp::host::PluginFormat::CLAP;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }

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
    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>&, const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::host::ParameterEventQueue& events,
                 int frames) override {
        delivered_count = 0;
        for (const auto& event : events) {
            if (event.param_id == kParam && delivered_count < delivered.size())
                delivered[delivered_count++] = event.value;
        }
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::fill_n(output.channel_ptr(channel), frames, 0.0f);
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        pulp::host::HostParamInfo parameter;
        parameter.id = kParam;
        parameter.name = "probe";
        parameter.min_value = 0.0f;
        parameter.max_value = 1.0f;
        parameter.flags.automatable = true;
        parameter.flags.modulatable = true;
        parameter.rate = rate_;
        return {parameter};
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
        return true;
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return latency_;
    }
    int tail_samples() const override {
        return 0;
    }

  private:
    pulp::host::PluginInfo info_;
    pulp::host::ParamRate rate_;
    int latency_ = 0;
};

struct AudioGraph {
    pulp::host::SignalGraph graph;
    pulp::host::NodeId input = 0;
    pulp::host::NodeId plugin = 0;
    pulp::host::NodeId output = 0;

    explicit AudioGraph(pulp::host::ParamRate rate) {
        input = graph.add_input_node(1, "source");
        plugin = graph.add_plugin_node(std::make_unique<ProbeSlot>(rate), 1, 1, "destination");
        output = graph.add_output_node(1, "output");
        REQUIRE(graph.connect(input, 0, plugin, 0));
        REQUIRE(graph.connect(plugin, 0, output, 0));
        if (rate == pulp::host::ParamRate::ControlRate)
            REQUIRE(graph.connect_automation(input, 0, plugin, ProbeSlot::kParam, 0.0f, 1.0f));
        graph.set_canonical_executor_routing_enabled(true);
    }

    pulp::host::PortHandle source_port() const {
        return graph.node(input)->out("audio");
    }
    pulp::host::PortHandle destination_port() const {
        return graph.node(plugin)->in("audio");
    }
};

void process_audio(pulp::host::SignalGraph& graph, bool reset = false) {
    std::array<float, 8> input{};
    input.fill(0.5f);
    std::array<float, 8> output{};
    const float* input_channels[] = {input.data()};
    float* output_channels[] = {output.data()};
    pulp::audio::BufferView<const float> input_view(input_channels, 1, input.size());
    pulp::audio::BufferView<float> output_view(output_channels, 1, output.size());
    if (reset) {
        pulp::format::ProcessContext context;
        context.reset_requested = true;
        graph.process(output_view, input_view, static_cast<int>(input.size()), context);
    } else {
        graph.process(output_view, input_view, static_cast<int>(input.size()));
    }
}

} // namespace

TEST_CASE("G7 control probes cover lifecycle refusals, dense attachment, and reset clocks",
          "[host][graph][g7][probe]") {
    AudioGraph unprepared(pulp::host::ParamRate::ControlRate);
    const auto source = unprepared.source_port();
    REQUIRE(
        unprepared.graph.attach_control_edge_probe(source, unprepared.plugin, ProbeSlot::kParam) ==
        pulp::host::SignalGraph::EdgeProbeStatus::Unprepared);

    REQUIRE(unprepared.graph.prepare(48000.0, 8));
    const auto invalid_source =
        pulp::host::PortHandle{unprepared.input, 99, true, "missing", pulp::host::PortKind::Audio};
    REQUIRE(unprepared.graph.attach_control_edge_probe(invalid_source, unprepared.plugin,
                                                       ProbeSlot::kParam) ==
            pulp::host::SignalGraph::EdgeProbeStatus::UnknownPort);
    REQUIRE(unprepared.graph.attach_control_edge_probe(source, unprepared.plugin,
                                                       ProbeSlot::kParam + 1) ==
            pulp::host::SignalGraph::EdgeProbeStatus::MissingEdge);
    auto wrong_kind = source;
    wrong_kind.kind = pulp::host::PortKind::Midi;
    REQUIRE(unprepared.graph.attach_control_edge_probe(wrong_kind, unprepared.plugin,
                                                       ProbeSlot::kParam) ==
            pulp::host::SignalGraph::EdgeProbeStatus::UnsupportedKind);
    unprepared.graph.set_parallel_routing_enabled(true);
    REQUIRE(
        unprepared.graph.attach_control_edge_probe(source, unprepared.plugin, ProbeSlot::kParam) ==
        pulp::host::SignalGraph::EdgeProbeStatus::UnsupportedExecution);
    unprepared.graph.set_parallel_routing_enabled(false);
    REQUIRE(
        unprepared.graph.attach_control_edge_probe(source, unprepared.plugin, ProbeSlot::kParam) ==
        pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    const auto identity = unprepared.graph.control_edge_probe_identity();
    REQUIRE(identity.authoring.valid());
    REQUIRE(identity.connection != 0);
    REQUIRE(identity.source == unprepared.input);
    REQUIRE(identity.destination == unprepared.plugin);
    REQUIRE(identity.parameter_id == ProbeSlot::kParam);

    process_audio(unprepared.graph);
    pulp::graph::GraphRuntimeProbeSample sample;
    REQUIRE(unprepared.graph.read_control_edge_probe(sample));
    REQUIRE(sample.timestamp_samples == 0);
    process_audio(unprepared.graph, true);
    REQUIRE(unprepared.graph.control_edge_probe_status() ==
            pulp::host::SignalGraph::EdgeProbeStatus::Detached);
    REQUIRE_FALSE(unprepared.graph.read_control_edge_probe(sample));
    REQUIRE(
        unprepared.graph.attach_control_edge_probe(source, unprepared.plugin, ProbeSlot::kParam) ==
        pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    process_audio(unprepared.graph);
    REQUIRE(unprepared.graph.read_control_edge_probe(sample));
    REQUIRE(sample.timestamp_samples == 0);

    AudioGraph dense(pulp::host::ParamRate::AudioRate);
    REQUIRE(dense.graph.connect_audio_rate_modulation(dense.input, 0, dense.plugin,
                                                      ProbeSlot::kParam, 0.0f, 1.0f));
    REQUIRE(dense.graph.prepare(48000.0, 8));
    REQUIRE(dense.graph.attach_control_edge_probe(dense.source_port(), dense.plugin,
                                                  ProbeSlot::kParam) ==
            pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    process_audio(dense.graph);
    REQUIRE(dense.graph.read_control_edge_probe(sample));
    REQUIRE(sample.value == 0.5f);
    REQUIRE(sample.value_end == 0.5f);
    REQUIRE(sample.peak == 0.5f);
    REQUIRE(sample.rms == 0.5f);
    REQUIRE(sample.payload_count == 8);
}

TEST_CASE("G7 event probes cover lifecycle, identity, reset detach, and publication",
          "[host][graph][g7][probe]") {
    pulp::host::SignalGraph graph;
    const auto audio_input = graph.add_input_node(1, "audio");
    const auto audio_output = graph.add_output_node(1, "audio-out");
    const auto midi_input = graph.add_midi_input_node("source");
    const auto midi_output = graph.add_midi_output_node("destination");
    const auto other = graph.add_midi_output_node("other");
    REQUIRE(graph.connect(audio_input, 0, audio_output, 0));
    REQUIRE(graph.connect_midi(midi_input, midi_output));
    graph.set_canonical_executor_routing_enabled(true);
    const auto source = graph.node(midi_input)->out("audio");
    const auto destination = graph.node(midi_output)->in("audio");
    REQUIRE(graph.attach_event_edge_probe(source, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::Unprepared);
    REQUIRE(graph.prepare(48000.0, 8));
    auto invalid = source;
    invalid.index = 99;
    REQUIRE(graph.attach_event_edge_probe(invalid, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::UnknownPort);
    auto wrong_kind = source;
    wrong_kind.kind = pulp::host::PortKind::Audio;
    REQUIRE(graph.attach_event_edge_probe(wrong_kind, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::UnsupportedKind);
    REQUIRE(graph.attach_event_edge_probe(source, graph.node(other)->in("audio")) ==
            pulp::host::SignalGraph::EdgeProbeStatus::MissingEdge);
    graph.set_parallel_routing_enabled(true);
    REQUIRE(graph.attach_event_edge_probe(source, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::UnsupportedExecution);
    graph.set_parallel_routing_enabled(false);
    REQUIRE(graph.attach_event_edge_probe(source, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    const auto identity = graph.event_edge_probe_identity();
    REQUIRE(identity.authoring.valid());
    REQUIRE(identity.connection != 0);
    REQUIRE(identity.source == midi_input);
    REQUIRE(identity.destination == midi_output);

    pulp::midi::MidiBuffer events;
    events.reserve(2);
    REQUIRE(events.add(pulp::midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE(graph.inject_midi(midi_input, events));
    process_audio(graph);
    pulp::graph::GraphRuntimeProbeSample sample;
    REQUIRE(graph.read_event_edge_probe(sample));
    REQUIRE(sample.timestamp_samples == 0);
    REQUIRE(sample.payload_count == 1);
    process_audio(graph, true);
    REQUIRE(graph.event_edge_probe_status() == pulp::host::SignalGraph::EdgeProbeStatus::Detached);
    REQUIRE_FALSE(graph.read_event_edge_probe(sample));
    REQUIRE(graph.attach_event_edge_probe(source, destination) ==
            pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    REQUIRE(graph.inject_midi(midi_input, events));
    process_audio(graph);
    REQUIRE(graph.read_event_edge_probe(sample));
    REQUIRE(sample.timestamp_samples == 0);
}

TEST_CASE("G7 dense control probe summarizes the selected slewed contribution before mixing",
          "[host][graph][g7][probe][dense-probe]") {
    using Graph = pulp::host::SignalGraph;
    using Status = Graph::EdgeProbeStatus;
    for (const auto& partitions :
         {std::vector<int>{8, 8, 8, 8}, std::vector<int>{1, 3, 8, 5, 7, 8}}) {
        Graph graph;
        const auto input = graph.add_input_node(2);
        auto slot = std::make_unique<ProbeSlot>(pulp::host::ParamRate::AudioRate);
        auto* receiver = slot.get();
        const auto destination = graph.add_plugin_node(std::move(slot), 1, 1);
        const auto output = graph.add_output_node(1);
        REQUIRE(graph.connect(input, 0, destination, 0));
        REQUIRE(graph.connect(destination, 0, output, 0));
        REQUIRE(graph.connect_audio_rate_modulation(input, 0, destination, ProbeSlot::kParam, -1.0f,
                                                    2.0f, 0.5f));
        REQUIRE(graph.connect_audio_rate_modulation(input, 1, destination, ProbeSlot::kParam, 0.6f,
                                                    0.6f, 0.0f, pulp::host::AutomationMix::Add));
        REQUIRE(graph.prepare(48000, 8));
        const auto source = graph.node(input)->out("audio");
        REQUIRE(graph.attach_control_edge_probe(source, destination, ProbeSlot::kParam) ==
                Status::Attached);
        const auto identity = graph.control_edge_probe_identity();
        std::uint64_t timestamp = 0;
        std::uint64_t sequence = 0;
        float previous = 0.0f;
        bool primed = false;
        for (const int frames : partitions) {
            std::array<float, 8> in{}, other{}, out{}, expected{};
            float peak = 0.0f;
            double squares = 0.0;
            for (int f = 0; f < frames; ++f) {
                const auto position = timestamp + f;
                in[f] = (position % 5 == 0)   ? 1.25f
                        : (position % 5 == 1) ? -0.25f
                                              : static_cast<float>(position % 4) / 4.0f;
                const float target = -1.0f + 3.0f * std::clamp(in[f], 0.0f, 1.0f);
                // Independent scalar slew: 3 units over 24 samples at 48 kHz.
                if (!primed) {
                    previous = target;
                    primed = true;
                } else if (target > previous + 0.125f)
                    previous += 0.125f;
                else if (target < previous - 0.125f)
                    previous -= 0.125f;
                else
                    previous = target;
                expected[f] = previous;
                peak = std::max(peak, std::abs(previous));
                squares += static_cast<double>(previous) * previous;
            }
            const float* ips[] = {in.data(), other.data()};
            float* ops[] = {out.data()};
            pulp::audio::BufferView<const float> iv(ips, 2, frames);
            pulp::audio::BufferView<float> ov(ops, 1, frames);
            graph.process(ov, iv, frames);
            pulp::graph::GraphRuntimeProbeSample sample;
            REQUIRE(graph.read_control_edge_probe(sample));
            REQUIRE(sample.kind == pulp::graph::GraphRuntimeProbeKind::Control);
            REQUIRE(sample.sequence == sequence++);
            REQUIRE(sample.timestamp_samples == timestamp);
            REQUIRE(sample.frames == frames);
            REQUIRE(sample.payload_count == frames);
            REQUIRE(sample.payload_id == ProbeSlot::kParam);
            REQUIRE(sample.value == expected[0]);
            REQUIRE(sample.value_end == expected[frames - 1]);
            REQUIRE(sample.peak == peak);
            REQUIRE(sample.rms == static_cast<float>(std::sqrt(squares / frames)));
            REQUIRE(receiver->delivered_count == frames);
            for (int f = 0; f < frames; ++f)
                REQUIRE(receiver->delivered[f] == std::clamp(expected[f] + 0.6f, 0.0f, 1.0f));
            REQUIRE(graph.control_edge_probe_identity().connection == identity.connection);
            REQUIRE_FALSE(graph.read_control_edge_probe(sample));
            timestamp += frames;
        }
        for (int block = 0; block < 258; ++block)
            process_audio(graph);
        REQUIRE(graph.control_edge_probe_dropped() == 2);
        pulp::graph::GraphRuntimeProbeSample sample;
        for (int record = 0; record < 256; ++record) {
            REQUIRE(graph.read_control_edge_probe(sample));
            REQUIRE(sample.sequence == sequence++);
            REQUIRE(sample.timestamp_samples == timestamp + record * 8);
        }
        REQUIRE_FALSE(graph.read_control_edge_probe(sample));
        graph.detach_control_edge_probe();
        REQUIRE(graph.control_edge_probe_status() == Status::Detached);
        REQUIRE(graph.attach_control_edge_probe(source, destination, ProbeSlot::kParam) ==
                Status::Attached);
        process_audio(graph);
        REQUIRE(graph.read_control_edge_probe(sample));
        REQUIRE(sample.sequence == 0);
        REQUIRE(sample.timestamp_samples == 0);
        REQUIRE(graph.control_edge_probe_dropped() == 0);
        REQUIRE(graph.prepare(48000, 8));
        REQUIRE(graph.control_edge_probe_status() == Status::Detached);
        REQUIRE(graph.attach_control_edge_probe(source, destination, ProbeSlot::kParam) ==
                Status::Attached);
        REQUIRE(graph.disconnect(input, 0, destination, 0));
        REQUIRE(graph.control_edge_probe_status() == Status::Unprepared);
        REQUIRE_FALSE(graph.read_control_edge_probe(sample));
    }
}

TEST_CASE("G7 dense control probe observes PDC history rather than current source",
          "[host][graph][g7][probe][dense-probe]") {
    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto delay = graph.add_plugin_node(
        std::make_unique<ProbeSlot>(pulp::host::ParamRate::AudioRate, 3), 1, 1);
    auto slot = std::make_unique<ProbeSlot>(pulp::host::ParamRate::AudioRate);
    auto* receiver = slot.get();
    const auto destination = graph.add_plugin_node(std::move(slot), 1, 1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, delay, 0));
    REQUIRE(graph.connect(delay, 0, destination, 0));
    REQUIRE(graph.connect(destination, 0, output, 0));
    REQUIRE(
        graph.connect_audio_rate_modulation(input, 0, destination, ProbeSlot::kParam, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48000, 8));
    REQUIRE(graph.attach_control_edge_probe(graph.node(input)->out("audio"), destination,
                                            ProbeSlot::kParam) ==
            pulp::host::SignalGraph::EdgeProbeStatus::Attached);
    std::vector<float> history(3, 0.0f);
    std::uint64_t timestamp = 0;
    for (const int frames : {8, 1, 3, 7, 5}) {
        std::array<float, 8> input_samples{}, output_samples{}, expected{};
        float peak = 0.0f;
        double squares = 0.0;
        for (int f = 0; f < frames; ++f) {
            input_samples[f] = static_cast<float>((timestamp + f) % 7 + 1) * 0.125f;
            history.push_back(input_samples[f]);
            expected[f] = history[timestamp + f];
            peak = std::max(peak, expected[f]);
            squares += static_cast<double>(expected[f]) * expected[f];
        }
        const float* ips[] = {input_samples.data()};
        float* ops[] = {output_samples.data()};
        pulp::audio::BufferView<const float> iv(ips, 1, frames);
        pulp::audio::BufferView<float> ov(ops, 1, frames);
        graph.process(ov, iv, frames);
        pulp::graph::GraphRuntimeProbeSample sample;
        REQUIRE(graph.read_control_edge_probe(sample));
        REQUIRE(sample.timestamp_samples == timestamp);
        REQUIRE(sample.value == expected[0]);
        REQUIRE(sample.value_end == expected[frames - 1]);
        REQUIRE(sample.peak == peak);
        REQUIRE(sample.rms == static_cast<float>(std::sqrt(squares / frames)));
        REQUIRE(receiver->delivered_count == frames);
        for (int f = 0; f < frames; ++f)
            REQUIRE(receiver->delivered[f] == expected[f]);
        timestamp += frames;
    }
}
