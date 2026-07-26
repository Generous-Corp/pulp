// The modulation family's bake-layer catalog suite.
//
// The DSP block's own acceptance suite (test_signal_frequency_shifter_ssb.cpp)
// proves the shifter; this file proves the NODE — that the graph, the bake and
// the parameter-injection channel deliver those parameters to it in real units,
// in the right order, on the right rail, without allocating.
//
// Measurement recipe. fs = 48 kHz over 128-frame blocks, so the block's DFT bin
// spacing is exactly 375 Hz and every frequency named here is a whole multiple
// of it — the analysis window holds a whole number of periods of each, and a
// coherent read at one of them contains nothing of its neighbours. The test
// tone is 3 kHz (16 samples per period, 8 whole periods per block) and the
// shift is 1500 Hz, which puts the retained sideband at 4500 Hz and the image
// at 1500 Hz, both on bins and both well clear of the tone.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_modulation_catalog.hpp>

#include <cmath>
#include <vector>

using namespace pulp::host;
namespace mod = pulp::host::modulation;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kBinHz = kSr / kFrames;  // 375 Hz
constexpr double kToneHz = 3000.0;        // 8 whole periods per block
constexpr double kShiftHz = 1500.0;       // 4 bins
constexpr float kAmplitude = 0.5f;

/// Blocks to render before measuring. The node's parameters are de-zippered
/// with a 20 ms time constant and every injection starts from the registered
/// DEFAULT, so a short settle measures the ramp rather than the setting — the
/// first draft of this file read 0.308 where it expected 0.5 for exactly that
/// reason. 256 blocks is 32768 samples, about 34 time constants, which lands
/// the smoothers within 1e-15 of their targets.
constexpr int kSettleBlocks = 256;

// TRUE STEREO: the node's two ports are L and R of one logical wire, so the
// fixture is built with two channels rather than instanced twice. The
// stereo-split case below depends on that being true.
using Fixture = pulp::test::BakedNodeFixture<2>;

Fixture make_fixture() { return Fixture(mod::make_frequency_shifter_node(), kSr, kFrames); }

std::vector<float> tone(float amp = kAmplitude) {
    return pulp::test::sine_block(kFrames, kToneHz, kSr, amp);
}

std::vector<float> silence() { return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f); }

/// Coherent DFT magnitude at `hz` over one block. Exact for any whole multiple
/// of `kBinHz`, which every call site below is.
double magnitude_at(const std::vector<float>& x, double hz) {
    const double w = 2.0 * M_PI * hz / kSr;
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        re += x[n] * std::cos(w * static_cast<double>(n));
        im += x[n] * std::sin(w * static_cast<double>(n));
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

bool on_bin(double hz) {
    const double bins = hz / kBinHz;
    return std::abs(bins - std::round(bins)) < 1e-9;
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

/// Sets the node to a plain up-shift with the feedback path idle.
void set_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(mod::kShiftHz, static_cast<float>(kShiftHz))) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(mod::kFeedback, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(mod::kMix, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeUp)) == InjectStatus::Ok);
}

}  // namespace

TEST_CASE("Forge modulation: the recipe's frequencies are all on analysis bins",
          "[host][baked][forge][forge-modulation]") {
    // Guards the measurement rather than the code. A frequency off a bin makes
    // every magnitude read in this file leaky, and the failure would look like
    // a DSP bug rather than a recipe bug.
    for (double hz : {kToneHz, kShiftHz, kToneHz + kShiftHz, kToneHz - kShiftHz,
                      kToneHz + 2.0 * kShiftHz})
        REQUIRE(on_bin(hz));
}

TEST_CASE("Forge modulation: the frequency shifter bakes and runs",
          "[host][baked][forge][forge-modulation]") {
    auto fx = make_fixture();
    const auto t = tone();
    const auto out = fx.settle({t, t}, kSettleBlocks);
    for (int ch = 0; ch < 2; ++ch)
        for (float v : out[static_cast<std::size_t>(ch)]) REQUIRE(std::isfinite(v));
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge modulation: injecting shift_hz relocates the tone",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    const auto out = fx.settle({t, t}, kSettleBlocks)[0];
    // All of it arrives at f + shift, and the input frequency is emptied.
    REQUIRE_THAT(magnitude_at(out, kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(out, kToneHz) < kAmplitude * 0.05);

    // Zero shift passes the signal through: allpass, so magnitude survives.
    REQUIRE(inj.inject(immediate(mod::kShiftHz, 0.0f)) == InjectStatus::Ok);
    const auto through = fx.settle({t, t}, kSettleBlocks)[0];
    REQUIRE_THAT(magnitude_at(through, kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
}

TEST_CASE("Forge modulation: the mode param selects the sideband",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeDown)) == InjectStatus::Ok);
    const auto down = fx.settle({t, t}, kSettleBlocks)[0];
    REQUIRE_THAT(magnitude_at(down, kToneHz - kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(down, kToneHz + kShiftHz) < kAmplitude * 0.05);

    // `dual_mono` is the up combine under a name that says "deliberately no
    // stereo differentiation"; asserted so the enum cannot quietly drift.
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeDualMono)) == InjectStatus::Ok);
    const auto dual = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(dual[0], kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    for (std::size_t n = 0; n < dual[0].size(); ++n) REQUIRE(dual[0][n] == dual[1][n]);
}

TEST_CASE("Forge modulation: stereo split survives the graph",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // The true-stereo wiring assertion. `stereo_split` drives the left channel
    // up and the right down from ONE shared carrier, so the two rails have to
    // be processed in the same call. A node wired dual-mono would pass every
    // other test in this file and fail here, with both channels shifted up.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeStereoSplit)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 100.0f)) == InjectStatus::Ok);

    const auto t = tone();
    const auto out = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(out[0], kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(out[1], kToneHz - kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(out[0], kToneHz - kShiftHz) < kAmplitude * 0.05);
    REQUIRE(magnitude_at(out[1], kToneHz + kShiftHz) < kAmplitude * 0.05);
}

TEST_CASE("Forge modulation: stereo spread scales the split",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeStereoSplit)) == InjectStatus::Ok);
    const auto t = tone();

    // The spread scales the CARRIER, so half spread is a half-size shift on
    // each side rather than a blend of both sidebands. 750 Hz is two bins.
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 50.0f)) == InjectStatus::Ok);
    const auto half = fx.settle({t, t}, kSettleBlocks);
    REQUIRE(on_bin(kToneHz + 0.5 * kShiftHz));
    REQUIRE_THAT(magnitude_at(half[0], kToneHz + 0.5 * kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(half[1], kToneHz - 0.5 * kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));

    // At zero spread the carrier stops and both rails carry the input at full
    // magnitude — not a ring-modulated pair of half-amplitude sidebands.
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 0.0f)) == InjectStatus::Ok);
    const auto none = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(none[0], kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(none[1], kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
}

TEST_CASE("Forge modulation: feedback recirculates through the shifter again",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // The barberpole signature, made measurable: each pass adds ANOTHER shift,
    // so energy appears at f + 2*shift that a single pass cannot produce. A
    // feedback path wired around the shifter rather than through it would put
    // the recirculated energy back at f + shift and fail this.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kFeedbackDelayMs, 1.0f)) == InjectStatus::Ok);
    const auto t = tone();

    const auto dry = fx.settle({t, t}, kSettleBlocks)[0];
    const double without = magnitude_at(dry, kToneHz + 2.0 * kShiftHz);

    REQUIRE(inj.inject(immediate(mod::kFeedback, 0.8f)) == InjectStatus::Ok);
    const auto wet = fx.settle({t, t}, kSettleBlocks)[0];
    const double with = magnitude_at(wet, kToneHz + 2.0 * kShiftHz);

    INFO("second pass " << without << " -> " << with);
    REQUIRE(with > without * 10.0);
    REQUIRE(with > 0.05 * kAmplitude);
}

TEST_CASE("Forge modulation: mix blends against the whole wet chain",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    REQUIRE(inj.inject(immediate(mod::kMix, 0.0f)) == InjectStatus::Ok);
    const auto dry = fx.settle({t, t}, kSettleBlocks)[0];
    // Fully dry is the input, unaltered — not merely close to it.
    for (int n = 0; n < kFrames; ++n)
        REQUIRE_THAT(static_cast<double>(dry[static_cast<std::size_t>(n)]),
                     WithinAbs(static_cast<double>(t[static_cast<std::size_t>(n)]), 1e-6));

    REQUIRE(inj.inject(immediate(mod::kMix, 50.0f)) == InjectStatus::Ok);
    const auto half = fx.settle({t, t}, kSettleBlocks)[0];
    // Half the dry tone and half the shifted one, both present at once.
    REQUIRE_THAT(magnitude_at(half, kToneHz), WithinRel(0.5 * kAmplitude, 0.1));
    REQUIRE_THAT(magnitude_at(half, kToneHz + kShiftHz), WithinRel(0.5 * kAmplitude, 0.1));
}

TEST_CASE("Forge modulation: the baked table is the DSP block's own contract",
          "[host][baked][forge][forge-modulation]") {
    // Ranges are declared once, in the DSP header, and the node's table quotes
    // them. Asserted rather than eyeballed so the two cannot drift.
    using Shifter = pulp::signal::SsbFrequencyShifter;
    const auto type = mod::make_frequency_shifter_node();
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.lowerable);
    REQUIRE(type.baked_params.size() == 6);

    auto find = [&type](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return p;
        FAIL("missing baked param");
        return type.baked_params.front();
    };

    const auto shift = find(mod::kShiftHz);
    REQUIRE_THAT(shift.min_value, WithinRel(-static_cast<float>(Shifter::kMaxShiftHz), 1e-6f));
    REQUIRE_THAT(shift.max_value, WithinRel(static_cast<float>(Shifter::kMaxShiftHz), 1e-6f));
    REQUIRE(shift.default_value == 0.0f);

    const auto feedback = find(mod::kFeedback);
    REQUIRE(feedback.min_value == 0.0f);
    REQUIRE_THAT(feedback.max_value, WithinRel(static_cast<float>(Shifter::kMaxFeedback), 1e-6f));
    REQUIRE(feedback.default_value == 0.0f);

    const auto delay = find(mod::kFeedbackDelayMs);
    REQUIRE_THAT(delay.min_value, WithinRel(static_cast<float>(Shifter::kMinDelayMs), 1e-6f));
    REQUIRE_THAT(delay.max_value, WithinRel(static_cast<float>(Shifter::kMaxLoopMs), 1e-6f));

    const auto mode = find(mod::kShiftMode);
    REQUIRE(mode.min_value == mod::kModeUp);
    REQUIRE(mode.max_value == mod::kModeStereoSplit);
    REQUIRE(mode.default_value == mod::kModeUp);
}

TEST_CASE("Forge modulation: the stepped mode param rounds to the nearest step",
          "[host][baked][forge][forge-modulation]") {
    using pulp::signal::FrequencyShiftMode;
    REQUIRE(mod::mode_from_param(0.0f) == FrequencyShiftMode::up);
    REQUIRE(mod::mode_from_param(0.49f) == FrequencyShiftMode::up);
    // A host ramping toward a stepped value must land on the nearest step, not
    // sit on the one below it for the whole ramp.
    REQUIRE(mod::mode_from_param(0.51f) == FrequencyShiftMode::down);
    REQUIRE(mod::mode_from_param(2.0f) == FrequencyShiftMode::dual_mono);
    REQUIRE(mod::mode_from_param(3.0f) == FrequencyShiftMode::stereo_split);
    // Out of range clamps to a valid mode rather than reading past the enum.
    REQUIRE(mod::mode_from_param(-7.0f) == FrequencyShiftMode::up);
    REQUIRE(mod::mode_from_param(99.0f) == FrequencyShiftMode::up);
}

TEST_CASE("Forge modulation: the registry's worst-case gain is the loop envelope",
          "[host][baked][forge][forge-modulation]") {
    // Series law 8: a tested invariant, not an estimate. The DSP suite measures
    // both factors of it; this asserts the registry quotes the same number.
    using Shifter = pulp::signal::SsbFrequencyShifter;
    const double expected = 1.0 / (1.0 - Shifter::kMaxFeedback * Shifter::kGshiftBudget);
    REQUIRE_THAT(static_cast<double>(mod::ssb_frequency_shifter_worst_case_gain()),
                 WithinRel(expected, 1e-6));
}

TEST_CASE("Forge modulation: the reported latency is zero through the graph",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // At zero shift and full wet the node is an allpass network with no bulk
    // delay, so an impulse's FIRST output sample is already non-zero. A node
    // that had acquired latency somewhere in the bake would show a run of
    // zeros first.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    fx.settle({silence(), silence()}, 8);

    auto impulse = silence();
    impulse[0] = 0.5f;
    const auto first = fx.render({impulse, impulse});
    REQUIRE(std::fabs(first[0][0]) > 1e-4f);
}

TEST_CASE("Forge modulation: the node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][rt-safety]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);

    // Buffers and views built outside the probe, which is what
    // `ReusableRenderer` exists for. The fixture's convenience `render()`
    // constructs its own output vectors, so driving it from inside a probe
    // would report the harness's allocations as the node's.
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(mod::kShiftHz, -2000.0f + 125.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kFeedback, 0.02f * static_cast<float>(b)));
        inj.inject(immediate(mod::kFeedbackDelayMs, 0.1f + 1.5f * static_cast<float>(b % 32)));
        inj.inject(immediate(mod::kMix, 100.0f - 2.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kStereoSpread, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kShiftMode, static_cast<float>(b % 4)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
