#include <catch2/catch_test_macros.hpp>

#include "support/audio_contracts.hpp"

#include <pulp/format/processor.hpp>
#include <pulp/signal/oversampling.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::test::audio;

namespace {

struct OversamplingConfig {
    pulp::signal::Oversampler::Factor factor;
    pulp::signal::Oversampler::Quality quality;
    int expected_latency;
};

constexpr std::array<OversamplingConfig, 8> kConfigurations = {{
    {pulp::signal::Oversampler::Factor::x2, pulp::signal::Oversampler::Quality::standard, 64},
    {pulp::signal::Oversampler::Factor::x4, pulp::signal::Oversampler::Quality::standard, 76},
    {pulp::signal::Oversampler::Factor::x8, pulp::signal::Oversampler::Quality::standard, 80},
    {pulp::signal::Oversampler::Factor::x16, pulp::signal::Oversampler::Quality::standard, 82},
    {pulp::signal::Oversampler::Factor::x2, pulp::signal::Oversampler::Quality::pristine, 192},
    {pulp::signal::Oversampler::Factor::x4, pulp::signal::Oversampler::Quality::pristine, 209},
    {pulp::signal::Oversampler::Factor::x8, pulp::signal::Oversampler::Quality::pristine, 216},
    {pulp::signal::Oversampler::Factor::x16, pulp::signal::Oversampler::Quality::pristine, 219},
}};

OversamplingConfig g_config = kConfigurations[0];

class OversamplingIdentityProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "OversamplingLatencyFixture",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.oversampling-latency",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore&) override {}

    void prepare(const pulp::format::PrepareContext& context) override {
        oversamplers_.clear();
        oversamplers_.resize(static_cast<std::size_t>(context.output_channels));
        for (auto& oversampler : oversamplers_) {
            oversampler.set_kind(pulp::signal::Oversampler::Kind::linear_phase_fir);
            oversampler.set_quality(g_config.quality);
            oversampler.set_factor(g_config.factor);
            oversampler.set_sample_rate(static_cast<float>(context.sample_rate));
        }
        reported_latency_ = oversamplers_.empty() ? 0 : oversamplers_.front().latency_samples();
    }

    int latency_samples() const override {
        return reported_latency_;
    }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in, pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&, const pulp::format::ProcessContext&) override {
        const std::size_t channels =
            std::min({out.num_channels(), in.num_channels(), oversamplers_.size()});
        for (std::size_t channel = 0; channel < channels; ++channel) {
            oversamplers_[channel].process_block(in.channel_ptr(channel), out.channel_ptr(channel),
                                                 out.num_samples(),
                                                 [](float sample) { return sample; });
        }
    }

  private:
    std::vector<pulp::signal::Oversampler> oversamplers_;
    int reported_latency_ = 0;
};

std::unique_ptr<pulp::format::Processor> create_oversampling_identity() {
    return std::make_unique<OversamplingIdentityProcessor>();
}

pulp::audio::Buffer<float> make_aperiodic_multisine(int channels, int frames, double sample_rate) {
    constexpr double pi = 3.14159265358979323846;
    pulp::audio::Buffer<float> input(static_cast<std::size_t>(channels),
                                     static_cast<std::size_t>(frames));
    for (int channel = 0; channel < channels; ++channel) {
        auto samples = input.channel(static_cast<std::size_t>(channel));
        for (int frame = 0; frame < frames; ++frame) {
            double value = 0.0;
            for (int tone = 0; tone < 23; ++tone) {
                const double frequency = 173.13 + 337.71 * tone;
                const double phase = 0.37 * tone + 0.19 * channel;
                value += std::sin(2.0 * pi * frequency * frame / sample_rate + phase);
            }
            samples[static_cast<std::size_t>(frame)] = static_cast<float>(0.015 * value);
        }
    }
    return input;
}

} // namespace

TEST_CASE("Oversampling latency is proven and pinned through the audio contract",
          "[audio][contract][latency][oversampling]") {
    constexpr int frames = 8192;
    constexpr double sample_rate = 48000.0;

    for (std::size_t index = 0; index < kConfigurations.size(); ++index) {
        g_config = kConfigurations[index];
        const int block_size = std::array{31, 64, 127, 256}[index % 4];
        auto result = RenderScenario(create_oversampling_identity)
                          .name("oversampling.linear-phase.identity")
                          .sample_rate(sample_rate)
                          .block_size(block_size)
                          .input(make_aperiodic_multisine(2, frames, sample_rate))
                          .render();

        DelayedNullOptions options;
        options.max_delay_samples = 300;
        auto evidence = evaluate_reported_latency(result, options);
        apply_expected_samples(evidence, g_config.expected_latency);

        INFO("factor=" << static_cast<int>(g_config.factor) << " expected="
                       << g_config.expected_latency << " block=" << block_size << "\n"
                       << latency_evidence_summary(evidence));
        REQUIRE(evidence.reported_samples == g_config.expected_latency);
        REQUIRE(evidence.measured_samples == g_config.expected_latency);
        REQUIRE(evidence.delta_samples == 0);
        REQUIRE(evidence.expected_samples == g_config.expected_latency);
        REQUIRE(evidence.contract_outcome == LatencyContractOutcome::satisfied);
        REQUIRE_FALSE(evidence.gates_failure());

        AudioContract contract("oversampler reports its true pinned latency", std::move(result));
        contract.expect(expect_reported_latency(contract.result(), options));
        const auto verdict = contract.verify();
        INFO(verdict.message);
        REQUIRE(verdict.passed);
    }
}
