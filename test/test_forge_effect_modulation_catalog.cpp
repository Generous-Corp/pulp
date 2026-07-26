// The modulation family's bake-layer catalog suite.
//
// The DSP blocks' own acceptance suites prove the effects; this file proves the
// NODES — that the graph, the bake and the parameter-injection channel deliver
// those parameters to them in real units, in the right order, on the right rail,
// without allocating.
//
// Four lineages live here: the SSB frequency shifter, the chorus ensemble, the
// phaser, and the three vibrato engines. Every baked param below is shown to
// MOVE THE BAKED NODE'S AUDIO through the real production path — bake,
// claim_param_injection, ParamInjector, routed executor — because a test that
// only instantiates a node proves the registration compiled, not that the knob
// is wired to anything.
//
// Where a param has a PREDICTABLE effect the prediction is asserted rather than
// a difference: the phaser's centre frequency is checked by putting a tone on
// the notch the shipped notch law says it creates, the delay vibrato's depth by
// measuring the cents it actually shifts, and the two mix controls by requiring
// a bit-exact dry passthrough at zero. A difference test would pass on a node
// that wired the knob to the wrong parameter.
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

#include <pulp/host/forge_effect_modulation_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <utility>
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

// ═══════════════════════════════════════════════════════════════════════════
//  Shared instruments for the three lineages below
// ═══════════════════════════════════════════════════════════════════════════

namespace {

using Mono = pulp::test::BakedNodeFixture<1>;
using Stereo = pulp::test::BakedNodeFixture<2>;

/// Concatenates `blocks` rendered blocks of one channel into a single trace.
///
/// The input block is repeated, which is phase-continuous precisely because
/// every tone in this file holds a whole number of periods per block — the same
/// property `on_bin` guards. A tone that did not would step in phase at every
/// block boundary and every measurement below would read that step as
/// modulation.
template <int Channels>
std::vector<float> capture(pulp::test::BakedNodeFixture<Channels>& fx,
                           const std::vector<std::vector<float>>& in, int blocks,
                           int channel = 0) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(blocks * kFrames));
    for (int b = 0; b < blocks; ++b) {
        const auto block = fx.render(in);
        const auto& ch = block[static_cast<std::size_t>(channel)];
        out.insert(out.end(), ch.begin(), ch.end());
    }
    return out;
}

/// Both rails of `blocks` rendered blocks. Side-signal measurements need L and
/// R from the SAME render, which two single-channel captures cannot give.
std::pair<std::vector<float>, std::vector<float>> capture_pair(
    Stereo& fx, const std::vector<std::vector<float>>& in, int blocks) {
    std::pair<std::vector<float>, std::vector<float>> out;
    for (int b = 0; b < blocks; ++b) {
        const auto block = fx.render(in);
        out.first.insert(out.first.end(), block[0].begin(), block[0].end());
        out.second.insert(out.second.end(), block[1].begin(), block[1].end());
    }
    return out;
}

/// Instantaneous frequency of a modulated carrier, by complex demodulation.
///
/// The same instrument the Leslie suite uses, and chosen over peak tracking for
/// the same reason: a peak detector on a modulated carrier samples the
/// modulator only at the carrier's peaks, so it reports the beat between the
/// two rather than the modulation.
struct Demod {
    std::vector<double> envelope;
    std::vector<double> freq_hz;
    double rate_hz = 0.0;
};

Demod demodulate(const std::vector<float>& x, double carrier_hz, double lowpass_hz,
                 int decimation) {
    Demod out;
    out.rate_hz = kSr / decimation;
    const double pole = std::exp(-2.0 * M_PI * lowpass_hz / kSr);
    double si[4] = {0, 0, 0, 0};
    double sq[4] = {0, 0, 0, 0};
    double previous = 0.0;
    bool have = false;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = 2.0 * M_PI * carrier_hz * static_cast<double>(n) / kSr;
        double i = x[n] * std::cos(w);
        double q = -x[n] * std::sin(w);
        for (int k = 0; k < 4; ++k) {
            si[k] = pole * si[k] + (1.0 - pole) * i;
            i = si[k];
            sq[k] = pole * sq[k] + (1.0 - pole) * q;
            q = sq[k];
        }
        if (static_cast<int>(n) % decimation != 0) continue;
        out.envelope.push_back(2.0 * std::hypot(i, q));
        const double phase = std::atan2(q, i);
        if (have) {
            double d = phase - previous;
            while (d > M_PI) d -= 2.0 * M_PI;
            while (d < -M_PI) d += 2.0 * M_PI;
            out.freq_hz.push_back(carrier_hz + d * out.rate_hz / (2.0 * M_PI));
        }
        previous = phase;
        have = true;
    }
    return out;
}

/// Peak of `|trace − centre|` over the settled tail. The statistic for a SINE
/// deviation, which is what every modulator in this file produces.
double peak_deviation(const std::vector<double>& trace, double centre) {
    double peak = 0.0;
    for (std::size_t i = trace.size() / 3; i < trace.size(); ++i)
        peak = std::max(peak, std::abs(trace[i] - centre));
    return peak;
}

/// The frequency of the strongest component of a trace within a band, by
/// scanning the coherent DFT on a fine grid rather than reading an FFT bin.
double locate_rate(const std::vector<double>& trace, double lo_hz, double hi_hz,
                   double rate_hz, int steps = 2000) {
    double sum = 0.0;
    for (double v : trace) sum += v;
    const double mean = sum / static_cast<double>(trace.size());

    double best_hz = lo_hz;
    double best_mag = -1.0;
    for (int k = 0; k <= steps; ++k) {
        const double hz = lo_hz + (hi_hz - lo_hz) * k / steps;
        std::complex<double> acc{0.0, 0.0};
        for (std::size_t n = 0; n < trace.size(); ++n) {
            const double w = 2.0 * M_PI * hz * static_cast<double>(n) / rate_hz;
            acc += (trace[n] - mean) * std::complex<double>(std::cos(w), -std::sin(w));
        }
        const double mag = std::abs(acc);
        if (mag > best_mag) {
            best_mag = mag;
            best_hz = hz;
        }
    }
    return best_hz;
}

/// Coherent magnitude at an arbitrary frequency over a long trace. Unlike
/// `magnitude_at` above, this does not require the frequency to land on a
/// block's DFT bin — the trace is long enough that the residual is negligible.
double trace_magnitude_at(const std::vector<float>& x, double hz) {
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = 2.0 * M_PI * hz * static_cast<double>(n) / kSr;
        re += x[n] * std::cos(w);
        im += x[n] * std::sin(w);
    }
    const double scale = 2.0 / static_cast<double>(x.size());
    return std::hypot(re * scale, im * scale);
}

std::vector<float> tone_at(double hz, float amp = kAmplitude) {
    return pulp::test::sine_block(kFrames, hz, kSr, amp);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
//  Chorus
// ═══════════════════════════════════════════════════════════════════════════

namespace chorus_ns = pulp::host::modulation::chorus;

TEST_CASE("Forge modulation: the chorus voicings are distinct registrations",
          "[host][baked][forge][forge-modulation][chorus]") {
    // The realization axis, asserted as an axis. Four voicings, three Juno
    // positions on one of them, and the colour stage on any — every combination
    // has to be a distinct type id, because a registry that gave two
    // differently-behaving nodes one id would load a session and sound wrong.
    using Voicing = chorus_ns::Voicing;
    using JunoMode = chorus_ns::JunoMode;
    std::vector<std::string> ids;
    for (auto v : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                   Voicing::tri_chorus})
        for (auto m : {JunoMode::mode_I, JunoMode::mode_II, JunoMode::mode_I_plus_II})
            for (bool bbd : {false, true}) {
                const auto type = chorus_ns::make_chorus_node(v, m, bbd);
                REQUIRE(type.lowerable);
                REQUIRE(type.num_input_ports == 2);
                REQUIRE(type.num_output_ports == 2);
                REQUIRE(type.baked_params.size() == 4);
                if (v == Voicing::juno_ensemble || m == JunoMode::mode_I)
                    ids.push_back(type.type_id);
            }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // And the reason they are realizations rather than a knob: the voicings do
    // not share a voice count, so switching one is a topology change.
    using Engine = chorus_ns::Engine;
    REQUIRE(Engine::calibration(Voicing::ce2).voices == 1);
    REQUIRE(Engine::calibration(Voicing::dimension_d).voices == 2);
    REQUIRE(Engine::calibration(Voicing::tri_chorus).voices == 3);
}

TEST_CASE("Forge modulation: the chorus mix param reaches the engine",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    // At mix 0 the engine is a wire, so this is a bit-exact assertion rather
    // than a tolerance — and it fails on a node that wired `mix` to any other
    // parameter, which a "the output changed" test would not.
    auto fx = Stereo(chorus_ns::make_chorus_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(chorus_ns::kMix, 0.0f)) == InjectStatus::Ok);
    const auto t = tone();
    const auto out = fx.settle({t, t}, 32);
    for (int k = 0; k < kFrames; ++k) {
        REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
        REQUIRE(out[1][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    // ...and at full mix it is emphatically not a wire.
    REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
    const auto wet = fx.settle({t, t}, 32);
    double difference = 0.0;
    for (int k = 0; k < kFrames; ++k)
        difference = std::max(difference, std::abs(static_cast<double>(
                                              wet[0][static_cast<std::size_t>(k)] -
                                              t[static_cast<std::size_t>(k)])));
    REQUIRE(difference > 0.05);
}

TEST_CASE("Forge modulation: the chorus rate param sets the measured modulation speed",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    // Measured out of the audio, not read back off the setter. The CE-2 voicing
    // is used because the Juno's three modes run at their own fixed rates and
    // ignore this control by design — testing the rate there would assert the
    // opposite of the documented behaviour.
    //
    // What is asserted is the RATIO between two injected rates rather than the
    // rate itself, and that is not a weaker claim dressed up — it is the only
    // correct one for this effect. A chorus combs a dry copy against a delayed
    // one, and the CE-2's delay sweeps across many comb periods per LFO cycle
    // (±10 ms at 3 kHz is ±377 radians), so the envelope's fundamental is a
    // HARMONIC of the LFO whose order depends on the tone, the centre delay and
    // the depth. Asserting the envelope peak equals the injected rate would be
    // asserting something false; the DSP measured 6 Hz for a 1 Hz LFO here. The
    // comb geometry is identical between the two renders below because only the
    // rate differs, so the harmonic order cancels out of the ratio exactly.
    // A low tone and a shallow depth keep the comb's harmonic order down to
    // two, and the scan band is SCALED BY THE RATE so both renders can see the
    // same set of orders. With a fixed band the faster render's higher harmonics
    // fall outside it, the two peaks land on different orders, and the ratio
    // reads 0.4 instead of 2 — a measurement artefact that looks exactly like
    // the rate knob being wired backwards.
    constexpr double kLowToneHz = 375.0;  // one whole period per block
    const auto envelope_speed = [](double rate) {
        auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::ce2), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(chorus_ns::kRateHz, static_cast<float>(rate))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 5.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);

        const auto t = tone_at(kLowToneHz);
        fx.settle({t, t}, 64);
        const auto trace = capture(fx, {t, t}, 768);  // ~2 s
        const auto d = demodulate(trace, kLowToneHz, 100.0, 48);
        return locate_rate(d.envelope, 0.3 * rate, 12.0 * rate, d.rate_hz, 6000);
    };

    const double slow = envelope_speed(1.0);
    const double fast = envelope_speed(2.0);
    INFO("envelope speed at 1 Hz = " << slow << ", at 2 Hz = " << fast);
    REQUIRE(slow > 0.5);  // the modulation is present at all
    // The order is small and identical for both, which is what makes the ratio
    // meaningful — a large or differing order would mean the two renders were
    // not being compared on the same feature.
    REQUIRE(slow / 1.0 < 8.0);
    REQUIRE_THAT(fast / slow, WithinRel(2.0, 0.05));
}

TEST_CASE("Forge modulation: the chorus depth and width params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    const auto t = tone();

    // Depth 0 freezes the tap at its centre delay, so the whole node becomes a
    // time-INVARIANT comb: two consecutive settled blocks are then identical.
    // At full depth they cannot be. This is a stronger statement than "the
    // output changed" — it names what depth does.
    {
        auto fx = Stereo(chorus_ns::make_chorus_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
        fx.settle({t, t}, 64);
        const auto a = fx.render({t, t});
        const auto b = fx.render({t, t});
        for (int k = 0; k < kFrames; ++k)
            REQUIRE_THAT(a[0][static_cast<std::size_t>(k)],
                         WithinAbs(b[0][static_cast<std::size_t>(k)], 1e-6f));

        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 100.0f)) == InjectStatus::Ok);
        fx.settle({t, t}, 64);
        const auto c = fx.render({t, t});
        const auto e = fx.render({t, t});
        double moved = 0.0;
        for (int k = 0; k < kFrames; ++k)
            moved = std::max(moved, std::abs(static_cast<double>(
                                        c[0][static_cast<std::size_t>(k)] -
                                        e[0][static_cast<std::size_t>(k)])));
        REQUIRE(moved > 1e-3);
    }

    // Width drives the Dimension D's cross-feed, so it is measured on that
    // voicing — on the CE-2 it is a documented no-op, and asserting it there
    // would be asserting nothing.
    {
        // Averaged over WHOLE LFO CYCLES rather than one block. A single 2.7 ms
        // block is one instant of a 2 Hz sweep, and the instantaneous side
        // energy at one arbitrary LFO phase is not the quantity width scales —
        // measured that way the two settings read within 0.2 % of each other
        // while the engine itself separates them by a factor of two.
        const auto side_energy = [&t](float width) {
            auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::dimension_d), kSr,
                             kFrames);
            ParamInjector inj = fx.claim_injector();
            REQUIRE(inj.inject(immediate(chorus_ns::kStereoWidth, width)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(chorus_ns::kRateHz, 2.0f)) == InjectStatus::Ok);
            fx.settle({t, t}, 64);
            const auto pair = capture_pair(fx, {t, t}, 768);  // ~2 s, four cycles
            double energy = 0.0;
            for (std::size_t k = 0; k < pair.first.size(); ++k) {
                const double side =
                    static_cast<double>(pair.first[k]) - static_cast<double>(pair.second[k]);
                energy += side * side;
            }
            return std::sqrt(energy / static_cast<double>(pair.first.size()));
        };
        REQUIRE(side_energy(100.0f) > 1.7 * side_energy(0.0f));
    }
}

TEST_CASE("Forge modulation: the chorus registry gain is the DSP's own L1 bound",
          "[host][baked][forge][forge-modulation][chorus]") {
    // Series law 8. Delegated rather than restated, because two of its terms
    // are counterintuitive — each modulated tap carries the Lagrange kernel's
    // 1.25, and the Dimension D's cross-feed high-pass carries nearly 2.
    using Engine = chorus_ns::Engine;
    for (auto v : {chorus_ns::Voicing::ce2, chorus_ns::Voicing::dimension_d,
                   chorus_ns::Voicing::tri_chorus}) {
        Engine reference;
        reference.prepare(kSr);
        reference.set_voicing(v);
        reference.set_stereo_width(1.0f);
        REQUIRE_THAT(static_cast<double>(chorus_ns::chorus_worst_case_gain(v, kSr)),
                     WithinRel(reference.worst_case_gain(), 1e-6));
    }
    // Not the naive "one dry plus one tap": the tap alone is 1.25.
    REQUIRE(chorus_ns::chorus_worst_case_gain(chorus_ns::Voicing::ce2, kSr) > 2.0f);
}

TEST_CASE("Forge modulation: the chorus node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][chorus][rt-safety]") {
    auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::tri_chorus,
                                                 chorus_ns::JunoMode::mode_I, true),
                     kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(chorus_ns::kRateHz, 0.05f + 0.3f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kDepth, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kMix, 100.0f - 3.0f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kStereoWidth, 3.0f * static_cast<float>(b)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Phaser
// ═══════════════════════════════════════════════════════════════════════════

namespace phaser_ns = pulp::host::modulation::phaser;

namespace {

/// The centre frequency that puts the cascade's FIRST notch at `notch_hz`.
///
/// Bisected against the module's own shipped notch law rather than inverting it
/// by hand, so the expectation cannot drift from the implementation — and so
/// this reads as "ask the DSP where its notch is" rather than as a second copy
/// of the arctangent algebra.
double center_for_notch(double notch_hz, int stages) {
    double lo = 20.0, hi = 20000.0;
    for (int i = 0; i < 80; ++i) {
        const double mid = 0.5 * (lo + hi);
        if (phaser_ns::Engine::notch_frequency_hz(1, stages, mid, kSr) < notch_hz)
            lo = mid;
        else
            hi = mid;
    }
    return 0.5 * (lo + hi);
}

}  // namespace

TEST_CASE("Forge modulation: the phaser stage counts are distinct registrations",
          "[host][baked][forge][forge-modulation][phaser]") {
    using Engine = phaser_ns::Engine;
    std::vector<std::string> ids;
    for (int stages : {4, 6, 8, 10, 12}) {
        const auto type = phaser_ns::make_phaser_node(stages);
        REQUIRE(type.lowerable);
        REQUIRE(type.num_input_ports == 2);
        REQUIRE(type.num_output_ports == 2);
        REQUIRE(type.baked_params.size() == 8);
        ids.push_back(type.type_id);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Odd and out-of-range requests normalise the same way the DSP normalises
    // them, so the registered id can never claim a count the engine will not
    // run. Registering "7 stages" and running 6 would be a session that reloads
    // into a different effect.
    REQUIRE(phaser_ns::make_phaser_node(7).type_id == phaser_ns::make_phaser_node(6).type_id);
    REQUIRE(phaser_ns::make_phaser_node(99).type_id ==
            phaser_ns::make_phaser_node(Engine::kMaxStages).type_id);

    // And the reason it is a realization: the notch COUNT and the notch
    // FREQUENCIES are both functions of it, so two counts are two different
    // response functions rather than two values of one.
    REQUIRE(Engine::notch_count(4) == 2);
    REQUIRE(Engine::notch_count(12) == 6);
    REQUIRE(Engine::notch_frequency_hz(1, 4, 800.0, kSr) !=
            Engine::notch_frequency_hz(1, 12, 800.0, kSr));
}

TEST_CASE("Forge modulation: the phaser centre param puts the notch where the law says",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    // The strongest available statement about this knob: not "the output
    // changed" but "the null landed at the frequency the shipped notch law
    // predicts for the value injected". A node that wired `center_hz` to any
    // other parameter fails this; a difference test would not.
    for (int stages : {4, 8}) {
        // The probe tone must sit on a block DFT bin, because the capture
        // repeats one input block and an off-bin tone would step in phase at
        // every boundary. So the NOTCH target is chosen from the bin grid and
        // the centre is solved for — the lowest bin whose required centre lands
        // inside the registered range, which differs per stage count because a
        // longer cascade puts its first notch proportionally lower.
        double notch_hz = 0.0;
        double center = 0.0;
        for (int bin = 1; bin <= 32; ++bin) {
            const double candidate = bin * kBinHz;
            const double required = center_for_notch(candidate, stages);
            if (required > phaser_ns::kCenterMinHz * 1.05 &&
                required < phaser_ns::kCenterMaxHz * 0.95) {
                notch_hz = candidate;
                center = required;
                break;
            }
        }
        REQUIRE(notch_hz > 0.0);
        REQUIRE(on_bin(notch_hz));

        const auto probe = tone_at(notch_hz);
        const auto measure = [&](float centre_hz, float mix_percent) {
            auto fx = Stereo(phaser_ns::make_phaser_node(stages), kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            // Depth 0 parks the sweep at the centre, which is what makes the
            // notch stand still long enough to be located.
            REQUIRE(inj.inject(immediate(phaser_ns::kDepth, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kFeedback, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kMix, mix_percent)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kCenterHz, centre_hz)) == InjectStatus::Ok);
            fx.settle({probe, probe}, 96);
            return trace_magnitude_at(capture(fx, {probe, probe}, 32), notch_hz);
        };

        // The control is the SAME tone through the same node at mix 0, so the
        // comparison needs no passband frequency to be found and no assumption
        // about where the cascade happens to be flat.
        const double dry = measure(static_cast<float>(center), 0.0f);
        const double on_notch = measure(static_cast<float>(center), 50.0f);
        INFO("stages=" << stages << " centre=" << center << " notch=" << notch_hz
                       << " dry=" << dry << " notched=" << on_notch);
        REQUIRE_THAT(dry, WithinRel(static_cast<double>(kAmplitude), 0.02));
        REQUIRE(on_notch < 0.1 * dry);

        // And the null follows the CENTRE rather than being a fixed hole in the
        // node: moved away, the same tone passes.
        const double moved = measure(static_cast<float>(center * 0.5), 50.0f);
        REQUIRE(moved > 5.0 * on_notch);
    }
}

TEST_CASE("Forge modulation: the phaser mix and spread params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    const auto t = tone();

    // Mix 0 is a wire — bit-exact, so this cannot pass on a mis-wired knob.
    {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 0.0f)) == InjectStatus::Ok);
        const auto out = fx.settle({t, t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    // Spread 0 puts both channels' LFOs on one phase, so the two rails are
    // bit-identical; quadrature makes them differ. Also bit-exact in the zero
    // direction, which is what makes it a test of the spread rather than of
    // some incidental decorrelation.
    {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kStereoSpread, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 50.0f)) == InjectStatus::Ok);
        const auto mono = fx.settle({t, t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(mono[0][static_cast<std::size_t>(k)] == mono[1][static_cast<std::size_t>(k)]);

        REQUIRE(inj.inject(immediate(phaser_ns::kStereoSpread, 0.25f)) == InjectStatus::Ok);
        fx.settle({t, t}, 96);
        const auto wide = capture_pair(fx, {t, t}, 128);
        double side = 0.0;
        for (std::size_t k = 0; k < wide.first.size(); ++k)
            side = std::max(side, std::abs(static_cast<double>(wide.first[k]) -
                                           static_cast<double>(wide.second[k])));
        REQUIRE(side > 0.01);
    }
}

TEST_CASE("Forge modulation: the phaser depth, feedback, rate, wave and stagger all move the audio",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    const auto t = tone();
    // A baseline every variant is compared against, so each assertion isolates
    // one knob.
    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 50.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kCenterHz, 1200.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kRateHz, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kDepth, 60.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(id, value)) == InjectStatus::Ok);
        fx.settle({t, t}, 96);
        return capture(fx, {t, t}, 128);
    };
    const auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k)
            d = std::max(d, std::abs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
        return d;
    };

    const auto baseline = render_with(phaser_ns::kDepth, 60.0f);
    REQUIRE(difference(baseline, render_with(phaser_ns::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kFeedback,
                                             static_cast<float>(
                                                 phaser_ns::Engine::kColorOnFeedback))) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kRateHz, 5.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kWave, phaser_ns::kWaveSquare)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kStaggerRatio,
                                             static_cast<float>(
                                                 phaser_ns::Engine::kStaggerMax))) > 0.01);
}

TEST_CASE("Forge modulation: the stepped wave param rounds to the nearest shape",
          "[host][baked][forge][forge-modulation][phaser]") {
    using pulp::signal::LfoWave;
    REQUIRE(phaser_ns::wave_from_param(0.0f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(0.49f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(0.51f) == LfoWave::triangle);
    REQUIRE(phaser_ns::wave_from_param(4.0f) == LfoWave::square);
    REQUIRE(phaser_ns::wave_from_param(6.0f) == LfoWave::smooth_random);
    REQUIRE(phaser_ns::wave_from_param(-3.0f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(42.0f) == LfoWave::sine);
}

TEST_CASE("Forge modulation: the phaser registry gain is the DSP's loop bound",
          "[host][baked][forge][forge-modulation][phaser]") {
    // Series law 8. This lineage HAS a feedback path, so the bound is a loop
    // envelope rather than a constructive sum, and the DSP publishes it as a
    // constexpr its own suite asserts against.
    using Engine = phaser_ns::Engine;
    REQUIRE_THAT(static_cast<double>(phaser_ns::phaser_worst_case_gain()),
                 WithinRel(1.0 / (1.0 - Engine::kFeedbackMax), 1e-9));
    REQUIRE_THAT(static_cast<double>(phaser_ns::phaser_worst_case_gain()),
                 WithinRel(Engine::worst_case_gain(), 1e-12));
}

TEST_CASE("Forge modulation: the phaser node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][phaser][rt-safety]") {
    auto fx = Stereo(phaser_ns::make_phaser_node(12), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(phaser_ns::kRateHz, 0.02f + 0.3f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kDepth, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kCenterHz, 100.0f + 150.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kFeedback, -0.9f + 0.056f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kMix, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kStereoSpread, 0.015f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kStaggerRatio, 0.85f + 0.009f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kWave, static_cast<float>(b % 7)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  Vibrato — three lineages, three nodes
// ═══════════════════════════════════════════════════════════════════════════

namespace vib = pulp::host::modulation::vibrato;
namespace vib_delay = pulp::host::modulation::vibrato::delay_line;
namespace vib_phase = pulp::host::modulation::vibrato::phase;
namespace vib_univibe = pulp::host::modulation::vibrato::univibe;

TEST_CASE("Forge modulation: the delay vibrato shifts pitch by the cents it was given",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The one engine here that really moves pitch, so `depth_cents` has a
    // closed-form consequence: a peak fractional shift of `2^(cents/1200) − 1`.
    // Measured out of the rendered audio by complex demodulation, and compared
    // against that formula rather than against another render.
    const auto measured_ratio = [](float cents, float rate_hz) {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_delay::kRateHz, rate_hz)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, cents)) == InjectStatus::Ok);
        const auto t = tone();
        fx.settle({t}, 64);
        const auto trace = capture(fx, {t}, 512);  // ~1.4 s
        const auto d = demodulate(trace, kToneHz, 200.0, 48);
        return peak_deviation(d.freq_hz, kToneHz) / kToneHz;
    };

    for (float cents : {25.0f, 50.0f}) {
        const double expected = std::exp2(static_cast<double>(cents) / 1200.0) - 1.0;
        const double got = measured_ratio(cents, 6.0f);
        INFO("cents=" << cents << " expected=" << expected << " measured=" << got);
        REQUIRE_THAT(got, WithinRel(expected, 0.10));
    }

    // Zero depth is a static tap: no pitch movement at all.
    REQUIRE(measured_ratio(0.0f, 6.0f) < 1e-4);
}

TEST_CASE("Forge modulation: the delay vibrato rate param sets the measured rate",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The modulator here is a clean sine on the delay, so unlike the chorus the
    // instantaneous-frequency trace carries the LFO's own fundamental and can be
    // compared against the injected rate directly.
    for (double rate : {5.0, 9.0}) {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_delay::kRateHz, static_cast<float>(rate))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, 50.0f)) == InjectStatus::Ok);
        const auto t = tone();
        fx.settle({t}, 64);
        const auto trace = capture(fx, {t}, 512);
        const auto d = demodulate(trace, kToneHz, 200.0, 48);
        const double measured = locate_rate(d.freq_hz, rate * 0.5, rate * 1.5, d.rate_hz, 3000);
        REQUIRE_THAT(measured, WithinRel(rate, 0.03));
    }
}

TEST_CASE("Forge modulation: the delay vibrato's onset controls do not stall the modulation",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // A REGRESSION TEST for the trap the node exists to avoid. Both onset
    // setters re-arm the lifecycle envelope and zero its depth scale, so a node
    // that wrote them on every sample — which is what the rest of this family's
    // params do — would hold that envelope at zero forever. The node would then
    // pass audio with NO VIBRATO while every parameter read back exactly the
    // value that was set, and the only symptom would be an effect that seemed
    // not to work. Deleting the change detection in the node makes this fail.
    auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(vib_delay::kRateHz, 6.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, 50.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(vib_delay::kFadeInMs, 300.0f)) == InjectStatus::Ok);

    const auto t = tone();
    const auto trace = capture(fx, {t}, 750);  // ~2 s, well past the 300 ms fade
    const auto d = demodulate(trace, kToneHz, 200.0, 48);

    // The early window starts at 30 ms, not at zero. The demodulator's own
    // four-pole low-pass is still charging for the first few milliseconds and
    // its startup transient reads as a larger frequency excursion than anything
    // the vibrato produces — measured from sample zero it reports 115 Hz of
    // "deviation" against a full-depth 88, which would look like the fade
    // running backwards.
    const double trace_rate = d.rate_hz;
    const auto early_begin = static_cast<std::size_t>(0.030 * trace_rate);
    const auto early_end = static_cast<std::size_t>(0.120 * trace_rate);
    REQUIRE(early_end < d.freq_hz.size());
    double early = 0.0;
    for (std::size_t i = early_begin; i < early_end; ++i)
        early = std::max(early, std::abs(d.freq_hz[i] - kToneHz));
    double late = 0.0;
    for (std::size_t i = d.freq_hz.size() / 2; i < d.freq_hz.size(); ++i)
        late = std::max(late, std::abs(d.freq_hz[i] - kToneHz));

    const double expected = (std::exp2(50.0 / 1200.0) - 1.0) * kToneHz;
    INFO("early=" << early << " late=" << late << " expected full depth=" << expected);
    REQUIRE(late > 0.8 * expected);  // the envelope DID open — the trap's assertion
    REQUIRE(early < 0.5 * late);     // ...and it opened gradually, so the fade ran
}

TEST_CASE("Forge modulation: delay vibrato does not claim fixed PDC latency",
          "[host][baked][forge][forge-modulation][vibrato]") {
    // Rate and depth move the tap continuously, so a worst-case upper bound is
    // not an exact intrinsic latency and must not feed graph PDC.
    const auto type = vib_delay::make_delay_vibrato_node(4.0f);
    REQUIRE_FALSE(type.latency_samples);

    // The floor still changes the registered parameter contract and therefore
    // remains a stable realization identity.
    REQUIRE(vib_delay::make_delay_vibrato_node(4.0f).type_id !=
            vib_delay::make_delay_vibrato_node(8.0f).type_id);

    for (const auto& p : type.baked_params)
        if (p.id == vib_delay::kRateHz) REQUIRE_THAT(p.min_value, WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("Forge modulation: the phase vibrato's params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    const auto t = tone();

    // Mix 0 is a wire, bit-exact.
    {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 0.0f)) == InjectStatus::Ok);
        const auto out = fx.settle({t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 100.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kRateHz, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kDepth, 60.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kCenterHz, 500.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(id, value)) == InjectStatus::Ok);
        fx.settle({t}, 96);
        return capture(fx, {t}, 128);
    };
    const auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k)
            d = std::max(d, std::abs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
        return d;
    };

    const auto baseline = render_with(vib_phase::kDepth, 60.0f);
    REQUIRE(difference(baseline, render_with(vib_phase::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kCenterHz, 1800.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kRateHz, 6.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kMix, 50.0f)) > 0.01);
}

TEST_CASE("Forge modulation: the phase vibrato stage counts are distinct registrations",
          "[host][baked][forge][forge-modulation][vibrato]") {
    using Engine = vib_phase::Engine;
    std::vector<std::string> ids;
    for (int stages = 1; stages <= Engine::kMaxStages; ++stages)
        ids.push_back(vib_phase::make_phase_vibrato_node(stages).type_id);
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Different cascade lengths are audibly different, which is what makes the
    // realization axis a real one rather than a naming convention.
    const auto t = tone();
    const auto render = [&t](int stages) {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(stages), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 100.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kDepth, 80.0f)) == InjectStatus::Ok);
        fx.settle({t}, 96);
        return capture(fx, {t}, 64);
    };
    const auto two = render(2);
    const auto four = render(4);
    double d = 0.0;
    for (std::size_t k = 0; k < two.size(); ++k)
        d = std::max(d, std::abs(static_cast<double>(two[k]) - static_cast<double>(four[k])));
    REQUIRE(d > 0.01);
}

TEST_CASE("Forge modulation: the Univibe's stepped mode param reaches the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The decisive test for this knob, and it is available only because the DSP
    // guarantees something exact: in the chorus position the LEFT output is the
    // untouched input. So the stepped param is checked bit-for-bit rather than
    // by a level difference.
    const auto t = tone();
    auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(vib_univibe::kMode, vib_univibe::kModeChorus)) ==
            InjectStatus::Ok);
    const auto chorus_out = fx.settle({t, t}, 96);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE(chorus_out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    // ...while the right rail is genuinely phase-shifted.
    double side = 0.0;
    for (int k = 0; k < kFrames; ++k)
        side = std::max(side, std::abs(static_cast<double>(
                                  chorus_out[0][static_cast<std::size_t>(k)] -
                                  chorus_out[1][static_cast<std::size_t>(k)])));
    REQUIRE(side > 0.01);

    // In the vibrato position both rails carry the same wet signal, and neither
    // is the input.
    REQUIRE(inj.inject(immediate(vib_univibe::kMode, vib_univibe::kModeVibrato)) ==
            InjectStatus::Ok);
    const auto vibrato_out = fx.settle({t, t}, 96);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE(vibrato_out[0][static_cast<std::size_t>(k)] ==
                vibrato_out[1][static_cast<std::size_t>(k)]);
    double moved = 0.0;
    for (int k = 0; k < kFrames; ++k)
        moved = std::max(moved, std::abs(static_cast<double>(
                                    vibrato_out[0][static_cast<std::size_t>(k)] -
                                    t[static_cast<std::size_t>(k)])));
    REQUIRE(moved > 0.01);

    REQUIRE(vib_univibe::mode_from_param(0.0f) == vib_univibe::Mode::vibrato);
    REQUIRE(vib_univibe::mode_from_param(0.49f) == vib_univibe::Mode::vibrato);
    REQUIRE(vib_univibe::mode_from_param(0.51f) == vib_univibe::Mode::chorus);
    REQUIRE(vib_univibe::mode_from_param(7.0f) == vib_univibe::Mode::chorus);
}

TEST_CASE("Forge modulation: the Univibe's rate and depth params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    const auto t = tone();
    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_univibe::kRateHz, 3.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_univibe::kDepth, 70.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(id, value)) == InjectStatus::Ok);
        fx.settle({t, t}, 96);
        return capture(fx, {t, t}, 128);
    };
    const auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k)
            d = std::max(d, std::abs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
        return d;
    };
    const auto baseline = render_with(vib_univibe::kDepth, 70.0f);
    REQUIRE(difference(baseline, render_with(vib_univibe::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_univibe::kRateHz, 8.0f)) > 0.01);
}

TEST_CASE("Forge modulation: the vibrato registry gains cite the bounds their suite asserts",
          "[host][baked][forge][forge-modulation][vibrato]") {
    // Series law 8, and the place in this family where the obvious number is the
    // wrong one.
    //
    // The delay engine is one unit-gain tap and still is not 0 dB, because the
    // tap is read through the Lagrange kernel whose L1 norm is 1.25.
    REQUIRE_THAT(static_cast<double>(vib_delay::delay_vibrato_worst_case_gain()),
                 WithinRel(static_cast<double>(vib_delay::Engine::kInterpolatorPeakGain), 1e-9));
    REQUIRE(vib_delay::delay_vibrato_worst_case_gain() > 1.0f);

    // The two PHASE engines are allpass cascades, and an allpass is unity
    // MAGNITUDE — which bounds steady-state sinusoids and says nothing about
    // sample gain. Its impulse response changes sign, so a sign-matched bounded
    // input accumulates to the cascade's L1 norm, and the DSP suite measures
    // both engines above a factor of two. Citing the sinusoidal bound of 1 would
    // put a number in the registry that the DSP's own suite disproves, so these
    // assertions pin the L1 and pin it ABOVE the sinusoidal bound — a later
    // "simplification" back to 1.0 fails here.
    for (int stages : {2, 4}) {
        const double lowest = vib_phase::Engine::kMinCenterHz *
                              std::exp2(-vib_phase::Engine::kSweepOctaves);
        const double reference = vib::allpass_cascade_l1(
            std::vector<double>(static_cast<std::size_t>(stages), lowest), kSr);
        // Compared at float precision: the node returns a float for the
        // registry, so a double-precision tolerance would be asserting that a
        // float can hold sixteen digits.
        REQUIRE_THAT(static_cast<double>(vib_phase::phase_vibrato_worst_case_gain(stages, kSr)),
                     WithinRel(reference, 1e-6));
        REQUIRE(vib_phase::phase_vibrato_worst_case_gain(stages, kSr) >
                static_cast<float>(vib_phase::Engine::kSinusoidalGainBound));
    }
    REQUIRE(vib_univibe::univibe_worst_case_gain(kSr) >
            static_cast<float>(vib_univibe::Engine::kSinusoidalGainBound));
    REQUIRE(vib_univibe::univibe_worst_case_gain(kSr) > 2.0f);

    // A longer cascade cannot have a smaller worst case, which is the sanity
    // check that the number tracks the realization rather than being a
    // stage-count-blind constant.
    REQUIRE(vib_phase::phase_vibrato_worst_case_gain(4, kSr) >
            vib_phase::phase_vibrato_worst_case_gain(2, kSr));
}

TEST_CASE("Forge modulation: the vibrato nodes' process paths allocate nothing",
          "[host][baked][forge][forge-modulation][vibrato][rt-safety]") {
    const auto t = tone();
    {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t}, 8);
        pulp::test::ReusableRenderer<1> renderer(fx, {t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_delay::kRateHz, 4.0f + 0.5f * static_cast<float>(b)));
            inj.inject(immediate(vib_delay::kDepthCents, 3.0f * static_cast<float>(b)));
            // Including the onset controls, whose change-detected path must be
            // allocation-free on both the taken and the untaken branch.
            inj.inject(immediate(vib_delay::kDelayMs, 10.0f * static_cast<float>(b % 4)));
            inj.inject(immediate(vib_delay::kFadeInMs, 20.0f * static_cast<float>(b % 3)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
    {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(4), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t}, 8);
        pulp::test::ReusableRenderer<1> renderer(fx, {t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_phase::kRateHz, 0.05f + 0.3f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kDepth, 3.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kCenterHz, 200.0f + 55.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kMix, 3.0f * static_cast<float>(b)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
    {
        auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t, t}, 8);
        pulp::test::ReusableRenderer<2> renderer(fx, {t, t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_univibe::kRateHz, 0.3f + 0.24f * static_cast<float>(b)));
            inj.inject(immediate(vib_univibe::kDepth, 3.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_univibe::kMode, static_cast<float>(b % 2)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge modulation: every node in the family has a distinct type id",
          "[host][baked][forge][forge-modulation]") {
    // One registry, one namespace of ids. Two nodes sharing an id would load a
    // session into the wrong effect, and it is exactly the kind of thing that
    // only shows up once a fifth lineage is added.
    std::vector<std::string> ids{mod::kSsbFrequencyShifterTypeId,
                                 vib_univibe::kTypeId};
    using Voicing = chorus_ns::Voicing;
    using JunoMode = chorus_ns::JunoMode;
    for (auto v : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                   Voicing::tri_chorus})
        for (auto m : {JunoMode::mode_I, JunoMode::mode_II, JunoMode::mode_I_plus_II})
            for (bool bbd : {false, true}) {
                if (v != Voicing::juno_ensemble && m != JunoMode::mode_I) continue;
                ids.push_back(chorus_ns::make_chorus_node(v, m, bbd).type_id);
            }
    for (int stages : {4, 6, 8, 10, 12}) ids.push_back(phaser_ns::make_phaser_node(stages).type_id);
    for (float floor_hz : {4.0f, 8.0f})
        ids.push_back(vib_delay::make_delay_vibrato_node(floor_hz).type_id);
    for (int stages = 1; stages <= vib_phase::Engine::kMaxStages; ++stages)
        ids.push_back(vib_phase::make_phase_vibrato_node(stages).type_id);

    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    // Every one of them is namespaced to the family, so a future dynamics or
    // reverb id cannot collide with these either.
    for (const auto& id : ids) REQUIRE(id.rfind("modulation.", 0) == 0);
}

TEST_CASE("Forge modulation: flanger, Leslie and scanner factories expose canonical contracts",
          "[host][baked][forge][forge-modulation][catalog]") {
    auto flanger = mod::flanger::make_flanger_node();
    auto leslie = mod::leslie::make_leslie_node();
    auto scanner = mod::leslie::make_scanner_vibrato_node();
    REQUIRE(flanger.type_id == mod::flanger::kTypeId);
    REQUIRE(flanger.baked_params.size() == 9);
    REQUIRE(flanger.num_input_ports == 2);
    REQUIRE(flanger.num_output_ports == 2);
    REQUIRE(leslie.type_id == mod::leslie::kTypeId);
    REQUIRE(leslie.baked_params.size() == 24);
    REQUIRE(leslie.num_input_ports == 2);
    REQUIRE(leslie.num_output_ports == 2);
    REQUIRE(scanner.type_id == mod::leslie::kScannerTypeId);
    REQUIRE(scanner.baked_params.size() == 7);
    REQUIRE(scanner.num_input_ports == 1);
    REQUIRE(scanner.num_output_ports == 1);
    REQUIRE(mod::flanger::worst_case_gain() ==
            static_cast<float>(pulp::signal::Flanger::worst_case_gain()));
    REQUIRE(mod::leslie::leslie_worst_case_gain() ==
            static_cast<float>(pulp::signal::LeslieRotary::kWorstCaseGain));
    REQUIRE(mod::leslie::scanner_worst_case_gain() ==
            static_cast<float>(pulp::signal::ScannerVibrato::kWorstCaseGain));

    Fixture flanger_fx(std::move(flanger), kSr, kFrames);
    const auto block = tone();
    auto out = flanger_fx.render({block, block});
    for (const auto& channel : out)
        for (float x : channel) REQUIRE(std::isfinite(x));
    Fixture leslie_fx(std::move(leslie), kSr, kFrames);
    out = leslie_fx.render({block, block});
    for (const auto& channel : out)
        for (float x : channel) REQUIRE(std::isfinite(x));
    pulp::test::BakedNodeFixture<1> scanner_fx(std::move(scanner), kSr, kFrames);
    const auto mono = scanner_fx.render({block});
    for (float x : mono[0]) REQUIRE(std::isfinite(x));
}

TEST_CASE("Forge modulation: flanger latency controls are frozen realizations",
          "[host][baked][forge][forge-modulation][flanger][latency]") {
    using Mode = pulp::signal::FlangerMode;
    const auto classic = mod::flanger::make_flanger_node(Mode::classic, 4.0);
    const auto through_zero = mod::flanger::make_flanger_node(Mode::through_zero, 4.0);
    REQUIRE(classic.type_id != through_zero.type_id);
    REQUIRE(mod::flanger::latency_samples(Mode::classic, 4.0, kSr) == 0);
    REQUIRE(mod::flanger::latency_samples(Mode::through_zero, 4.0, kSr) == 192);
    REQUIRE(classic.latency_samples(kSr) == 0);
    REQUIRE(through_zero.latency_samples(kSr) == 192);

    for (const auto& row : classic.baked_params) {
        REQUIRE(row.id != mod::flanger::kMode);
        REQUIRE(row.id != mod::flanger::kOffset);
    }
}
