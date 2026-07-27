#pragma once

// Catalog nodes for the SPACE family — convolution reverb and nonlin ambience.
//
// THE BAR every case here is held to, unchanged from the dynamics family: a
// baked param has to be shown MOVING THE BAKED NODE'S AUDIO over the real
// production path — bake → `claim_param_injection` → `ParamInjector` → routed
// executor. A param declared in the table but never read by
// `process_instance_baked_param` looks exactly like a working one until someone
// automates it, so every declared param on both nodes is exercised below and
// every assertion is directional (this knob makes THIS happen), not "the output
// changed".
//
// Two things are specific to these two nodes and get more than the usual
// attention:
//
// **The convolver's zero latency has to survive the graph.** Its DSP suite
// asserts `latency_samples() == 0` four ways, and none of that helps if the
// node wrapper quietly buffers a block. A wrapper that did would still pass
// every frequency, every level, and every mix assertion in this file. So the
// property is asserted at the node level three ways: the baked processor
// reports zero; a baked impulse comes out at sample 0 and not at sample 128;
// and the node's whole output is compared SAMPLE-FOR-SAMPLE against the bare
// engine driven with the same IR, which is the form that cannot be satisfied by
// anything that adds delay, gain, or filtering on the way through.
//
// **The ambience's four programs are the product.** Asserting that switching
// programs changes the output would pass against a node that just perturbed a
// coefficient. What is asserted instead is that each program's rendered
// IMPULSE-RESPONSE ENVELOPE has the SHAPE that program names — flat-then-cut,
// rising-then-cut, falling, humped — measured from the baked node's audio.
// Those shapes are also mutually exclusive, so a program that silently failed
// to switch would fail three of the four.
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// Impulse responses are rendered by pushing a unit impulse in the first block
// and silence thereafter, concatenating the blocks. Envelopes are rectangular-
// window RMS: an unweighted sum of the energy inside the window, so the
// expected value stays a closed form rather than acquiring a window correction
// that a tolerance would then have to hide.
//
// Both nodes are TRUE STEREO, so the fixture is `BakedNodeFixture<2>` and the
// `ReusableRenderer<2>` counterpart drives the allocation probe — the
// convenience `render()` allocates its own output vectors and is documented as
// unsuitable for that path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_space_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pulp::host;
namespace sp = pulp::host::space;
namespace conv = pulp::host::space::convolution;
namespace amb = pulp::host::space::nonlin_ambience;
namespace cabinet = pulp::host::space::cabinet;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

using Fixture = pulp::test::BakedNodeFixture<2>;

/// A stereo block of silence.
std::vector<std::vector<float>> silence(int frames = kFrames) {
    return {std::vector<float>(static_cast<std::size_t>(frames), 0.0f),
            std::vector<float>(static_cast<std::size_t>(frames), 0.0f)};
}

/// A stereo block holding a unit impulse at sample 0 on both channels.
std::vector<std::vector<float>> impulse_block(float amplitude = 1.0f, int frames = kFrames) {
    auto b = silence(frames);
    b[0][0] = amplitude;
    b[1][0] = amplitude;
    return b;
}

/// Pushes an impulse and then silence, concatenating `blocks` blocks of output.
/// The result is the node's stereo impulse response as the host would hear it.
struct Ir {
    std::vector<float> left, right;
};

Ir render_impulse_response(Fixture& fx, int blocks, float amplitude = 1.0f) {
    Ir ir;
    const auto first = fx.render(impulse_block(amplitude));
    ir.left = first[0];
    ir.right = first[1];
    const auto quiet = silence();
    for (int b = 1; b < blocks; ++b) {
        const auto out = fx.render(quiet);
        ir.left.insert(ir.left.end(), out[0].begin(), out[0].end());
        ir.right.insert(ir.right.end(), out[1].begin(), out[1].end());
    }
    return ir;
}

/// Renders `blocks` blocks of silence and discards them — used to let a
/// program-swap crossfade finish before anything is measured.
void settle_silent(Fixture& fx, int blocks) {
    const auto quiet = silence();
    for (int b = 0; b < blocks; ++b) fx.render(quiet);
}

double window_power(const std::vector<float>& x, int start, int length) {
    double sum = 0.0;
    const int end = std::min(start + length, static_cast<int>(x.size()));
    for (int i = std::max(0, start); i < end; ++i) {
        const double v = x[static_cast<std::size_t>(i)];
        sum += v * v;
    }
    return sum / static_cast<double>(std::max(1, end - std::max(0, start)));
}

double to_db(double power) { return 10.0 * std::log10(std::max(power, 1e-300)); }

double peak(const std::vector<float>& x) {
    double p = 0.0;
    for (float v : x) p = std::max(p, static_cast<double>(std::fabs(v)));
    return p;
}

int first_nonzero(const std::vector<float>& x, double threshold = 1e-6) {
    for (std::size_t n = 0; n < x.size(); ++n)
        if (std::fabs(static_cast<double>(x[n])) > threshold) return static_cast<int>(n);
    return -1;
}

/// A short-time RMS envelope in dB, one entry per hop.
std::vector<double> envelope_db(const std::vector<float>& x, int window, int hop) {
    std::vector<double> env;
    for (int start = 0; start + window <= static_cast<int>(x.size()); start += hop)
        env.push_back(to_db(window_power(x, start, window)));
    return env;
}

/// The high-frequency energy fraction — a one-zero difference against the total.
/// Enough to say "this got darker" without a full spectrum.
double hf_fraction(const std::vector<float>& x, int start, int count) {
    double hf = 0.0, total = 0.0;
    const int end = std::min(start + count, static_cast<int>(x.size()));
    for (int n = std::max(1, start); n < end; ++n) {
        const double d = static_cast<double>(x[static_cast<std::size_t>(n)]) -
                         static_cast<double>(x[static_cast<std::size_t>(n - 1)]);
        hf += d * d;
        total += static_cast<double>(x[static_cast<std::size_t>(n)]) *
                 static_cast<double>(x[static_cast<std::size_t>(n)]);
    }
    return hf / std::max(total, 1e-30);
}

/// Counts samples above the local RMS — the echo-density probe. A sparse tap
/// cloud's only such samples are its taps, so this tracks tap density.
int active_samples(const std::vector<float>& x, int start, int count) {
    const double sigma = std::sqrt(window_power(x, start, count));
    int above = 0;
    const int end = std::min(start + count, static_cast<int>(x.size()));
    for (int n = std::max(0, start); n < end; ++n)
        if (std::fabs(static_cast<double>(x[static_cast<std::size_t>(n)])) > sigma) ++above;
    return above;
}

// ── Impulse responses for the convolver ──────────────────────────────────

/// A single-sample IR. The convolution of anything with it is that thing, which
/// is what makes it the right probe for latency and for pre-delay: any shift the
/// node introduces is visible directly as a shift in the output.
conv::ImpulseResponse delta_ir(int channels = 2, int length = 2048) {
    conv::ImpulseResponse ir;
    ir.channels.assign(static_cast<std::size_t>(channels),
                       std::vector<float>(static_cast<std::size_t>(length), 0.0f));
    for (auto& c : ir.channels) c[0] = 1.0f;
    ir.sample_rate = kSr;
    return ir;
}

/// A deterministic decaying-noise IR — a stand-in for a measured room, with
/// enough length to exercise several partition groups.
conv::ImpulseResponse room_ir(int channels = 2, int length = 8192, std::uint32_t seed = 0x51ED) {
    conv::ImpulseResponse ir;
    ir.channels.assign(static_cast<std::size_t>(channels),
                       std::vector<float>(static_cast<std::size_t>(length), 0.0f));
    for (int c = 0; c < channels; ++c) {
        pulp::signal::Xorshift32 rng(seed + static_cast<std::uint32_t>(c) * 0x9E3779B9u);
        for (int n = 0; n < length; ++n) {
            const double decay = std::exp(-5.0 * n / static_cast<double>(length));
            ir.channels[static_cast<std::size_t>(c)][static_cast<std::size_t>(n)] =
                static_cast<float>(rng.next_bipolar<double>() * decay);
        }
    }
    ir.sample_rate = kSr;
    return ir;
}

/// Injects the convolver's whole param table at known values, so a case that
/// varies one knob is varying exactly one thing.
void inject_convolution_defaults(ParamInjector& inj, float wet = 100.0f, float dry = 0.0f) {
    REQUIRE(inj.inject(immediate(conv::kIrGainDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kPredelayMs, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWetPercent, wet)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kDryPercent, dry)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWidthPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kLowcutHz, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kHighcutHz, 20000.0f)) == InjectStatus::Ok);
}

/// Injects the ambience's whole param table at its documented defaults.
void inject_ambience_defaults(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(amb::kProgram, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kLengthMs, 350.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kPredelayMs, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kDensityPct, 60.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kDensityGrowth, 2.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kGateHoldPct, 70.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kAttackPct, 85.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.7f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kTone, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kHfDampHz, 6000.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kWidthPct, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kConverterAmount, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kOutputGainDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kMixPct, 100.0f)) == InjectStatus::Ok);
}

/// Renders the ambience's impulse response at one program, after letting the
/// program-swap crossfade finish. The fixture must be fresh, so no previous
/// program's tail is still ringing when the impulse goes in.
Ir render_program_ir(Fixture& fx, ParamInjector& inj, int program, double length_ms,
                     int blocks) {
    inject_ambience_defaults(inj);
    REQUIRE(inj.inject(immediate(amb::kProgram, static_cast<float>(program))) ==
            InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(amb::kLengthMs, static_cast<float>(length_ms))) ==
            InjectStatus::Ok);
    // The swap crossfade is 20 ms; 32 blocks of 128 is 85 ms, and with a silent
    // input there is no tail to outlast.
    settle_silent(fx, 32);
    return render_impulse_response(fx, blocks);
}

}  // namespace

// ══ Convolution reverb ════════════════════════════════════════════════════










// ══ Nonlin ambience ═══════════════════════════════════════════════════════
