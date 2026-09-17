#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/state/store.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t kGainParam = 7;
constexpr std::uint32_t kFrames = 256;
constexpr double kSampleRate = 48000.0;

class AudioRateGain final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.name = "AudioRateGain";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "dev.pulp.example.audio-rate-gain";
        descriptor.version = "1.0.0";
        descriptor.category = pulp::format::PluginCategory::Effect;
        descriptor.input_buses = {{"Main In", 1, false}};
        descriptor.output_buses = {{"Main Out", 1, false}};
        descriptor.node_capabilities.consumes_audio_rate_modulations = true;
        return descriptor;
    }

    void define_parameters(pulp::state::StateStore& store) override {
        pulp::state::ParamInfo gain;
        gain.id = kGainParam;
        gain.name = "Gain";
        gain.range = {0.0f, 1.0f, 0.5f};
        gain.rate = pulp::state::ParamRate::AudioRate;
        store.add_parameter(gain);
    }

    void prepare(const pulp::format::PrepareContext&) override {}

    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    bool process_block(pulp::format::ProcessBlock& block) override {
        if (block.buses == nullptr || block.events == nullptr ||
            block.events->audio_rate_modulations.size() != 1) {
            return false;
        }
        const auto& gain = block.events->audio_rate_modulations.front();
        if (gain.param_id != kGainParam || gain.values.size() != block.frame_count)
            return false;

        const auto* input =
            block.buses->first(pulp::format::BusDirection::Input, pulp::format::BusRole::Main);
        auto* output =
            block.buses->first(pulp::format::BusDirection::Output, pulp::format::BusRole::Main);
        if (input == nullptr || output == nullptr)
            return false;

        const float* in = input->input.channel_ptr(0);
        float* out = output->output.channel_ptr(0);
        for (std::uint32_t frame = 0; frame < block.frame_count; ++frame)
            out[frame] = in[frame] * gain.values[frame];
        return true;
    }
};

bool render_and_check() {
    auto instance = pulp::format::ProcessorNodeInstance::create(std::make_unique<AudioRateGain>());
    if (!instance)
        return false;

    pulp::format::PrepareContext prepare;
    prepare.sample_rate = kSampleRate;
    prepare.max_buffer_size = static_cast<int>(kFrames);
    prepare.input_channels = 1;
    prepare.output_channels = 1;
    if (!instance->prepare(prepare))
        return false;

    const std::array nodes = {
        pulp::graph::GraphRuntimeNodeSpec{1, pulp::graph::GraphRuntimeNodeKind::AudioInput, 0, 2},
        pulp::graph::GraphRuntimeNodeSpec{2, pulp::graph::GraphRuntimeNodeKind::Processor, 1, 1},
        pulp::graph::GraphRuntimeNodeSpec{3, pulp::graph::GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };

    auto gain = pulp::graph::GraphRuntimeConnectionSpec{1, 1, 2, 0};
    gain.kind = pulp::graph::GraphRuntimeConnectionKind::Automation;
    gain.automation.param_id = kGainParam;
    gain.automation.audio_rate = true;
    gain.automation.range_lo = 0.0f;
    gain.automation.range_hi = 1.0f;
    gain.automation.bounds_lo = 0.0f;
    gain.automation.bounds_hi = 1.0f;

    const std::array connections = {
        pulp::graph::GraphRuntimeConnectionSpec{1, 0, 2, 0},
        gain,
        pulp::graph::GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    const std::array bindings = {
        pulp::format::GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        instance->binding(2),
        pulp::format::GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };

    auto plan = pulp::graph::build_graph_runtime_plan(nodes, connections);
    if (!plan.ok())
        return false;
    pulp::format::GraphRuntimeSnapshot snapshot;
    if (!snapshot.reset(std::move(plan.plan), bindings))
        return false;

    pulp::format::GraphRuntimeBufferPool pool;
    if (!pool.reset(snapshot.buffer_slot_count(), kFrames))
        return false;
    pulp::format::GraphRuntimeAutomationScratch automation;
    if (!automation.reset(snapshot.plan(), snapshot.bindings(), kFrames))
        return false;

    std::vector<float> audio(kFrames);
    std::vector<float> modulation(kFrames);
    std::vector<float> output(kFrames, 0.0f);
    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        audio[frame] = 0.25f + 0.001f * static_cast<float>(frame);
        modulation[frame] = static_cast<float>(frame) / static_cast<float>(kFrames - 1);
    }

    std::array<const float*, 2> input_channels{audio.data(), modulation.data()};
    std::array<float*, 1> output_channels{output.data()};
    pulp::format::BusBufferSet buses;
    if (!buses.add_input("main", pulp::audio::BufferView<const float>(
                                     input_channels.data(), input_channels.size(), kFrames)) ||
        !buses.add_output("main", pulp::audio::BufferView<float>(
                                      output_channels.data(), output_channels.size(), kFrames))) {
        return false;
    }

    pulp::format::ProcessBlock block;
    block.sample_rate = kSampleRate;
    block.frame_count = kFrames;
    block.buses = &buses;

    pulp::format::GraphRuntimeExecutor executor;
    if (!executor.process_routed(block, snapshot, pool, nullptr, &automation).ok())
        return false;

    for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
        const float expected = audio[frame] * modulation[frame];
        if (std::abs(output[frame] - expected) > 1.0e-7f)
            return false;
    }
    return true;
}

} // namespace

int main() {
    if (!render_and_check()) {
        std::cerr << "dense AudioRate ProcessorNode example failed\n";
        return 1;
    }
    std::cout << "dense AudioRate ProcessorNode example passed\n";
    return 0;
}
