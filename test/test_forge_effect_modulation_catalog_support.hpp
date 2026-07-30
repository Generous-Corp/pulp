#pragma once

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
#include <numbers>
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
    const double w = 2.0 * std::numbers::pi * hz / kSr;
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
    const double pole = std::exp(-2.0 * std::numbers::pi * lowpass_hz / kSr);
    double si[4] = {0, 0, 0, 0};
    double sq[4] = {0, 0, 0, 0};
    double previous = 0.0;
    bool have = false;
    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = 2.0 * std::numbers::pi * carrier_hz * static_cast<double>(n) / kSr;
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
            while (d > std::numbers::pi)
                d -= 2.0 * std::numbers::pi;
            while (d < -std::numbers::pi)
                d += 2.0 * std::numbers::pi;
            out.freq_hz.push_back(carrier_hz + d * out.rate_hz / (2.0 * std::numbers::pi));
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
            const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / rate_hz;
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
        const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / kSr;
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








// ═══════════════════════════════════════════════════════════════════════════
//  Vibrato — three lineages, three nodes
// ═══════════════════════════════════════════════════════════════════════════

namespace vib = pulp::host::modulation::vibrato;
namespace vib_delay = pulp::host::modulation::vibrato::delay_line;
namespace vib_phase = pulp::host::modulation::vibrato::phase;
namespace vib_univibe = pulp::host::modulation::vibrato::univibe;
