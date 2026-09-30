// test_transition_cost.cpp — deterministic per-callback cost gate at state
// transitions.
//
// Offline renders have no deadline, so a transition that bursts work into one
// callback renders cleanly offline and drops a buffer in a host. These tests
// cover the operation-count instrument (RtWorkCounter hooks in Fft and
// rt::polar), the per-block cost records RenderScenario now carries, the
// TransitionScenario catalog, and the gate itself — including its negative
// control, BurstyProcessor<23>, which must fail. Counts do not depend on load,
// so everything here is required-lane safe; the timing twin lives in
// test_transition_cost_timing.cpp under the `performance` label.

#include <catch2/catch_test_macros.hpp>

#include "support/bursty_processor.hpp"
#include "support/transition_scenario.hpp"

#include <pulp/signal/fft.hpp>
#include <pulp/signal/fft_backend.hpp>
#include <pulp/signal/freeze_hold.hpp>
#include <pulp/signal/spectral_mask_processor.hpp>
#include <pulp/signal/rt_work_counter.hpp>

#include <algorithm>
#include <complex>
#include <memory>
#include <string>
#include <vector>

using namespace pulp::test::audio;
namespace rt = pulp::signal::rt;

TEST_CASE("RtWorkCounter is compiled into test builds",
          "[transition-cost][rt-work-counter]") {
    // Every gate below reads these counters. A build without them would make
    // each gate report "no work" rather than "not measured"; the ops gate
    // refuses that case, and this test makes the configuration explicit.
    REQUIRE(rt::kWorkCountersEnabled);
}

TEST_CASE("Fft counts each transform exactly once",
          "[transition-cost][rt-work-counter]") {
    pulp::signal::Fft fft(256);
    std::vector<std::complex<float>> data(256, {1.0f, 0.0f});
    std::vector<float> real(256, 0.5f);

    rt::RtWorkCounter counter;
    fft.forward(data.data());
    CHECK(counter.delta().fft == 1);
    fft.inverse(data.data());
    CHECK(counter.delta().fft == 2);
    fft.forward_real(real.data(), data.data());
    CHECK(counter.delta().fft == 3);
    CHECK(counter.delta().trig == 0);

    pulp::signal::Fft64 fft64(64);
    std::vector<std::complex<double>> data64(64, {1.0, 0.0});
    std::vector<double> real64(64, 0.25);
    counter.restart();
    fft64.forward_real(real64.data(), data64.data());
    CHECK(counter.delta().fft == 1);
}

TEST_CASE("MultiBackendFft counts each transform exactly once",
          "[transition-cost][rt-work-counter]") {
    pulp::signal::MultiBackendFft fft(128, pulp::signal::FftBackend::kissfft);
    std::vector<std::complex<float>> data(128, {1.0f, 0.0f});
    rt::RtWorkCounter counter;
    fft.forward(data.data());
    fft.inverse(data.data());
    CHECK(counter.delta().fft == 2);
}

TEST_CASE("rt::polar and friends count and stay bit-identical",
          "[transition-cost][rt-work-counter]") {
    rt::RtWorkCounter counter;
    const auto z = rt::polar(0.75f, 1.25f);
    CHECK(z == std::polar(0.75f, 1.25f));
    CHECK(rt::sin(0.3) == std::sin(0.3));
    CHECK(rt::cos(0.3f) == std::cos(0.3f));
    CHECK(rt::exp(-2.0) == std::exp(-2.0));
    const std::complex<double> w(0.2, -0.7);
    CHECK(rt::arg(w) == std::arg(w));
    CHECK(counter.delta().trig == 5);
    CHECK(counter.delta().fft == 0);
    rt::count_bins(4097);
    CHECK(counter.delta().bins == 4097);
}

TEST_CASE("RenderScenario records one cost per processed block",
          "[transition-cost][render-scenario]") {
    const auto result = RenderScenario(make_bursty_processor<0>)
                            .block_size(128)
                            .duration_frames(128 * 10 + 17)
                            .input([](double, int ch, std::int64_t n) {
                                return make_white_noise(ch, static_cast<int>(n),
                                                        3u, 0.5f);
                            })
                            .automate(ParamStep{kBurstyGain, 128 * 4, 0.5f})
                            .render();
    REQUIRE(result.block_costs.size() == 11);
    CHECK(result.block_costs.back().frames == 17);
    CHECK(result.block_costs[4].start_frame == 128 * 4);
    for (std::size_t i = 0; i < result.block_costs.size(); ++i) {
        CHECK(result.block_costs[i].param_steps == (i == 4 ? 1 : 0));
        // The fixture does exactly one analysis FFT per block.
        CHECK(result.block_costs[i].ops.fft == 1);
        CHECK(result.block_costs[i].wall_ns >= 0);
    }
}

TEST_CASE("standard transition catalog comes from define_parameters",
          "[transition-cost][catalog]") {
    const auto cases = standard_transition_cases(make_bursty_processor<0>);
    REQUIRE(cases.size() == 4);
    const auto has = [&](pulp::state::ParamID id, float from, float to) {
        return std::any_of(cases.begin(), cases.end(), [&](const auto& c) {
            return c.id == id && c.from == from && c.to == to;
        });
    };
    CHECK(has(kBurstyEngage, 0.0f, 1.0f));
    CHECK(has(kBurstyEngage, 1.0f, 0.0f));
    CHECK(has(kBurstyGain, 0.0f, 1.0f));
    CHECK(has(kBurstyGain, 1.0f, 0.0f));
}

TEST_CASE("transition ops gate passes a processor with no transition burst",
          "[transition-cost][gate]") {
    const auto outcomes = TransitionScenario::standard(make_bursty_processor<0>)
                              .block_sizes({128, 32})
                              .warmup_blocks(4)
                              .settle_blocks(4)
                              .run();
    REQUIRE(outcomes.size() == 8);
    const auto check = assert_transition_ops_bounded(outcomes);
    INFO(check.message);
    CHECK(check.passed);
    for (const auto& outcome : outcomes) {
        // Positive control: the instrument sees the steady FFT. A zero here
        // would mean the gate passed because it measured nothing.
        CHECK(outcome.steady_ops_max.fft == 1);
        CHECK(outcome.transition_ops_max.fft == 1);
    }
}

TEST_CASE("transition ops gate fails a processor that bursts on an edge",
          "[transition-cost][gate][negative-control]") {
    const auto outcomes = TransitionScenario::standard(make_bursty_processor<23>)
                              .block_sizes({128, 32})
                              .warmup_blocks(4)
                              .settle_blocks(4)
                              .run();
    const auto check = assert_transition_ops_bounded(outcomes);
    INFO(check.message);
    REQUIRE_FALSE(check.passed);
    CHECK(check.message.find("Engage") != std::string::npos);
    CHECK(check.message.find("Gain") == std::string::npos);

    std::size_t engage_edges = 0;
    for (const auto& outcome : outcomes) {
        if (outcome.transition.id != kBurstyEngage)
            continue;
        ++engage_edges;
        CHECK(outcome.transition_ops_max.fft == 24);
        CHECK(outcome.transition_ops_max.trig ==
              23u * BurstyProcessor<23>::kBins);
        CHECK(outcome.transition_ops_worst_block == outcome.transition_block);
    }
    CHECK(engage_edges == 4);

    // A declared allowance that covers the burst passes; one short fails.
    CHECK(assert_transition_ops_bounded(
              outcomes, {.fft = 23, .trig = 23u * BurstyProcessor<23>::kBins})
              .passed);
    CHECK_FALSE(assert_transition_ops_bounded(
                    outcomes,
                    {.fft = 22, .trig = 23u * BurstyProcessor<23>::kBins})
                    .passed);
}

TEST_CASE("transition scenario rejects a warmup with no steady block",
          "[transition-cost][gate]") {
    auto scenario = TransitionScenario::standard(make_bursty_processor<0>)
                        .warmup_blocks(1);
    CHECK_THROWS_AS(scenario.run(), std::invalid_argument);
}

// ── FreezeHold through the gate ────────────────────────────────────────────
//
// A spectral freeze as a plugin would build it: FreezeHold as the pre-mask
// stage of a SpectralMaskProcessor, engaged by a toggle parameter. The
// default hold bursts its latch into one callback (the whole capture window
// summed, then a polar per bin per held hop); the bounded configuration
// (running sums, rotor synthesis, staged engage) stays within a declared
// allowance of the steady per-callback work.

namespace {

enum FreezeParams : pulp::state::ParamID { kFreeze = 1 };

template <bool Bounded>
class FreezeProcessor final : public pulp::format::Processor {
public:
    static constexpr int kFft = 4096;
    static constexpr int kHop = 512;
    static constexpr int kBins = kFft / 2 + 1;
    static constexpr int kStaging = 512;

    pulp::format::PluginDescriptor descriptor() const override {
        return {
            .name = "FreezeProcessor",
            .manufacturer = "Pulp",
            .bundle_id = "com.pulp.test.freeze",
            .version = "1.0.0",
            .category = pulp::format::PluginCategory::Effect,
            .input_buses = {{"Audio In", 2}},
            .output_buses = {{"Audio Out", 2}},
        };
    }

    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({
            .id = kFreeze,
            .name = "Freeze",
            .range = {0.0f, 1.0f, 0.0f, 1.0f},
            .kind = pulp::state::ParamKind::Toggle,
        });
    }

    void prepare(const pulp::format::PrepareContext& context) override {
        pulp::signal::SpectralMaskProcessorConfig config;
        config.frame.fft_size = kFft;
        config.frame.analysis_hop = kHop;
        config.frame.channels = 2;
        config.frame.max_block = context.max_buffer_size;
        config.frame.window = pulp::signal::WindowFunction::Type::hann;
        config.sample_rate = static_cast<float>(context.sample_rate);
        (void)spectral_.prepare(config);

        pulp::signal::FreezeHold::Config hold;
        hold.fft_size = kFft;
        hold.channels = 2;
        hold.analysis_hop = kHop;
        hold.sample_rate = context.sample_rate;
        hold.capture_seconds = 0.5;
        if constexpr (Bounded) {
            hold.synthesis = pulp::signal::FreezeHoldSynthesis::rotor;
            hold.frequency = pulp::signal::FreezeHoldFrequency::energy_weighted;
            hold.average = pulp::signal::FreezeHoldAverage::running;
            hold.engage_bins_per_hop = kStaging;
        }
        stage_.hold.prepare(hold);
        spectral_.set_pre_mask_stage(&stage_);
    }

    void process(pulp::audio::BufferView<float>& output,
                 const pulp::audio::BufferView<const float>& input,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        stage_.hold.set_frozen(state().get_value(kFreeze) >= 0.5f);
        const float* in[] = {input.channel(0).data(), input.channel(1).data()};
        float* out[] = {output.channel(0).data(), output.channel(1).data()};
        (void)spectral_.process(in, out, static_cast<int>(output.num_samples()));
    }

private:
    struct Stage final : pulp::signal::SpectralPreMaskStage {
        pulp::signal::FreezeHold hold;
        void process_frames(std::complex<float>* const* frames, int channels,
                            int num_bins) noexcept override {
            hold.process_group(frames, channels, num_bins);
        }
    };
    pulp::signal::SpectralMaskProcessor spectral_;
    Stage stage_;
};

template <bool Bounded>
std::unique_ptr<pulp::format::Processor> make_freeze_processor() {
    return std::make_unique<FreezeProcessor<Bounded>>();
}

template <bool Bounded>
std::vector<TransitionOutcome> freeze_engage_outcomes() {
    // 0.5 s of capture at a 512-sample hop is 47 frames; the first frame
    // completes after 4096 samples. 240 blocks of 128 fill the window with
    // margin, and 64 settle blocks cover the fade and the staged engage.
    return TransitionScenario(make_freeze_processor<Bounded>)
        .add({"Freeze engage", kFreeze, 0.0f, 1.0f})
        .block_sizes({128})
        .warmup_blocks(240)
        .settle_blocks(64)
        .run();
}

} // namespace

TEST_CASE("transition ops gate: FreezeHold engage bursts by default",
          "[transition-cost][gate][freeze][negative-control]") {
    const auto outcomes = freeze_engage_outcomes<false>();
    const auto check = assert_transition_ops_bounded(
        outcomes, {.trig = 2u * FreezeProcessor<true>::kStaging,
                   .bins = 2u * FreezeProcessor<true>::kBins});
    INFO(check.message);
    CHECK_FALSE(check.passed);
    // The whole 47-frame window is summed in the latching callback.
    CHECK(outcomes.front().transition_ops_max.bins >=
          47u * 2u * FreezeProcessor<false>::kBins);
}

TEST_CASE("transition ops gate: bounded FreezeHold engage stays within its allowance",
          "[transition-cost][gate][freeze]") {
    const auto outcomes = freeze_engage_outcomes<true>();
    // Declared transition allowance: one staged chunk of rotors and
    // frequencies (a polar and an atan2 per bin), and one divide per channel
    // bin at the latch.
    const auto check = assert_transition_ops_bounded(
        outcomes, {.trig = 2u * FreezeProcessor<true>::kStaging,
                   .bins = 2u * FreezeProcessor<true>::kBins});
    INFO(check.message);
    CHECK(check.passed);
    // Positive control: the gate saw the STFT and the staged engage.
    CHECK(outcomes.front().steady_ops_max.fft > 0);
    CHECK(outcomes.front().transition_ops_max.trig == 2u * FreezeProcessor<true>::kStaging);
}
