// S4 proof: authored ProcessorNode instances must be reachable through the
// same Timeline automation and graph modulation routes as hosted plugins.

#include "support/timeline_graph_binding_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using pulp::format::BusDirection;
using pulp::format::BusRole;
using pulp::format::PluginCategory;
using pulp::format::PluginDescriptor;
using pulp::format::PrepareContext;
using pulp::format::ProcessBlock;
using pulp::format::ProcessContext;
using pulp::format::Processor;
using pulp::host::NodeId;
using pulp::host::SignalGraph;
using pulp::host::TimelineDeviceGraphRoute;
using pulp::host::TimelineGraphPlaybackBinding;
using pulp::host::TimelineTrackGraphRoute;
using pulp::state::ParamInfo;
using pulp::state::ParamRate;
using pulp::state::StateStore;

constexpr pulp::state::ParamID kSequencerParam = 2901;

class SequencerProbeProcessor final : public Processor {
  public:
    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "S4 Sequencer Probe";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.test.s4-sequencer-probe";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", 1, false}};
        d.output_buses = {{"Main Out", 1, false}};
        d.node_capabilities.consumes_audio_rate_modulations = true;
        return d;
    }

    void define_parameters(StateStore& store) override {
        ParamInfo p;
        p.id = kSequencerParam;
        p.name = "Sequencer Amount";
        p.range = {0.0f, 1.0f, 0.25f};
        p.rate = ParamRate::AudioRate;
        store.add_parameter(p);
    }

    void prepare(const PrepareContext&) override {
        sparse_events.clear();
        dense_values.clear();
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const ProcessContext&) override {
        if (output.num_channels() == 0)
            return;
        const auto* source = input.num_channels() == 0 ? nullptr : input.channel_ptr(0);
        auto* destination = output.channel_ptr(0);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            destination[frame] = source == nullptr ? 0.0f : source[frame];
    }

    bool process_block(ProcessBlock& block) override {
        if (block.events != nullptr) {
            for (const auto& event : block.events->parameters())
                sparse_events.push_back(event);
            if (!block.events->audio_rate_modulations.empty()) {
                const auto& lane = block.events->audio_rate_modulations.front();
                dense_values.assign(lane.values.begin(), lane.values.end());
            }
        }
        auto* input = block.buses == nullptr
                          ? nullptr
                          : block.buses->first(BusDirection::Input, BusRole::Main);
        auto* output = block.buses == nullptr
                           ? nullptr
                           : block.buses->first(BusDirection::Output, BusRole::Main);
        if (output == nullptr)
            return false;
        const auto* source = input == nullptr ? nullptr : input->input.channel_ptr(0);
        auto* destination = output->output.channel_ptr(0);
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame)
            destination[frame] = source == nullptr ? 0.0f : source[frame];
        return true;
    }

    std::vector<pulp::state::ParameterEvent> sparse_events;
    std::vector<float> dense_values;
};

} // namespace

TEST_CASE("S4 timeline automation reaches an authored ProcessorNode") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(automation_project(*map, 0.25f, 0.75f, kSequencerParam), map,
                     take(DecodedAudioAssetPool::create({})), 1);
    const auto program = programs.store.read();
    REQUIRE(program);

    SignalGraph graph;
    const auto output = graph.add_output_node(1);
    auto processor = std::make_unique<SequencerProbeProcessor>();
    auto* observed = processor.get();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.is_processor_node(node));
    REQUIRE(graph.connect(node, 0, output, 0));
    REQUIRE(graph.prepare(48'000.0, 64));

    const std::array devices{TimelineDeviceGraphRoute{{20}, node}};
    const std::array routes{TimelineTrackGraphRoute{{10}, output, 0, 0, devices}};
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.prepare_quiesced(*program, routes, config(1), 48'000.0, 64));

    Buffer input(1, 32, 1.0f);
    Buffer rendered(1, 32);
    auto input_view = input.const_view();
    auto rendered_view = rendered.view();
    auto result = binding.process(rendered_view, input_view, snapshot(*program, 32));
    REQUIRE(result);
    REQUIRE(result.emitted_automation_events > 0);
    REQUIRE_FALSE(observed->sparse_events.empty());
    REQUIRE(std::all_of(observed->sparse_events.begin(), observed->sparse_events.end(),
                        [](const auto& event) { return event.param_id == kSequencerParam; }));
    REQUIRE(std::any_of(observed->sparse_events.begin(), observed->sparse_events.end(),
                        [](const auto& event) {
                            return event.value == 0.75f && event.ramp_duration_sample_frames > 0;
                        }));
}

TEST_CASE("S4 graph modulation reaches the same authored ProcessorNode parameter") {
    SignalGraph graph;
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    auto processor = std::make_unique<SequencerProbeProcessor>();
    auto* observed = processor.get();
    const auto node = graph.add_processor_node(std::move(processor));
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input_node, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output_node, 0));
    REQUIRE(graph.connect_audio_rate_modulation(input_node, 0, node, kSequencerParam, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48'000.0, 32));

    Buffer input(1, 32, 0.5f);
    Buffer output(1, 32);
    auto input_view = input.const_view();
    auto output_view = output.view();
    graph.process(output_view, input_view, 32);
    REQUIRE(observed->dense_values.size() == 32);
    REQUIRE(std::all_of(observed->dense_values.begin(), observed->dense_values.end(),
                        [](float value) { return value == 0.5f; }));
}
