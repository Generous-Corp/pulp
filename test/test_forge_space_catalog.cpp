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

TEST_CASE("Forge space convolution: the node bakes and runs true stereo",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());
    REQUIRE(std::string(type.type_id) == "space.convolution_reverb");
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 7);

    // Every declared range is the DSP's canonical contract, not a second
    // opinion about it.
    using E = conv::Engine;
    auto param = [&](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return p;
        FAIL("param not declared");
        return type.baked_params.front();
    };
    REQUIRE_THAT(param(conv::kIrGainDb).min_value, WithinAbs(E::kIrGainDbMin, 1e-6));
    REQUIRE_THAT(param(conv::kIrGainDb).max_value, WithinAbs(E::kIrGainDbMax, 1e-6));
    REQUIRE_THAT(param(conv::kPredelayMs).max_value, WithinAbs(E::kPredelayMsMax, 1e-6));
    REQUIRE_THAT(param(conv::kWidthPercent).max_value, WithinAbs(E::kWidthPercentMax, 1e-6));
    REQUIRE_THAT(param(conv::kLowcutHz).min_value, WithinAbs(E::kLowcutHzMin, 1e-6));
    REQUIRE_THAT(param(conv::kHighcutHz).max_value, WithinAbs(E::kHighcutHzMax, 1e-6));

    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);
    const auto out = fx.settle(impulse_block());
    REQUIRE(std::isfinite(out[0][0]));
    REQUIRE(std::isfinite(out[1][0]));
}

TEST_CASE("Forge space convolution: zero latency survives the graph",
          "[host][baked][param-injection][forge][forge-space][convolution][latency]") {
    // The DSP promises a literal constant 0. A node wrapper that buffered one
    // block would still pass every level, spectrum and mix assertion in this
    // file, so the property is asserted here three separate ways.
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);

    // 1. The baked processor reports zero.
    REQUIRE(fx.baked().latency_samples() == 0);

    // 2. A baked impulse comes out at sample 0. With a single-sample IR the
    //    convolution IS the input, so any wrapper delay appears directly here —
    //    a one-block buffer would put the onset at 128.
    const Ir ir = render_impulse_response(fx, 8);
    REQUIRE(first_nonzero(ir.left) == 0);
    REQUIRE(first_nonzero(ir.right) == 0);
    REQUIRE(peak(ir.left) > 0.1);

    // 3. And the strongest form: the node reproduces the BARE ENGINE
    //    sample-for-sample. Nothing that adds delay, gain, or filtering on the
    //    way through can satisfy this.
    pulp::signal::ZeroLatencyConvolver bare;
    bare.prepare(kSr, kFrames, 2);
    bare.set_normalize_mode(conv::IrPolicy{}.normalize);
    bare.set_tail_trim_db(conv::IrPolicy{}.tail_trim_db);
    bare.set_tail_fade_ms(conv::IrPolicy{}.tail_fade_ms);
    bare.set_resample_taps_per_phase(conv::IrPolicy{}.resample_taps_per_phase);
    bare.set_true_stereo(conv::IrPolicy{}.true_stereo);
    const auto reference_ir = delta_ir();
    std::vector<const float*> ir_ptrs{reference_ir.channels[0].data(),
                                      reference_ir.channels[1].data()};
    REQUIRE(bare.load_impulse_response(ir_ptrs.data(), 2,
                                       static_cast<int>(reference_ir.channels[0].size()),
                                       reference_ir.sample_rate));
    bare.set_ir_gain_db(0.0);
    bare.set_predelay_ms(0.0);
    bare.set_wet_percent(100.0);
    bare.set_dry_percent(0.0);
    bare.set_width_percent(100.0);
    bare.set_lowcut_hz(20.0);
    bare.set_highcut_hz(20000.0);

    const int blocks = 8;
    std::vector<float> ref_left, ref_right;
    for (int b = 0; b < blocks; ++b) {
        std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
        std::vector<float> ol(kFrames, 0.0f), orr(kFrames, 0.0f);
        if (b == 0) {
            l[0] = 1.0f;
            r[0] = 1.0f;
        }
        const float* in_ptrs[2] = {l.data(), r.data()};
        float* out_ptrs[2] = {ol.data(), orr.data()};
        bare.process(in_ptrs, out_ptrs, kFrames);
        ref_left.insert(ref_left.end(), ol.begin(), ol.end());
        ref_right.insert(ref_right.end(), orr.begin(), orr.end());
    }
    REQUIRE(ref_left.size() == ir.left.size());
    int mismatches = 0, first_mismatch = -1;
    for (std::size_t n = 0; n < ref_left.size(); ++n)
        if (ir.left[n] != ref_left[n] || ir.right[n] != ref_right[n]) {
            ++mismatches;
            if (first_mismatch < 0) first_mismatch = static_cast<int>(n);
        }
    INFO(mismatches << " samples differ from the bare engine, first at " << first_mismatch);
    REQUIRE(mismatches == 0);
}

TEST_CASE("Forge space convolution: pre-delay shifts the wet path and not the report",
          "[host][baked][param-injection][forge][forge-space][convolution][latency]") {
    // The DSP is explicit that pre-delay is signal delay and not I/O latency.
    // With a single-sample IR the shift is exact and countable, which is the
    // only way to tell "the wet path moved" from "the node started buffering".
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    for (double predelay_ms : {0.0, 1.0, 5.0}) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kPredelayMs,
                                     static_cast<float>(predelay_ms))) == InjectStatus::Ok);
        const Ir ir = render_impulse_response(fx, 8);

        const int expected = static_cast<int>(std::lround(predelay_ms * kSr / 1000.0));
        INFO("pre-delay " << predelay_ms << " ms: onset at " << first_nonzero(ir.left)
                          << ", expected " << expected);
        REQUIRE(first_nonzero(ir.left) == expected);
        // The reported latency does not move with it — that is the distinction.
        REQUIRE(fx.baked().latency_samples() == 0);
    }
}

TEST_CASE("Forge space convolution: every gain param moves the audio the way it says",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());

    SECTION("ir_gain_db scales the wet path by exactly the dB injected") {
        double previous = 0.0;
        for (float db : {0.0f, 6.0f, 12.0f}) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_convolution_defaults(inj);
            REQUIRE(inj.inject(immediate(conv::kIrGainDb, db)) == InjectStatus::Ok);
            const Ir ir = render_impulse_response(fx, 24);
            const double p = peak(ir.left);
            if (previous > 0.0) {
                INFO("ir_gain " << db << " dB: peak ratio " << p / previous);
                REQUIRE_THAT(p / previous, WithinRel(std::pow(10.0, 6.0 / 20.0), 0.02));
            }
            previous = p;
        }
    }

    SECTION("wet and dry are separate paths") {
        // Dry only: the output IS the input, sample for sample. Wet only: the
        // input is gone and the convolution is what is left.
        Fixture dry_fx(type, kSr, kFrames);
        ParamInjector dry_inj = dry_fx.claim_injector();
        inject_convolution_defaults(dry_inj, /*wet=*/0.0f, /*dry=*/100.0f);
        const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
        const auto dry_out = dry_fx.settle({tone, tone});
        for (int n = 0; n < kFrames; ++n) {
            INFO("sample " << n);
            REQUIRE_THAT(dry_out[0][static_cast<std::size_t>(n)],
                         WithinAbs(tone[static_cast<std::size_t>(n)], 1e-6f));
        }

        Fixture wet_fx(type, kSr, kFrames);
        ParamInjector wet_inj = wet_fx.claim_injector();
        inject_convolution_defaults(wet_inj, /*wet=*/100.0f, /*dry=*/0.0f);
        const auto wet_out = wet_fx.settle({tone, tone});
        double difference = 0.0;
        for (int n = 0; n < kFrames; ++n)
            difference = std::max(difference,
                                  std::fabs(static_cast<double>(
                                      wet_out[0][static_cast<std::size_t>(n)] -
                                      tone[static_cast<std::size_t>(n)])));
        INFO("wet-vs-input largest difference " << difference);
        REQUIRE(difference > 0.05);
    }

    SECTION("width collapses and widens the wet return") {
        auto side_energy = [&](float width_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_convolution_defaults(inj);
            REQUIRE(inj.inject(immediate(conv::kWidthPercent, width_pct)) == InjectStatus::Ok);
            const Ir ir = render_impulse_response(fx, 24);
            double side = 0.0;
            for (std::size_t n = 0; n < ir.left.size(); ++n) {
                const double s = 0.5 * (ir.left[n] - ir.right[n]);
                side += s * s;
            }
            return side;
        };
        const double mono = side_energy(0.0f);
        const double normal = side_energy(100.0f);
        const double wide = side_energy(200.0f);
        INFO("side energy: mono " << mono << " normal " << normal << " wide " << wide);
        REQUIRE(mono < normal * 1e-9);   // 0 % is exactly mono
        REQUIRE(wide > normal * 3.0);    // 200 % doubles the side amplitude
        REQUIRE_THAT(wide / normal, WithinRel(4.0, 0.05));  // energy, so 2^2
    }
}

TEST_CASE("Forge space convolution: the send EQ corners filter the send",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(delta_ir());

    auto wet_hf = [&](float lowcut, float highcut) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kLowcutHz, lowcut)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(conv::kHighcutHz, highcut)) == InjectStatus::Ok);
        // A tone that holds whole cycles in the analysis block.
        const auto tone = pulp::test::sine_block(kFrames, 3000.0, kSr, 0.5f);
        const auto out = fx.settle({tone, tone});
        return pulp::test::harmonic_magnitude(out[0], 1, 3000.0, kSr);
    };
    auto wet_lf = [&](float lowcut, float highcut) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kLowcutHz, lowcut)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(conv::kHighcutHz, highcut)) == InjectStatus::Ok);
        const auto tone = pulp::test::sine_block(kFrames, 375.0, kSr, 0.5f);
        const auto out = fx.settle({tone, tone});
        return pulp::test::harmonic_magnitude(out[0], 1, 375.0, kSr);
    };

    // Both endpoints are documented as BYPASS, so the reference is the default
    // setting and each knob is measured as a departure from it.
    const double reference_hf = wet_hf(20.0f, 20000.0f);
    const double filtered_hf = wet_hf(20.0f, 1000.0f);
    INFO("3 kHz through the send: bypassed " << reference_hf << ", low-passed at 1 kHz "
                                             << filtered_hf);
    REQUIRE(filtered_hf < reference_hf * 0.7);

    const double reference_lf = wet_lf(20.0f, 20000.0f);
    const double filtered_lf = wet_lf(500.0f, 20000.0f);
    INFO("375 Hz through the send: bypassed " << reference_lf << ", high-passed at 500 Hz "
                                              << filtered_lf);
    REQUIRE(filtered_lf < reference_lf * 0.7);
}

TEST_CASE("Forge space convolution: params are block-rate, and that is the contract",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    // Not a limitation being papered over — the engine hoists its mix gains out
    // of its own sample loop, so sub-block injection could not reach the audio.
    // The node reads at offset 0 and this asserts that, so a future change that
    // moved to per-sample reads without changing the DSP would be caught rather
    // than quietly claiming a resolution it does not have.
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);
    fx.settle(silence(), 4);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
    // A dry-only reference block, so the level is exactly the input's.
    REQUIRE(inj.inject(immediate(conv::kWetPercent, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f)) == InjectStatus::Ok);
    const auto reference = fx.render({tone, tone});

    // Inject a halving of the dry level, HALFWAY through the next block.
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 50.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto during = fx.render({tone, tone});
    // Block rate: the whole block still carries the pre-injection value.
    for (int n = 0; n < kFrames; ++n) {
        INFO("sample " << n);
        REQUIRE_THAT(during[0][static_cast<std::size_t>(n)],
                     WithinAbs(reference[0][static_cast<std::size_t>(n)], 1e-6f));
    }
    // And the block after it carries the new one, in full.
    const auto after = fx.render({tone, tone});
    for (int n = 0; n < kFrames; ++n) {
        INFO("sample " << n);
        REQUIRE_THAT(after[0][static_cast<std::size_t>(n)],
                     WithinAbs(0.5f * reference[0][static_cast<std::size_t>(n)], 1e-6f));
    }
}

TEST_CASE("Forge space convolution: the registry gain bound holds over the real path",
          "[host][baked][param-injection][forge][forge-space][convolution][gain]") {
    const auto ir = room_ir();
    const conv::IrPolicy policy{};
    const float bound = conv::convolution_reverb_worst_case_gain(ir, policy, kSr, kFrames);
    REQUIRE(bound > 0.0f);

    // The bound is composed from the DSP's own measured-at-load L1 norm, and
    // the composition is re-derived here rather than restated.
    conv::Engine probe;
    probe.prepare(kSr, kFrames, 2);
    probe.set_normalize_mode(policy.normalize);
    std::vector<const float*> ptrs{ir.channels[0].data(), ir.channels[1].data()};
    REQUIRE(probe.load_impulse_response(ptrs.data(), 2,
                                        static_cast<int>(ir.channels[0].size()),
                                        ir.sample_rate));
    probe.set_ir_gain_db(conv::Engine::kIrGainDbMax);
    const double expected =
        1.0 + 1.0 * (conv::Engine::kWidthPercentMax / 100.0) * probe.worst_case_gain();
    INFO("bound " << bound << ", recomposed " << expected);
    REQUIRE_THAT(static_cast<double>(bound), WithinRel(expected, 1e-6));

    // And it really does bound the node: a full-scale input at every ceiling
    // cannot exceed it.
    Fixture fx(conv::make_convolution_reverb_node(ir, policy), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(conv::kIrGainDb, conv::Engine::kIrGainDbMax)) ==
            InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kPredelayMs, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWetPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWidthPercent, 200.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kLowcutHz, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kHighcutHz, 20000.0f)) == InjectStatus::Ok);
    const Ir rendered = render_impulse_response(fx, 24, 1.0f);
    INFO("peak " << peak(rendered.left) << " against bound " << bound);
    REQUIRE(peak(rendered.left) <= bound);
    REQUIRE(peak(rendered.right) <= bound);
}

TEST_CASE("Forge space convolution: process allocates nothing",
          "[host][baked][param-injection][forge][forge-space][convolution][rt]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.4f);
    pulp::test::ReusableRenderer<2> renderer(fx, {tone, tone});
    renderer.render();  // warm the first block outside the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 64; ++block) {
            const float u = static_cast<float>(block) / 64.0f;
            REQUIRE(inj.inject(immediate(conv::kIrGainDb, -12.0f + 24.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kPredelayMs, 50.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kWetPercent, 100.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f * (1.0f - u))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kWidthPercent, 200.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kLowcutHz, 20.0f + 480.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kHighcutHz, 20000.0f - 19000.0f * u)) ==
                    InjectStatus::Ok);
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge space convolution rejects malformed registration IRs",
          "[host][baked][forge][forge-space][convolution]") {
    auto ragged = delta_ir();
    ragged.channels[1].resize(ragged.channels[0].size() - 1);
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(ragged)),
                      std::invalid_argument);

    auto nonfinite = delta_ir();
    nonfinite.channels[0][7] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(nonfinite)),
                      std::invalid_argument);

    auto bad_count = delta_ir(3);
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(bad_count)),
                      std::invalid_argument);

    auto tiny_rate = delta_ir();
    tiny_rate.sample_rate = std::numeric_limits<double>::denorm_min();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(tiny_rate)),
                      std::invalid_argument);
    auto huge_rate = delta_ir();
    huge_rate.sample_rate = std::numeric_limits<double>::max();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(huge_rate)),
                      std::invalid_argument);

    for (double valid_rate : {8000.0, 768000.0}) {
        auto boundary = delta_ir();
        boundary.sample_rate = valid_rate;
        REQUIRE_NOTHROW(conv::make_convolution_reverb_node(std::move(boundary)));
    }
}

// ══ Nonlin ambience ═══════════════════════════════════════════════════════

TEST_CASE("Forge space ambience: the node bakes and runs true stereo",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();
    REQUIRE(std::string(type.type_id) == "space.nonlin_ambience");
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 14);

    // The seed is NOT among them — series law 2, and the file note's item 4.
    for (const auto& p : type.baked_params) REQUIRE(p.id != 0);
    auto declared = [&](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return true;
        return false;
    };
    REQUIRE(declared(amb::kProgram));
    REQUIRE(declared(amb::kMixPct));

    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    const auto out = fx.settle(impulse_block());
    REQUIRE(std::isfinite(out[0][0]));
    REQUIRE(std::isfinite(out[1][0]));
}

TEST_CASE("Forge space ambience: registration cannot invert the length range",
          "[host][baked][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node(amb::cal::kDefaultSeed, 1.0);
    const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                 [](const auto& p) { return p.id == amb::kLengthMs; });
    REQUIRE(it != type.baked_params.end());
    REQUIRE(it->min_value == static_cast<float>(amb::cal::kMinLengthMs));
    REQUIRE(it->max_value == static_cast<float>(amb::cal::kMinLengthMs));
    REQUIRE(it->default_value == static_cast<float>(amb::cal::kMinLengthMs));
}

TEST_CASE("Forge space ambience: the program param changes the envelope SHAPE",
          "[host][baked][param-injection][forge][forge-space][ambience][envelope]") {
    // The headline. Not "the output differs" — each program's rendered
    // impulse-response envelope has to have the shape that program is named
    // for, and the four shapes are mutually exclusive, so a program that
    // silently failed to switch fails three of the four.
    const auto type = amb::make_nonlin_ambience_node();
    constexpr double kLengthMs = 400.0;
    const int window_samples = static_cast<int>(kLengthMs * kSr / 1000.0);
    const int blocks = (window_samples + 8000) / kFrames + 2;
    const int win = static_cast<int>(0.020 * kSr);
    const int hop = static_cast<int>(0.010 * kSr);

    struct Rendered {
        std::vector<double> env;
        Ir ir;
    };
    auto measure = [&](int program) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        Rendered r;
        r.ir = render_program_ir(fx, inj, program, kLengthMs, blocks);
        r.env = envelope_db(r.ir.left, win, hop);
        REQUIRE(peak(r.ir.left) > 1e-4);
        return r;
    };
    auto tau_of = [&](std::size_t index) {
        return (static_cast<double>(index) * hop + win * 0.5) / window_samples;
    };

    SECTION("gated: a flat body and then a cut") {
        const auto r = measure(1);
        // Flat across the body — the shape a decaying tank cannot make.
        double lo = 1e9, hi = -1e9;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau > 0.10 && tau < 0.60) {
                lo = std::min(lo, r.env[i]);
                hi = std::max(hi, r.env[i]);
            }
        }
        INFO("gated body spread " << (hi - lo) << " dB");
        REQUIRE(hi - lo < 4.0);

        // And then gone. The measurement is taken after the diffuser's own
        // 60 dB ring time, which is what actually bounds the cut — the DSP's
        // pre-diffusion allpasses are recursive and that is disclosed in its
        // header rather than wished away.
        double body = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < r.env.size(); ++i)
            if (tau_of(i) > 0.10 && tau_of(i) < 0.60) {
                body += std::pow(10.0, r.env[i] / 10.0);
                ++count;
            }
        body /= count;
        const double late = std::pow(
            10.0, r.env[static_cast<std::size_t>(1.30 * window_samples / hop)] / 10.0);
        INFO("gated late level " << to_db(late / body) << " dB below the body");
        REQUIRE(to_db(late / body) < -50.0);
    }

    SECTION("reverse: the envelope rises") {
        const auto r = measure(2);
        // Monotone rising through the swell. No feedback reverb can do this,
        // which is why it is the assertion worth making at the node level.
        double previous = -1e9;
        int checked = 0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau < 0.15 || tau > 0.75) continue;
            INFO("tau " << tau << " level " << r.env[i] << " previous " << previous);
            REQUIRE(r.env[i] > previous - 1.0);
            previous = r.env[i];
            ++checked;
        }
        REQUIRE(checked > 8);

        // Late is louder than early — the defining comparison.
        double early = 0.0, late = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.20) < 0.03) early = r.env[i];
            if (std::fabs(tau_of(i) - 0.80) < 0.03) late = r.env[i];
        }
        INFO("reverse early " << early << " dB, late " << late << " dB");
        REQUIRE(late > early + 6.0);
    }

    SECTION("ambience: the envelope falls") {
        const auto r = measure(0);
        double early = 0.0, late = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.20) < 0.03) early = r.env[i];
            if (std::fabs(tau_of(i) - 0.80) < 0.03) late = r.env[i];
        }
        INFO("ambience early " << early << " dB, late " << late << " dB");
        REQUIRE(late < early - 20.0);

        // Monotone, which separates it from the humped NonLin2 program.
        double previous = 1e9;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau < 0.15 || tau > 0.90) continue;
            INFO("tau " << tau);
            REQUIRE(r.env[i] < previous + 1.0);
            previous = r.env[i];
        }
    }

    SECTION("nonlin2: the body ripples and then gates") {
        const auto r = measure(3);
        // Neither monotone falling (Ambience) nor monotone rising (Reverse):
        // the body goes up, down and up again. Measured as three probes
        // straddling the designed two humps at tau = 0.25 and 0.75.
        double first_hump = 0.0, trough = 0.0, second_hump = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.25) < 0.03) first_hump = r.env[i];
            if (std::fabs(tau_of(i) - 0.50) < 0.03) trough = r.env[i];
            if (std::fabs(tau_of(i) - 0.75) < 0.03) second_hump = r.env[i];
        }
        INFO("nonlin2 humps " << first_hump << " / " << trough << " / " << second_hump
                              << " dB");
        REQUIRE(first_hump > trough + 1.5);
        REQUIRE(second_hump > trough + 1.5);
    }

    SECTION("the four programs are four different renders") {
        // Belt and braces: the shapes above are what matter, but two programs
        // that produced identical audio would be a wiring bug the shape
        // assertions might individually tolerate.
        std::vector<Ir> renders;
        for (int program = 0; program < 4; ++program) renders.push_back(measure(program).ir);
        for (std::size_t a = 0; a < renders.size(); ++a)
            for (std::size_t b = a + 1; b < renders.size(); ++b) {
                INFO("programs " << a << " and " << b);
                REQUIRE(renders[a].left != renders[b].left);
            }
    }
}

TEST_CASE("Forge space ambience: the topology params reshape the field",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();

    SECTION("length_ms sets how long the field lasts") {
        // Measured with the diffuser bypassed, so the field's end is the last
        // tap rather than the last tap plus the allpasses' own ~118 ms ring.
        // With the ring in, a 150 ms field and a 600 ms field are 230 ms and
        // 568 ms of audible tail — still ordered, but the ratio is compressed
        // by a fixed additive term that has nothing to do with the knob.
        auto tail_end = [&](float length_ms) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, length_ms)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const int blocks =
                static_cast<int>((length_ms * kSr / 1000.0 + 8000) / kFrames) + 2;
            const Ir ir = render_impulse_response(fx, blocks);
            const double p = peak(ir.left);
            int last = 0;
            for (std::size_t n = 0; n < ir.left.size(); ++n)
                if (std::fabs(static_cast<double>(ir.left[n])) > p * 1e-3)
                    last = static_cast<int>(n);
            return last;
        };
        const int shortish = tail_end(150.0f);
        const int longish = tail_end(600.0f);
        // The gate closes at (h + w) of the window, so the ratio is the length
        // ratio: 600/150 = 4.
        INFO("field ends at " << shortish << " vs " << longish << " samples");
        REQUIRE_THAT(static_cast<double>(longish) / shortish, WithinRel(4.0, 0.15));
    }

    SECTION("predelay_ms delays the onset by the samples it names") {
        for (double ms : {0.0, 10.0}) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kPredelayMs, static_cast<float>(ms))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 40);
            const int onset = first_nonzero(ir.left, peak(ir.left) * 1e-3);
            const int expected = static_cast<int>(std::lround(ms * kSr / 1000.0));
            INFO("pre-delay " << ms << " ms: onset " << onset << ", at least " << expected);
            REQUIRE(onset >= expected);
        }
    }

    SECTION("density_pct makes the early field denser") {
        auto early_density = [&](float pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDensityPct, pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 16000.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            return active_samples(ir.left, 2000, static_cast<int>(0.02 * kSr));
        };
        const int sparse = early_density(10.0f);
        const int dense = early_density(100.0f);
        INFO("active samples early: sparse " << sparse << ", dense " << dense);
        REQUIRE(dense > sparse);
    }

    SECTION("density_growth decides whether the field densifies over time") {
        auto growth_ratio = [&](float gamma) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, 95.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDensityGrowth, gamma)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 16000.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            const int win = static_cast<int>(0.02 * kSr);
            const double early = active_samples(ir.left, 2000, win);
            const double late = active_samples(ir.left, static_cast<int>(0.7 * 0.8 * kSr), win);
            return late / std::max(early, 1.0);
        };
        const double flat = growth_ratio(0.0f);
        const double physical = growth_ratio(2.0f);
        INFO("late/early density ratio: gamma 0 -> " << flat << ", gamma 2 -> " << physical);
        REQUIRE_THAT(flat, WithinRel(1.0, 0.25));
        REQUIRE(physical > flat * 1.5);
    }

    SECTION("gate_hold_pct moves the gate and attack_pct moves the swell") {
        auto gate_end = [&](float hold_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 400.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, hold_pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 220);
            const double p = peak(ir.left);
            int last = 0;
            for (std::size_t n = 0; n < ir.left.size(); ++n)
                if (std::fabs(static_cast<double>(ir.left[n])) > p * 1e-3)
                    last = static_cast<int>(n);
            return last;
        };
        const int early_gate = gate_end(20.0f);
        const int late_gate = gate_end(90.0f);
        INFO("gate closes at " << early_gate << " vs " << late_gate << " samples");
        REQUIRE(late_gate > early_gate * 2);

        auto swell_peak = [&](float attack_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 2.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 400.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kAttackPct, attack_pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 220);
            const auto env = envelope_db(ir.left, static_cast<int>(0.02 * kSr),
                                         static_cast<int>(0.01 * kSr));
            // The FIRST window that reaches within 3 dB of the maximum — not
            // the argmax. Reverse holds a PLATEAU from `r` to 1, and across it
            // the program's reversed segment mapping keeps brightening, so the
            // absolute maximum sits near the very end of the window for EVERY
            // attack setting: measured, both 30 % and 95 % peaked at the same
            // window index 39. Where the swell ARRIVES is the thing the knob
            // moves, and that is what this finds.
            double max_db = -1e9;
            for (double v : env) max_db = std::max(max_db, v);
            for (std::size_t i = 0; i < env.size(); ++i)
                if (env[i] > max_db - 3.0) return i;
            return env.size();
        };
        const std::size_t short_rise = swell_peak(30.0f);
        const std::size_t long_rise = swell_peak(95.0f);
        INFO("swell arrives at window " << short_rise << " vs " << long_rise);
        REQUIRE(long_rise > short_rise);
    }
}

TEST_CASE("Forge space ambience: the continuous params move the audio per sample",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();

    SECTION("output_gain_db scales by exactly the dB injected") {
        auto level = [&](float db) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kOutputGainDb, db)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            return peak(render_impulse_response(fx, 200).left);
        };
        const double unity = level(0.0f);
        const double boosted = level(6.0f);
        INFO("peak ratio " << boosted / unity);
        REQUIRE_THAT(boosted / unity, WithinRel(std::pow(10.0, 6.0 / 20.0), 0.02));
    }

    SECTION("mix_pct at 0 is the dry wire, sample-aligned") {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
        settle_silent(fx, 8);
        const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
        const auto out = fx.render({tone, tone});
        for (int n = 0; n < kFrames; ++n) {
            INFO("sample " << n);
            REQUIRE_THAT(out[0][static_cast<std::size_t>(n)],
                         WithinAbs(tone[static_cast<std::size_t>(n)], 1e-6f));
        }
        REQUIRE(fx.baked().latency_samples() == 0);
    }

    SECTION("width_pct at 0 is exactly mono") {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kWidthPct, 0.0f)) == InjectStatus::Ok);
        settle_silent(fx, 32);
        const Ir ir = render_impulse_response(fx, 200);
        REQUIRE(peak(ir.left) > 1e-4);
        REQUIRE(ir.left == ir.right);

        Fixture wide(type, kSr, kFrames);
        ParamInjector wide_inj = wide.claim_injector();
        inject_ambience_defaults(wide_inj);
        settle_silent(wide, 32);
        const Ir wide_ir = render_impulse_response(wide, 200);
        REQUIRE(wide_ir.left != wide_ir.right);
    }

    SECTION("tone and hf_damp_hz steer the colour of the tail") {
        auto late_hf = [&](float tone_value, float damp_hz) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, 95.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kTone, tone_value)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, damp_hz)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            return hf_fraction(ir.left, static_cast<int>(0.6 * 0.8 * kSr),
                               static_cast<int>(0.05 * kSr));
        };
        const double dark = late_hf(-1.0f, 6000.0f);
        const double neutral = late_hf(0.0f, 6000.0f);
        const double bright = late_hf(1.0f, 6000.0f);
        INFO("late HF fraction: tone -1 " << dark << ", 0 " << neutral << ", +1 " << bright);
        REQUIRE(bright > neutral);
        REQUIRE(neutral > dark);

        const double damped = late_hf(0.0f, 1000.0f);
        const double open = late_hf(0.0f, 16000.0f);
        INFO("late HF fraction: hf_damp 1 kHz " << damped << ", 16 kHz " << open);
        REQUIRE(open > damped);
    }

    SECTION("diffusion smears the discrete taps") {
        auto crest = [&](float diffusion) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, diffusion)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 200);
            const double rms = std::sqrt(window_power(ir.left, 0, static_cast<int>(ir.left.size())));
            return peak(ir.left) / std::max(rms, 1e-30);
        };
        const double naked = crest(0.0f);
        const double diffused = crest(0.85f);
        INFO("crest factor: diffusion 0 -> " << naked << ", 0.85 -> " << diffused);
        REQUIRE(diffused < naked);
    }

    SECTION("converter_amount engages the character stage") {
        auto render_at = [&](float amount) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kConverterAmount, amount)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            return render_impulse_response(fx, 200);
        };
        const Ir off = render_at(0.0f);
        const Ir on = render_at(1.0f);
        double difference = 0.0;
        for (std::size_t n = 0; n < off.left.size(); ++n)
            difference = std::max(difference, std::fabs(static_cast<double>(on.left[n] -
                                                                           off.left[n])));
        INFO("largest difference with the converter engaged: " << difference);
        REQUIRE(difference > 0.0);
        REQUIRE(peak(on.left) > 1e-5);
    }
}

TEST_CASE("Forge space ambience: topology is block-rate, continuous is sample-rate",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    // The two-tier contract, asserted rather than described. A future change
    // that moved a topology param to per-sample reads would put a full tap-table
    // rebuild in the sample loop, and this is what would catch it.
    const auto type = amb::make_nonlin_ambience_node();
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    REQUIRE(inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
    settle_silent(fx, 8);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);

    // Continuous: a mid-block injection takes effect MID-BLOCK. Asserted as a
    // divergence point rather than as a level, because the continuous params
    // are smoothed over 20 ms — "the value arrives at sample 64" shows up as
    // "samples 0..63 are identical to a run that never saw the change, and
    // samples after 64 are not", which is true regardless of the ramp shape.
    // A block-rate read would leave the WHOLE block identical.
    Fixture unchanged(type, kSr, kFrames);
    ParamInjector unchanged_inj = unchanged.claim_injector();
    inject_ambience_defaults(unchanged_inj);
    REQUIRE(unchanged_inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
    settle_silent(unchanged, 8);
    const auto reference = unchanged.render({tone, tone});

    REQUIRE(inj.inject(immediate(amb::kMixPct, 100.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto split = fx.render({tone, tone});
    for (int n = 0; n < kFrames / 2; ++n) {
        INFO("sample " << n << " (before the injection offset)");
        REQUIRE_THAT(split[0][static_cast<std::size_t>(n)],
                     WithinAbs(reference[0][static_cast<std::size_t>(n)], 1e-7f));
    }
    double late_divergence = 0.0;
    for (int n = kFrames / 2; n < kFrames; ++n)
        late_divergence =
            std::max(late_divergence,
                     std::fabs(static_cast<double>(split[0][static_cast<std::size_t>(n)] -
                                                   reference[0][static_cast<std::size_t>(n)])));
    INFO("divergence after the injection offset " << late_divergence);
    REQUIRE(late_divergence > 1e-6);

    // Topology: a mid-block injection does NOT take effect until the next
    // block. Measured on the tap count's audible proxy — the program — by
    // asserting the whole block still matches a run that never saw the change.
    Fixture a(type, kSr, kFrames), b(type, kSr, kFrames);
    ParamInjector a_inj = a.claim_injector(), b_inj = b.claim_injector();
    inject_ambience_defaults(a_inj);
    inject_ambience_defaults(b_inj);
    a.render(impulse_block());
    b.render(impulse_block());
    // `b` asks for a different program halfway through the next block.
    REQUIRE(b_inj.inject(immediate(amb::kProgram, 2.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto a_out = a.render(silence());
    const auto b_out = b.render(silence());
    REQUIRE(a_out[0] == b_out[0]);  // block rate: the change waits
}

TEST_CASE("Forge space ambience: the registry gain bound composes the DSP's own",
          "[host][baked][param-injection][forge][forge-space][ambience][gain]") {
    namespace cal = pulp::signal::nonlin_ambience;
    const float bound = amb::nonlin_ambience_worst_case_gain();

    // Recomposed from the shipped constants rather than restated: the DSP's
    // closed-form bound at maximum diffusion with the converter engaged, times
    // the node's output-trim ceiling.
    const double expected = cal::worst_case_gain(cal::kDiffusionMax, true) *
                            std::pow(10.0, amb::kOutputGainDbMax / 20.0);
    INFO("bound " << bound << ", recomposed " << expected);
    REQUIRE_THAT(static_cast<double>(bound), WithinRel(expected, 1e-6));
    // The decomposition, each factor asserted so a constant change re-derives
    // rather than inherits: (1 + 2*0.85)^2 * 4 * 2 * 10^(24/20).
    REQUIRE_THAT(cal::worst_case_gain(cal::kDiffusionMax, false),
                 WithinRel(std::pow(1.0 + 2.0 * cal::kDiffusionMax, cal::kNumAllpass) *
                               cal::kL1Budget,
                           1e-12));
    REQUIRE_THAT(cal::worst_case_gain(cal::kDiffusionMax, true),
                 WithinRel(2.0 * cal::worst_case_gain(cal::kDiffusionMax, false), 1e-12));

    // And it bounds the node over the real path, at every ceiling at once.
    const auto type = amb::make_nonlin_ambience_node();
    for (int program = 0; program < 4; ++program) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kProgram, static_cast<float>(program))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kLengthMs, 200.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kDiffusion,
                                     static_cast<float>(cal::kDiffusionMax))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kConverterAmount, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kOutputGainDb, amb::kOutputGainDbMax)) ==
                InjectStatus::Ok);
        settle_silent(fx, 32);
        const Ir ir = render_impulse_response(fx, 200, 1.0f);
        INFO("program " << program << " peak " << peak(ir.left) << " against bound " << bound);
        REQUIRE(peak(ir.left) <= bound);
        REQUIRE(peak(ir.right) <= bound);
    }
}

TEST_CASE("Forge space ambience: process allocates nothing, including across a swap",
          "[host][baked][param-injection][forge][forge-space][ambience][rt]") {
    const auto type = amb::make_nonlin_ambience_node();
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    REQUIRE(inj.inject(immediate(amb::kLengthMs, 200.0f)) == InjectStatus::Ok);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.4f);
    pulp::test::ReusableRenderer<2> renderer(fx, {tone, tone});
    renderer.render();  // warm the first block outside the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 64; ++block) {
            const float u = static_cast<float>(block) / 64.0f;
            // Continuous, every block.
            REQUIRE(inj.inject(immediate(amb::kTone, 2.0f * u - 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 1000.0f + 17000.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kWidthPct, 100.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kMixPct, 100.0f * (1.0f - u))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kOutputGainDb, 6.0f * u - 3.0f)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kConverterAmount, u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.85f * u)) == InjectStatus::Ok);
            // And the topology path, which is the one that regenerates a tap
            // table into the pre-sized back bank and crossfades it in.
            if (block == 16) REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) ==
                                     InjectStatus::Ok);
            if (block == 32) REQUIRE(inj.inject(immediate(amb::kLengthMs, 260.0f)) ==
                                     InjectStatus::Ok);
            if (block == 48)
                REQUIRE(inj.inject(immediate(amb::kDensityPct, 90.0f)) == InjectStatus::Ok);
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge speaker cabinet declares and renders its complete mono node",
          "[host][baked][param-injection][forge][forge-space][cabinet]") {
    const auto type = cabinet::make_speaker_cabinet_node();
    CHECK(type.num_input_ports == 1);
    CHECK(type.num_output_ports == 1);
    CHECK(type.baked_params.size() == 14);
    CHECK(type.lowerable);

    const auto tone = pulp::test::sine_block(kFrames, 220.0, kSr, 0.35f);
    auto render = [&](float drive_db, float output_trim_db = 0.0f) {
        pulp::test::BakedNodeFixture<1> fx(type, kSr, kFrames);
        auto injector = fx.claim_injector();
        REQUIRE(injector.inject(immediate(cabinet::kDriveDb, drive_db)) == InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(cabinet::kOutputTrimDb, output_trim_db)) ==
                InjectStatus::Ok);
        return fx.settle({tone}, 24)[0];
    };
    const auto clean = render(0.0f);
    const auto driven = render(static_cast<float>(pulp::signal::SpeakerModel::kDriveDbMax),
                               static_cast<float>(pulp::signal::SpeakerModel::kOutputTrimDbMax));
    REQUIRE(std::all_of(driven.begin(), driven.end(), [](float v) { return std::isfinite(v); }));
    CHECK(clean != driven);
    CHECK(peak(driven) <= cabinet::speaker_cabinet_worst_case_gain());
    CHECK(cabinet::speaker_cabinet_worst_case_gain() ==
          static_cast<float>(pulp::signal::SpeakerModel{}.worst_case_gain() *
                             pulp::signal::units::db_to_linear(
                                 pulp::signal::SpeakerModel::kOutputTrimDbMax)));
}
