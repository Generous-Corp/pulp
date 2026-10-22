#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/forge_lofi_catalog.hpp>
#include <pulp/host/forge_multiband_catalog.hpp>
#include <pulp/host/forge_sidechain_catalog.hpp>
#include <pulp/host/forge_synthesis_catalog.hpp>
#include <pulp/host/forge_wavetable_catalog.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/signal/compressor.hpp>
#include <pulp/signal/linkwitz_riley.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
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
            amplitude *
            std::sin(static_cast<float>(2.0 * std::numbers::pi * frequency_hz * i / kSampleRate));
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

// Runs a node's baked callback directly over `frames` samples in blocks of
// irregular length. `alias_port` < 0 gives the node its own output buffer;
// otherwise the output buffer IS that input port's buffer, as an executor that
// reuses buffers in place would hand it.
std::vector<float> run_callback(const pulp::host::CustomNodeType& type,
                                const FixedParamView& params,
                                std::vector<std::vector<float>> inputs, int alias_port = -1) {
    void* instance = type.create();
    REQUIRE(instance != nullptr);
    type.prepare(instance, kSampleRate, 1024);
    const std::size_t frames = inputs.front().size();
    std::vector<float> own_output(frames, 0.0f);
    float* output_base =
        alias_port < 0 ? own_output.data() : inputs[static_cast<std::size_t>(alias_port)].data();
    constexpr int kBlockSizes[] = {1, 300, 64, 513, 7, 1024, 255};
    std::size_t offset = 0;
    for (std::size_t b = 0; offset < frames; ++b) {
        const int n = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(kBlockSizes[b % std::size(kBlockSizes)]), frames - offset));
        std::vector<const float*> input_ptrs;
        for (auto& channel : inputs)
            input_ptrs.push_back(channel.data() + offset);
        float* output_ptr = output_base + offset;
        pulp::audio::BufferView<const float> input_view(
            input_ptrs.data(), static_cast<std::uint32_t>(input_ptrs.size()),
            static_cast<std::uint32_t>(n));
        pulp::audio::BufferView<float> output_view(&output_ptr, 1, static_cast<std::uint32_t>(n));
        type.process_instance_baked_param(instance, output_view, input_view, n, params);
        offset += static_cast<std::size_t>(n);
    }
    type.destroy(instance);
    return alias_port < 0 ? own_output : inputs[static_cast<std::size_t>(alias_port)];
}

// A gated two-tone burst: loud enough to drive every compressor here well into
// gain reduction, with silent gaps so attack and release both run.
std::vector<float> burst_signal(std::size_t frames) {
    std::vector<float> result(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        const bool on = (i / 2000) % 2 == 0;
        result[i] = on ? static_cast<float>(0.6 * std::sin(2.0 * std::numbers::pi * 150.0 * t) +
                                            0.3 * std::sin(2.0 * std::numbers::pi * 5000.0 * t))
                       : 0.0f;
    }
    return result;
}

// The node and the reference must agree bit for bit: the node only routes
// samples, so any difference is a routing bug, not rounding.
void require_matches(const std::vector<float>& actual, const std::vector<float>& expected) {
    REQUIRE(actual.size() == expected.size());
    std::size_t mismatches = 0;
    std::size_t first = actual.size();
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != expected[i]) {
            ++mismatches;
            first = std::min(first, i);
        }
    }
    INFO("mismatched samples = " << mismatches << ", first at " << first);
    CHECK(mismatches == 0);
}

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

TEST_CASE("Forge compressor nodes render what their compressor renders, at any block size",
          "[host][forge][catalog][dynamics]") {
    constexpr std::size_t kSamples = 9000;
    const auto program = burst_signal(kSamples);

    SECTION("lo-fi compressor") {
        namespace lofi = pulp::host::forge_lofi;
        const auto type = lofi::make_compressor_node();
        FixedParamView params(type);
        params.set(lofi::kCompThresholdDb, -30.0f);
        params.set(lofi::kCompRatio, 8.0f);
        params.set(lofi::kCompAttackMs, 1.0f);
        params.set(lofi::kCompReleaseMs, 40.0f);

        pulp::signal::Compressor reference;
        reference.set_sample_rate(static_cast<float>(kSampleRate));
        pulp::signal::Compressor::Params cp;
        cp.threshold_db = -30.0f;
        cp.ratio = 8.0f;
        cp.attack_ms = 1.0f;
        cp.release_ms = 40.0f;
        cp.knee_db = 6.0f;
        cp.makeup_db = 0.0f;
        reference.set_params(cp);
        std::vector<float> expected(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i)
            expected[i] = reference.process(program[i]);
        REQUIRE(energy(expected) < energy(program) * 0.5f);

        require_matches(run_callback(type, params, {program}), expected);
        require_matches(run_callback(type, params, {program}, 0), expected);
    }

    SECTION("sidechain compressor") {
        namespace sc = pulp::host::sidechain;
        const auto type = sc::make_sidechain_compressor_node();
        FixedParamView params(type);
        params.set(sc::kThresholdDb, -30.0f);
        params.set(sc::kRatio, 8.0f);
        params.set(sc::kAttackMs, 1.0f);
        params.set(sc::kReleaseMs, 40.0f);
        params.set(sc::kKneeDb, 3.0f);
        params.set(sc::kMakeupDb, 2.0f);
        params.set(sc::kKeyHpfHz, 0.0f);
        std::vector<float> main_signal(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i)
            main_signal[i] =
                0.5f * std::sin(static_cast<float>(2.0 * std::numbers::pi * 440.0 *
                                                   static_cast<double>(i) / kSampleRate));

        pulp::signal::Compressor reference;
        reference.set_sample_rate(static_cast<float>(kSampleRate));
        pulp::signal::Compressor::Params cp;
        cp.threshold_db = -30.0f;
        cp.ratio = 8.0f;
        cp.attack_ms = 1.0f;
        cp.release_ms = 40.0f;
        cp.knee_db = 3.0f;
        cp.makeup_db = 2.0f;
        reference.set_params(cp);
        std::vector<float> expected(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i)
            expected[i] = reference.process_with_sidechain(main_signal[i], program[i]);

        require_matches(run_callback(type, params, {main_signal, program}), expected);
        require_matches(run_callback(type, params, {main_signal, program}, sc::kSignalPort),
                        expected);
        // The key's own buffer receives the output: the key must still be read
        // as the key, not overwritten by the main signal before detection.
        require_matches(run_callback(type, params, {main_signal, program}, sc::kKeyPort), expected);
    }

    SECTION("multiband compressor") {
        namespace mb = pulp::host::multiband;
        const auto type = mb::make_multiband_compressor_node();
        FixedParamView params(type);
        params.set(mb::kCrossoverHz, 1000.0f);
        params.set(mb::kLowThresholdDb, -30.0f);
        params.set(mb::kLowRatio, 6.0f);
        params.set(mb::kHighThresholdDb, -24.0f);
        params.set(mb::kHighRatio, 3.0f);
        params.set(mb::kAttackMs, 2.0f);
        params.set(mb::kReleaseMs, 60.0f);
        params.set(mb::kLowMakeupDb, 1.5f);
        params.set(mb::kHighMakeupDb, -1.0f);

        pulp::signal::LinkwitzRiley crossover;
        crossover.set_frequency(1000.0f, static_cast<float>(kSampleRate));
        pulp::signal::Compressor low;
        pulp::signal::Compressor high;
        low.set_sample_rate(static_cast<float>(kSampleRate));
        high.set_sample_rate(static_cast<float>(kSampleRate));
        low.set_params(mb::detail::compressor_params(-30.0f, 6.0f, 2.0f, 60.0f, 1.5f));
        high.set_params(mb::detail::compressor_params(-24.0f, 3.0f, 2.0f, 60.0f, -1.0f));
        std::vector<float> expected(kSamples);
        for (std::size_t i = 0; i < kSamples; ++i) {
            const auto split = crossover.process(program[i]);
            expected[i] = low.process(split.low) + high.process(split.high);
        }

        require_matches(run_callback(type, params, {program}), expected);
        require_matches(run_callback(type, params, {program}, 0), expected);
    }
}
