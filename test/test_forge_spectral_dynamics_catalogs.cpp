#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_multiband_catalog.hpp>
#include <pulp/host/forge_sidechain_catalog.hpp>
#include <pulp/host/forge_synthesis_catalog.hpp>
#include <pulp/host/forge_wavetable_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kFrames = 256;

std::vector<float> render_node(const pulp::host::CustomNodeType& type,
                               const std::vector<std::vector<float>>& inputs, int blocks = 1) {
    using namespace pulp;
    host::SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(type));

    host::NodeId input_node = 0;
    if (!inputs.empty())
        input_node = graph.add_input_node(static_cast<std::uint16_t>(inputs.size()), "Input");
    const auto node = graph.add_custom_node(type.type_id, type.version, "Node");
    const auto output_node = graph.add_output_node(1, "Output");
    for (std::size_t port = 0; port < inputs.size(); ++port) {
        REQUIRE(graph.connect(input_node, static_cast<host::PortIndex>(port), node,
                              static_cast<host::PortIndex>(port)));
    }
    REQUIRE(graph.connect(node, 0, output_node, 0));
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(graph.prepare(kSampleRate, kFrames));

    auto lowered = host::bake(graph);
    REQUIRE(lowered.accepted);
    REQUIRE(lowered.processor);

    format::PrepareContext prepare;
    prepare.sample_rate = kSampleRate;
    prepare.max_buffer_size = kFrames;
    prepare.input_channels = static_cast<int>(inputs.size());
    prepare.output_channels = 1;
    lowered.processor->prepare(prepare);

    std::vector<const float*> input_ptrs;
    input_ptrs.reserve(inputs.size());
    for (const auto& channel : inputs)
        input_ptrs.push_back(channel.data());
    std::vector<float> output(static_cast<std::size_t>(kFrames), 0.0f);
    float* output_ptr = output.data();
    audio::BufferView<const float> input_view(
        input_ptrs.data(), static_cast<std::uint32_t>(input_ptrs.size()), kFrames);
    audio::BufferView<float> output_view(&output_ptr, 1, kFrames);
    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext context;
    context.sample_rate = kSampleRate;
    context.num_samples = kFrames;
    for (int block = 0; block < blocks; ++block)
        lowered.processor->process(output_view, input_view, midi_in, midi_out, context);
    return output;
}

float energy(const std::vector<float>& samples) {
    float total = 0.0f;
    for (float sample : samples)
        total += sample * sample;
    return total;
}

std::vector<float> sine(float amplitude, float frequency_hz = 1000.0f) {
    std::vector<float> result(static_cast<std::size_t>(kFrames));
    for (int i = 0; i < kFrames; ++i) {
        result[static_cast<std::size_t>(i)] =
            amplitude * std::sin(static_cast<float>(2.0 * M_PI * frequency_hz * i / kSampleRate));
    }
    return result;
}

class FixedParamView final : public pulp::host::BakedParamView {
  public:
    explicit FixedParamView(const pulp::host::CustomNodeType& type) {
        for (const auto& param : type.baked_params)
            values_.emplace_back(param.id, param.default_value);
    }

    void set(pulp::state::ParamID id, float value) {
        const auto found = std::find_if(values_.begin(), values_.end(),
                                        [id](const auto& item) { return item.first == id; });
        REQUIRE(found != values_.end());
        found->second = value;
    }

    float value_at(pulp::state::ParamID id, std::int32_t) const override {
        return value(id);
    }

    float value(pulp::state::ParamID id) const override {
        const auto found = std::find_if(values_.begin(), values_.end(),
                                        [id](const auto& item) { return item.first == id; });
        REQUIRE(found != values_.end());
        return found->second;
    }

  private:
    std::vector<std::pair<pulp::state::ParamID, float>> values_;
};

} // namespace

TEST_CASE("Forge spectral and compound-dynamics packs declare their real port shapes",
          "[host][forge][catalog]") {
    const auto wavetable = pulp::host::wavetable::make_wavetable_oscillator_node();
    CHECK(wavetable.type_id == pulp::host::wavetable::kTypeId);
    CHECK(wavetable.num_input_ports == 0);
    CHECK(wavetable.num_output_ports == 1);
    CHECK(wavetable.lowerable);

    const auto multiband = pulp::host::multiband::make_multiband_compressor_node();
    CHECK(multiband.type_id == pulp::host::multiband::kTypeId);
    CHECK(multiband.num_input_ports == 1);
    CHECK(multiband.num_output_ports == 1);
    CHECK(multiband.lowerable);

    const auto sidechain = pulp::host::sidechain::make_sidechain_compressor_node();
    CHECK(sidechain.type_id == pulp::host::sidechain::kTypeId);
    CHECK(sidechain.num_input_ports == 2);
    CHECK(sidechain.num_output_ports == 1);
    CHECK(sidechain.lowerable);
}

TEST_CASE("Forge vocoder exposes a sample-rate-independent registry gain ceiling",
          "[host][forge][catalog][vocoder]") {
    const float all_rates =
        pulp::host::synthesis::vocoder::vocoder_all_sample_rates_worst_case_gain();
    for (const double sample_rate :
         {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 768000.0, 1000000.0}) {
        INFO("sample_rate=" << sample_rate);
        CHECK(pulp::host::synthesis::vocoder::vocoder_worst_case_gain(sample_rate) < all_rates);
    }
}

TEST_CASE("Forge wavetable catalog source bakes and emits audio",
          "[host][forge][catalog][wavetable]") {
    const auto output = render_node(pulp::host::wavetable::make_wavetable_oscillator_node(), {});
    CHECK(energy(output) > 0.1f);
    for (float sample : output)
        CHECK(std::isfinite(sample));
}

TEST_CASE("Forge multiband catalog node bakes and processes both bands",
          "[host][forge][catalog][multiband]") {
    std::vector<float> input = sine(0.25f, 120.0f);
    const auto high = sine(0.25f, 4000.0f);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] += high[i];
    const auto output =
        render_node(pulp::host::multiband::make_multiband_compressor_node(), {input}, 24);
    CHECK(energy(output) > 0.01f);
    for (float sample : output)
        CHECK(std::isfinite(sample));
}

TEST_CASE("Forge sidechain catalog node uses its key input", "[host][forge][catalog][sidechain]") {
    const auto main = sine(0.5f);
    const std::vector<float> quiet_key(static_cast<std::size_t>(kFrames), 0.0f);
    const std::vector<float> loud_key(static_cast<std::size_t>(kFrames), 1.0f);
    const auto unkeyed =
        render_node(pulp::host::sidechain::make_sidechain_compressor_node(), {main, quiet_key}, 32);
    const auto keyed =
        render_node(pulp::host::sidechain::make_sidechain_compressor_node(), {main, loud_key}, 32);
    CHECK(energy(keyed) < energy(unkeyed) * 0.25f);
}

TEST_CASE("Forge sidechain HPF preserves detector state across blocks",
          "[host][forge][catalog][sidechain]") {
    const auto type = pulp::host::sidechain::make_sidechain_compressor_node();
    void* instance = type.create();
    REQUIRE(instance != nullptr);
    type.prepare(instance, kSampleRate, kFrames);

    FixedParamView params(type);
    params.set(pulp::host::sidechain::kThresholdDb, -40.0f);
    params.set(pulp::host::sidechain::kRatio, 20.0f);
    params.set(pulp::host::sidechain::kAttackMs, 0.1f);
    params.set(pulp::host::sidechain::kReleaseMs, 10.0f);
    params.set(pulp::host::sidechain::kKeyHpfHz, 1000.0f);

    const auto main = sine(0.5f);
    const std::vector<float> dc_key(static_cast<std::size_t>(kFrames), 1.0f);
    const float* input_ptrs[] = {main.data(), dc_key.data()};
    std::vector<float> output(static_cast<std::size_t>(kFrames));
    float* output_ptr = output.data();
    pulp::audio::BufferView<const float> input_view(input_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> output_view(&output_ptr, 1, kFrames);

    for (int block = 0; block < 100; ++block)
        type.process_instance_baked_param(instance, output_view, input_view, kFrames, params);
    type.destroy(instance);

    // A DC key disappears through a settled HPF. Reconfiguring the unchanged
    // cutoff every block would reset the filter, manufacture a new transient,
    // and keep the compressor falsely ducking forever.
    CHECK(energy(output) > energy(main) * 0.9f);
}
