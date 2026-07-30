// Catalog nodes for the three compressor LINEAGES — VCA, FET, diode bridge.
//
// The feedforward member's suite lives in `test_forge_dynamics_catalog.cpp`;
// these three share that file's family header and its conventions but are
// separate nodes with separate parameter sets.
//
// THE BAR every case here is held to: a baked param has to be shown MOVING THE
// BAKED NODE'S AUDIO over the real production path — bake →
// `claim_param_injection` → `ParamInjector` → routed executor. A node that
// merely instantiates is not a node that works, and a param that is declared in
// the table but never read by `process_instance_baked_param` looks exactly like
// a working one until someone automates it. Every declared param is exercised
// below, and the assertions are directional (this knob makes THIS happen), not
// "the output changed".
//
// These three are MONO nodes, unlike the feedforward member's true stereo. That
// is deliberate and is asserted here rather than assumed: none of the three has
// a stereo link, so two ports would claim a coupling the DSP does not have.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_dynamics_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

using namespace pulp::host;
namespace dyn = pulp::host::dynamics;
using Catch::Matchers::WithinAbs;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 750.0;  // 64 samples/period: whole cycles per block

// A longer block, for the two measurements that need a tone below the shortest
// block's fundamental: the FET's transformer tilt (corner 60 Hz) and the diode
// bridge's sidechain high-pass (range 20..400 Hz). At 1024 frames the bin
// spacing is 46.875 Hz, so both tones below still hold whole cycles.
constexpr int kLongFrames = 1024;
constexpr double kLowToneHz = 46.875;

// A short block, for the FET's microsecond ballistics: its attack range is
// 20..800 µs, which is one to thirty-eight samples. Measured across 128-sample
// blocks every setting looks identical because every setting has finished.
constexpr int kShortFrames = 64;

using Fixture = pulp::test::BakedNodeFixture<1>;

std::vector<float> sine(float amp, int frames = kFrames, double hz = kToneHz) {
    return pulp::test::sine_block(frames, hz, kSr, amp);
}

std::vector<float> silence(int frames = kFrames) {
    return std::vector<float>(static_cast<std::size_t>(frames), 0.0f);
}

std::vector<float> dc(float amp, int frames) {
    return std::vector<float>(static_cast<std::size_t>(frames), amp);
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

double mean_abs(const std::vector<float>& b) {
    double s = 0.0;
    for (float v : b) s += std::fabs(v);
    return s / static_cast<double>(b.size());
}

/// Gain, in dB, of a settled block relative to its input amplitude.
double gain_db(const std::vector<float>& out, float in_amp) {
    return 20.0 * std::log10(std::max(static_cast<double>(peak(out)), 1e-12) / in_amp);
}

void require_finite(const std::vector<float>& b) {
    for (float v : b) REQUIRE(std::isfinite(v));
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════════
//  The VCA lineage
// ══════════════════════════════════════════════════════════════════════════

namespace {

Fixture vca_fixture(float lookahead_ms = 0.0f, int frames = kFrames) {
    return Fixture(dyn::vca::make_vca_compressor_node(lookahead_ms), kSr, frames);
}

/// Injects a full, explicit operating point so no case depends on a default.
void vca_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kRatio, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kTimeMs, 5.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kMix, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kNegativeRatio, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kNegRatioAmount, -4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kCeilingDb, 96.0f)) == InjectStatus::Ok);
}

}  // namespace

TEST_CASE("Forge dynamics VCA: the node bakes and runs mono",
          "[host][baked][forge][forge-dynamics][vca]") {
    const auto type = dyn::vca::make_vca_compressor_node();
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.lowerable);

    auto fx = vca_fixture();
    const auto tone = sine(0.5f);
    const auto out = fx.settle({tone});
    require_finite(out[0]);
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge dynamics VCA: threshold and ratio both deepen reduction",
          "[host][baked][param-injection][forge][forge-dynamics][vca]") {
    auto fx = vca_fixture();
    ParamInjector inj = fx.claim_injector();
    vca_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    // Threshold: at 0 dBFS nothing crosses; at −40 the tone is well over.
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    const double high = gain_db(fx.settle({tone})[0], amp);
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -40.0f)) == InjectStatus::Ok);
    const double low = gain_db(fx.settle({tone})[0], amp);
    REQUIRE_THAT(high, WithinAbs(0.0, 0.2));
    REQUIRE(low < high - 6.0);

    // Ratio: monotone across the declared range.
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    double previous = 1.0;
    for (float ratio : {1.0f, 2.0f, 8.0f, 20.0f}) {
        REQUIRE(inj.inject(immediate(dyn::vca::kRatio, ratio)) == InjectStatus::Ok);
        const double g = gain_db(fx.settle({tone})[0], amp);
        REQUIRE(g < previous);
        previous = g;
    }
}

TEST_CASE("Forge dynamics VCA: the OverEasy knee widens by the width injected",
          "[host][baked][param-injection][forge][forge-dynamics][vca]") {
    // A FRESH FIXTURE PER WIDTH. Sharing one across the sweep leaves the
    // previous width's detector still recovering into the next measurement —
    // which is what made the first draft of this case read a reduction where
    // the curve says there is none.
    //
    // The reference is the shipped DSP class run on the same tone, not a
    // literal: the node is asserted to reproduce its own compressor's gain
    // computer at the level that compressor's own detector reports.
    constexpr float kThreshold = -20.0f;
    constexpr float kRatio = 8.0f;
    constexpr float amp = 0.126f;  // lands the RMS detector just over threshold

    const auto node_gain = [&](float knee) {
        auto fx = vca_fixture();
        ParamInjector inj = fx.claim_injector();
        vca_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, kThreshold)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::vca::kRatio, kRatio)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::vca::kKneeDb, knee)) == InjectStatus::Ok);
        return gain_db(fx.settle({sine(amp)}, 64)[0], amp);
    };

    const auto reference_gain = [&](float knee) {
        pulp::signal::VcaCompressor64 ref;
        ref.prepare(kSr);
        ref.set_threshold_db(kThreshold);
        ref.set_ratio(kRatio);
        ref.set_knee_db(knee);
        ref.set_time_ms(5.0);
        ref.set_makeup_db(0.0);
        ref.reset();
        const double w = 2.0 * std::numbers::pi * kToneHz / kSr;
        for (int i = 0; i < static_cast<int>(kSr); ++i) ref.process(amp * std::sin(w * i));
        return ref.gain_reduction_db();
    };

    double previous = 1.0;
    std::vector<double> measured;
    for (float knee : {0.0f, 6.0f, 12.0f, 18.0f, 24.0f}) {
        const double g = node_gain(knee);
        REQUIRE_THAT(g, WithinAbs(reference_gain(knee), 0.3));
        REQUIRE(g < previous);  // strictly deeper at every width
        previous = g;
        measured.push_back(g);
    }
    // Not a sequence of near-identical values dressed up as a sweep: the span
    // is the one the shipped curve predicts.
    const double expected_span = reference_gain(0.0f) - reference_gain(24.0f);
    REQUIRE(expected_span > 1.0);
    REQUIRE_THAT(measured.front() - measured.back(), WithinAbs(expected_span, 0.4));
}

TEST_CASE("Forge dynamics VCA: the one time control sets how fast it reacts",
          "[host][baked][param-injection][forge][forge-dynamics][vca]") {
    // ONE knob for both directions is this lineage's architectural claim, so the
    // node has to carry it through. Measured as how much reduction has happened
    // a fixed distance into a step.
    const auto measure = [](float time_ms) {
        auto fx = vca_fixture();
        ParamInjector inj = fx.claim_injector();
        vca_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::vca::kTimeMs, time_ms)) == InjectStatus::Ok);
        fx.settle({silence()}, 8);
        const auto tone = sine(0.7f);
        double last = 0.0;
        for (int b = 0; b < 4; ++b) last = gain_db(fx.render({tone})[0], 0.7f);
        return last;  // ~10 ms into the step
    };
    const double fast = measure(1.0f);
    const double slow = measure(500.0f);
    REQUIRE(fast < slow - 3.0);  // the fast setting is already reducing
}

TEST_CASE("Forge dynamics VCA: the attack-release lock is wired at registration",
          "[host][baked][forge][forge-dynamics][vca]") {
    // The other construction-time argument. It is not a param because the DSP
    // documents it as a calibration constant rather than a performance control —
    // but "not a param" must not mean "not connected", so it is exercised the
    // same way. Attack τ is `time_ms / k`, so a larger k attacks faster at the
    // same time setting.
    const auto energy = [](double k) {
        auto fx = Fixture(dyn::vca::make_vca_compressor_node(0.0f, k), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        vca_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -40.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::vca::kRatio, 20.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::vca::kTimeMs, 200.0f)) == InjectStatus::Ok);
        fx.settle({silence()}, 8);
        return mean_abs(fx.render({sine(0.9f)})[0]);
    };
    REQUIRE(energy(dyn::vca::Comp::kRatioKMax) < energy(dyn::vca::Comp::kRatioKMin) * 0.98);
}

TEST_CASE("Forge dynamics VCA: makeup and mix move the level the way they say",
          "[host][baked][param-injection][forge][forge-dynamics][vca]") {
    auto fx = vca_fixture();
    ParamInjector inj = fx.claim_injector();
    vca_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    // The VCA's single-tau RMS detector is not converged after the fixture's
    // generic 16-block default. Use an explicit 48 blocks here so the 0.2 dB
    // makeup budget measures the gain control, not residual detector settling.
    const double unity = gain_db(fx.settle({tone}, 48)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::vca::kMakeupDb, 12.0f)) == InjectStatus::Ok);
    const double lifted = gain_db(fx.settle({tone}, 48)[0], amp);
    REQUIRE_THAT(lifted - unity, WithinAbs(12.0, 0.2));  // makeup is exactly makeup

    // Mix 0 is the dry signal: this node's dry path is EQUALLY DELAYED, so at
    // zero lookahead it is a straight pass-through.
    REQUIRE(inj.inject(immediate(dyn::vca::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -50.0f)) == InjectStatus::Ok);
    const double wet = gain_db(fx.settle({tone})[0], amp);
    REQUIRE(wet < -6.0);
    REQUIRE(inj.inject(immediate(dyn::vca::kMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(gain_db(fx.settle({tone})[0], amp), WithinAbs(0.0, 0.05));
}

TEST_CASE("Forge dynamics VCA: infinity-plus inverts the curve and the ceiling floors it",
          "[host][baked][param-injection][forge][forge-dynamics][vca]") {
    // Three params in one case because they only mean anything together:
    // `negative_ratio` selects the mode, `neg_ratio_amount` is the slope it
    // runs at, and `ceiling_db` is the only thing keeping that slope's output
    // finite.
    auto fx = vca_fixture();
    ParamInjector inj = fx.claim_injector();
    vca_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kRatio, 20.0f)) == InjectStatus::Ok);
    const double positive = gain_db(fx.settle({tone})[0], amp);

    REQUIRE(inj.inject(immediate(dyn::vca::kNegativeRatio, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kNegRatioAmount, -20.0f)) == InjectStatus::Ok);
    const double gentle_negative = gain_db(fx.settle({tone})[0], amp);
    REQUIRE(gentle_negative < positive);  // past the knee it keeps pushing down

    // A steeper negative slope pushes harder still.
    REQUIRE(inj.inject(immediate(dyn::vca::kNegRatioAmount, -1.0f)) == InjectStatus::Ok);
    const double steep_negative = gain_db(fx.settle({tone})[0], amp);
    REQUIRE(steep_negative < gentle_negative - 6.0);

    // And the ceiling really is what stops it: drive the curve past 60 dB of
    // reduction and the two ceiling settings must disagree.
    REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, -60.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::vca::kCeilingDb, 144.0f)) == InjectStatus::Ok);
    const double unclamped = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::vca::kCeilingDb, 60.0f)) == InjectStatus::Ok);
    const double clamped = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(clamped > unclamped + 6.0);  // the floor held the gain up
}

TEST_CASE("Forge dynamics VCA: lookahead is frozen at registration and IS the latency",
          "[host][baked][forge][forge-dynamics][vca][latency]") {
    // The realization argument. It is not injectable precisely because it moves
    // `latency_samples()`, and a node whose latency moves under the audio thread
    // breaks the host's delay compensation. Verified by where an impulse lands:
    // at ratio 1 with no makeup the node is a pure delay.
    for (float ms : {0.0f, 2.0f}) {
        auto fx = vca_fixture(ms);
        ParamInjector inj = fx.claim_injector();
        vca_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::vca::kRatio, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::vca::kThresholdDb, 0.0f)) == InjectStatus::Ok);
        fx.settle({silence()}, 4);

        auto impulse = silence();
        impulse[0] = 0.5f;
        auto joined = fx.render({impulse})[0];
        const auto tail = fx.render({silence()})[0];
        joined.insert(joined.end(), tail.begin(), tail.end());

        int index = -1;
        float best = 0.0f;
        for (int i = 0; i < static_cast<int>(joined.size()); ++i) {
            if (std::fabs(joined[static_cast<std::size_t>(i)]) > best) {
                best = std::fabs(joined[static_cast<std::size_t>(i)]);
                index = i;
            }
        }
        REQUIRE(index == static_cast<int>(std::llround(ms * kSr / 1000.0)));
    }
}

TEST_CASE("Forge dynamics VCA: the registry gain bound is the makeup ceiling",
          "[host][baked][forge][forge-dynamics][vca]") {
    // Series law 8. Feedforward topology and a reducing-only gain computer, so
    // the bound is exactly the makeup ceiling — the invariant the DSP suite
    // asserts, restated from the shipped constant rather than measured again.
    REQUIRE_THAT(static_cast<double>(dyn::vca::vca_compressor_worst_case_gain()),
                 WithinAbs(std::pow(10.0, dyn::vca::Comp::kMakeupDbMax / 20.0), 1e-4));
}

TEST_CASE("Forge dynamics VCA: the node's process path allocates nothing",
          "[host][baked][forge][forge-dynamics][vca][rt-safety]") {
    auto fx = vca_fixture();
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    fx.settle({tone}, 8);
    pulp::test::ReusableRenderer<1> renderer(fx, {tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dyn::vca::kThresholdDb, static_cast<float>(-50 + b)));
        inj.inject(immediate(dyn::vca::kRatio, 1.0f + 0.5f * static_cast<float>(b % 20)));
        inj.inject(immediate(dyn::vca::kTimeMs, 1.0f + 10.0f * static_cast<float>(b % 20)));
        inj.inject(immediate(dyn::vca::kNegativeRatio, (b % 2) ? 1.0f : 0.0f));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

// ══════════════════════════════════════════════════════════════════════════
//  The FET lineage
// ══════════════════════════════════════════════════════════════════════════

namespace {

Fixture fet_fixture(int frames = kFrames) {
    return Fixture(dyn::fet::make_fet_compressor_node(), kSr, frames);
}

void fet_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kOutputGainDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kRatio, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kAttackUs, 200.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kReleaseMs, 300.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kTransformerAmount, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kMix, 1.0f)) == InjectStatus::Ok);
}

}  // namespace

TEST_CASE("Forge dynamics FET: the node bakes and runs mono",
          "[host][baked][forge][forge-dynamics][fet]") {
    const auto type = dyn::fet::make_fet_compressor_node();
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.lowerable);
    REQUIRE(std::string(type.type_id) == "dynamics.fet_compressor");

    auto fx = fet_fixture();
    const auto tone = sine(0.5f);
    const auto out = fx.settle({tone});
    require_finite(out[0]);
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge dynamics FET: input gain is the only lever into reduction",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    // This lineage has no threshold control on purpose. Raising input gain must
    // raise the output — but SUB-LINEARLY, because the extra level is what
    // drives the compression. A node that ignored the param would move 1:1 with
    // it, and one that never compressed would too.
    auto fx = fet_fixture();
    ParamInjector inj = fx.claim_injector();
    fet_baseline(inj);
    const float amp = 0.25f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 6.0f)) == InjectStatus::Ok);
    const double lower = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 18.0f)) == InjectStatus::Ok);
    const double higher = gain_db(fx.settle({tone}, 64)[0], amp);

    REQUIRE(higher > lower);              // it does reach the parameter
    REQUIRE(higher - lower < 12.0 - 3.0); // and 12 dB in buys well under 12 dB out
}

TEST_CASE("Forge dynamics FET: output gain is exactly output gain",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    auto fx = fet_fixture();
    ParamInjector inj = fx.claim_injector();
    fet_baseline(inj);
    const float amp = 0.25f;
    const auto tone = sine(amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 12.0f)) == InjectStatus::Ok);

    const double unity = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kOutputGainDb, 9.0f)) == InjectStatus::Ok);
    const double lifted = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE_THAT(lifted - unity, WithinAbs(9.0, 0.2));
}

TEST_CASE("Forge dynamics FET: the ratio buttons step monotonically including all-in",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    // The stepped param, walked across all five positions. The measured output
    // must fall at every step: 4:1 → 8:1 → 12:1 → 20:1 tighten the closed-loop
    // curve, and all-buttons-in adds its documented bias shift on top of the
    // 20:1 curve.
    auto fx = fet_fixture();
    ParamInjector inj = fx.claim_injector();
    fet_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 18.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 0.0f)) == InjectStatus::Ok);

    double previous = 1e9;
    for (int step = 0; step <= static_cast<int>(dyn::fet::kRatioSteps); ++step) {
        REQUIRE(inj.inject(immediate(dyn::fet::kRatio, static_cast<float>(step))) ==
                InjectStatus::Ok);
        const double g = gain_db(fx.settle({tone}, 96)[0], amp);
        REQUIRE(g < previous);
        previous = g;
    }
}

TEST_CASE("Forge dynamics FET: the microsecond attack and the release both reach the audio",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    // Attack is measured on a SHORT block because the whole documented range
    // (20..800 µs) finishes inside one 128-sample block — across normal blocks
    // every setting would look the same and the test would pass while asserting
    // nothing.
    const auto attack_energy = [](float attack_us) {
        auto fx = fet_fixture(kShortFrames);
        ParamInjector inj = fx.claim_injector();
        fet_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 24.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::fet::kAttackUs, attack_us)) == InjectStatus::Ok);
        fx.settle({silence(kShortFrames)}, 16);
        return mean_abs(fx.render({dc(0.8f, kShortFrames)})[0]);
    };
    // The fast setting has clamped down inside the block; the slow one is still
    // letting the step through.
    REQUIRE(attack_energy(20.0f) < attack_energy(800.0f) * 0.9);

    const auto recovery = [](float release_ms) {
        auto fx = fet_fixture();
        ParamInjector inj = fx.claim_injector();
        fet_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 24.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::fet::kReleaseMs, release_ms)) == InjectStatus::Ok);
        const auto loud = sine(0.9f);
        fx.settle({loud}, 96);
        const float quiet = 0.02f;
        const auto soft = sine(quiet);
        double last = 0.0;
        for (int b = 0; b < 8; ++b) last = gain_db(fx.render({soft})[0], quiet);  // ~21 ms
        return last;
    };
    // A short release has let go by 21 ms; the longest one has barely moved.
    REQUIRE(recovery(50.0f) > recovery(1100.0f) + 3.0);
}

TEST_CASE("Forge dynamics FET: the knee widens where reduction starts",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    auto fx = fet_fixture();
    ParamInjector inj = fx.claim_injector();
    fet_baseline(inj);
    // A level just under the fixed internal reference, where only a wide knee
    // reaches.
    const float amp = 0.1f;
    const auto tone = sine(amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kInputGainDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::fet::kRatio, 4.0f)) == InjectStatus::Ok);  // all-in

    REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 0.0f)) == InjectStatus::Ok);
    const double hard = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::fet::kKneeDb, 6.0f)) == InjectStatus::Ok);
    const double soft = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(soft < hard - 0.3);
}

TEST_CASE("Forge dynamics FET: the transformer tilts the low end and mix blends it away",
          "[host][baked][param-injection][forge][forge-dynamics][fet]") {
    // The tilt's corner is 60 Hz, so this needs the long block: at 128 frames
    // the lowest tone holding whole cycles is 375 Hz, where the shelf is
    // essentially flat and the measurement would read zero for both settings.
    const auto low_level = [](float transformer, float mix) {
        auto fx = fet_fixture(kLongFrames);
        ParamInjector inj = fx.claim_injector();
        fet_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::fet::kTransformerAmount, transformer)) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::fet::kMix, mix)) == InjectStatus::Ok);
        const auto tone = sine(0.05f, kLongFrames, kLowToneHz);
        return gain_db(fx.settle({tone}, 24)[0], 0.05f);
    };
    const double flat = low_level(0.0f, 1.0f);
    const double tilted = low_level(1.0f, 1.0f);
    REQUIRE(tilted < flat - 1.0);          // the shelf really cuts
    REQUIRE(tilted > flat - 4.0);          // ...by about the design's 3 dB, not wildly
    // Mix 0 is the dry path, which is taken PRE input gain and delay-aligned, so
    // the tilt disappears with it.
    REQUIRE_THAT(low_level(1.0f, 0.0f), WithinAbs(flat, 0.2));
}

TEST_CASE("Forge dynamics FET: latency is 16 samples and never moves",
          "[host][baked][forge][forge-dynamics][fet][latency]") {
    // Nothing about this member is a realization because nothing about it moves
    // latency: the resampling pair's group delay is the whole figure. Asserted
    // against the DSP's own constant, and against where an impulse lands, at
    // both ends of the ratio switch and the ballistics.
    for (float ratio : {0.0f, dyn::fet::kRatioSteps}) {
        for (float attack : {20.0f, 800.0f}) {
            auto fx = fet_fixture();
            ParamInjector inj = fx.claim_injector();
            fet_baseline(inj);
            REQUIRE(inj.inject(immediate(dyn::fet::kRatio, ratio)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(dyn::fet::kAttackUs, attack)) == InjectStatus::Ok);
            fx.settle({silence()}, 8);

            auto impulse = silence();
            impulse[0] = 0.01f;  // small enough that nothing engages
            const auto out = fx.render({impulse})[0];
            int index = -1;
            float best = 0.0f;
            for (int i = 0; i < static_cast<int>(out.size()); ++i) {
                if (std::fabs(out[static_cast<std::size_t>(i)]) > best) {
                    best = std::fabs(out[static_cast<std::size_t>(i)]);
                    index = i;
                }
            }
            REQUIRE(index == dyn::fet::Comp::kLatencySamples);
        }
    }
}

TEST_CASE("Forge dynamics FET: the registry gain bound is the DSP's own closed form",
          "[host][baked][forge][forge-dynamics][fet]") {
    // Series law 8, and the case where it matters most in this family: this
    // member has a feedback loop. The registry number is the DSP's closed-form
    // ℓ∞ bound at the node's parameter CEILINGS, which is what a baked param
    // can be automated to.
    pulp::signal::FetCompressor expected;
    expected.set_input_gain_db(dyn::fet::Comp::kInputGainDbMax);
    expected.set_output_gain_db(dyn::fet::Comp::kOutputGainDbMax);
    expected.set_mix(1.0);
    REQUIRE_THAT(static_cast<double>(dyn::fet::fet_compressor_worst_case_gain()),
                 WithinAbs(expected.worst_case_gain(), 1e-3));

    // Not a vacuous bound, and not an estimate: it is assembled from the shipped
    // gain stages and the resampling pair's own ℓ1 product.
    REQUIRE(dyn::fet::fet_compressor_worst_case_gain() >
            static_cast<float>(std::pow(10.0, (dyn::fet::Comp::kInputGainDbMax +
                                               dyn::fet::Comp::kOutputGainDbMax) /
                                                  20.0)));
}

TEST_CASE("Forge dynamics FET: the node's process path allocates nothing",
          "[host][baked][forge][forge-dynamics][fet][rt-safety]") {
    auto fx = fet_fixture();
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    fx.settle({tone}, 8);
    pulp::test::ReusableRenderer<1> renderer(fx, {tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dyn::fet::kInputGainDb, static_cast<float>(-20 + b)));
        inj.inject(immediate(dyn::fet::kRatio, static_cast<float>(b % 5)));
        inj.inject(immediate(dyn::fet::kAttackUs, 20.0f + 20.0f * static_cast<float>(b % 39)));
        inj.inject(immediate(dyn::fet::kTransformerAmount, 0.03f * static_cast<float>(b % 33)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

// ══════════════════════════════════════════════════════════════════════════
//  The diode-bridge lineage
// ══════════════════════════════════════════════════════════════════════════

namespace {

Fixture diode_fixture(bool feedback = true, bool adaa = true, int frames = kFrames) {
    return Fixture(dyn::diode::make_diode_bridge_compressor_node(feedback, adaa), kSr, frames);
}

void diode_baseline(ParamInjector& inj) {
    REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -12.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kRatio, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kAttackMs, 3.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kReleaseMs, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kCharacter, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kMixPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kScHpfHz, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kAutoRelease, 0.0f)) == InjectStatus::Ok);
}

}  // namespace

TEST_CASE("Forge dynamics diode: both topologies bake and run mono",
          "[host][baked][forge][forge-dynamics][diode]") {
    const auto fb = dyn::diode::make_diode_bridge_compressor_node(true);
    const auto ff = dyn::diode::make_diode_bridge_compressor_node(false);
    REQUIRE(fb.num_input_ports == 1);
    REQUIRE(fb.num_output_ports == 1);
    // Two REGISTERED realizations, not one node with a switch — distinct type
    // ids, so a baked artifact records which topology it was authored against.
    REQUIRE(std::string(fb.type_id) != std::string(ff.type_id));

    for (bool feedback : {true, false}) {
        auto fx = diode_fixture(feedback);
        const auto tone = sine(0.5f);
        const auto out = fx.settle({tone});
        require_finite(out[0]);
        REQUIRE(peak(out[0]) > 0.0f);
    }
}

TEST_CASE("Forge dynamics diode: the feedback realization really is a different curve",
          "[host][baked][forge][forge-dynamics][diode]") {
    // The reason feedback is a realization rather than a param. A feedback
    // detector reads the compressor's own output, so the level it senses is the
    // already-reduced one and the loop settles at LESS reduction than the
    // feedforward version at the same knob settings. If the realization argument
    // were being dropped on the floor, these two would be equal.
    const auto measure = [](bool feedback) {
        auto fx = diode_fixture(feedback);
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, 10.0f)) == InjectStatus::Ok);
        const auto tone = sine(0.7f);
        return gain_db(fx.settle({tone}, 96)[0], 0.7f);
    };
    const double feedback = measure(true);
    const double feedforward = measure(false);
    REQUIRE(feedforward < feedback - 3.0);
}

TEST_CASE("Forge dynamics diode: threshold ratio and knee shape the curve",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    auto fx = diode_fixture();
    ParamInjector inj = fx.claim_injector();
    diode_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, 6.0f)) == InjectStatus::Ok);
    const double high = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -40.0f)) == InjectStatus::Ok);
    const double low = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE_THAT(high, WithinAbs(0.0, 0.3));
    REQUIRE(low < high - 6.0);

    REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -24.0f)) == InjectStatus::Ok);
    double previous = 1.0;
    for (float ratio : {1.5f, 4.0f, 10.0f, 20.0f}) {
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, ratio)) == InjectStatus::Ok);
        const double g = gain_db(fx.settle({tone}, 64)[0], amp);
        REQUIRE(g < previous);
        previous = g;
    }

}

TEST_CASE("Forge dynamics diode: the knee widens by the width injected",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    // Split out of the case above, with a FRESH FIXTURE PER WIDTH. Sharing the
    // fixture with the ratio sweep left that sweep's gain reduction still
    // releasing 170 ms later, which read as a knee that reduces below its own
    // threshold — the same trap the feedforward suite's stereo-link case
    // documents.
    constexpr float kThreshold = -18.0f;
    constexpr float kRatio = 20.0f;
    constexpr float amp = 0.14f;  // ≈ −17 dBFS: just over threshold

    const auto node_gain = [&](float knee) {
        auto fx = diode_fixture();
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, kThreshold)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, kRatio)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kKneeDb, knee)) == InjectStatus::Ok);
        return gain_db(fx.settle({sine(amp)}, 96)[0], amp);
    };

    // Reference: the shipped DSP class on the same tone, so the expected span
    // comes from the curve rather than from a literal. This member's default
    // realization is the FEEDBACK topology, which divides the curve's reduction
    // down — using the feedforward curve here would over-predict it.
    const auto reference_gain = [&](float knee) {
        pulp::signal::DiodeBridgeCompressor64 ref;
        ref.prepare(kSr);
        ref.set_threshold_db(kThreshold);
        ref.set_ratio(kRatio);
        ref.set_knee_db(knee);
        ref.set_attack_ms(3.0);
        ref.set_release_ms(100.0);
        ref.set_character(0.0);
        ref.set_feedback(true);
        ref.reset();
        const double w = 2.0 * std::numbers::pi * kToneHz / kSr;
        for (int i = 0; i < static_cast<int>(kSr); ++i) ref.process(amp * std::sin(w * i));
        return ref.gain_reduction_db();
    };

    double previous = 1.0;
    std::vector<double> measured;
    for (float knee : {0.0f, 6.0f, 12.0f, 18.0f}) {
        const double g = node_gain(knee);
        REQUIRE(g < previous);
        previous = g;
        measured.push_back(g);
    }
    const double expected_span = reference_gain(0.0f) - reference_gain(18.0f);
    REQUIRE(expected_span > 0.5);
    REQUIRE_THAT(measured.front() - measured.back(), WithinAbs(expected_span, 0.4));
}

TEST_CASE("Forge dynamics diode: attack release and auto-release all reach the audio",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    const auto attack_energy = [](float attack_ms) {
        auto fx = diode_fixture();
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, 20.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kAttackMs, attack_ms)) == InjectStatus::Ok);
        fx.settle({silence()}, 16);
        return mean_abs(fx.render({sine(0.9f)})[0]);
    };
    REQUIRE(attack_energy(0.5f) < attack_energy(100.0f) * 0.9);

    const auto recovery = [](float release_ms, bool auto_release) {
        auto fx = diode_fixture();
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, 20.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kReleaseMs, release_ms)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kAutoRelease, auto_release ? 1.0f : 0.0f)) ==
                InjectStatus::Ok);
        fx.settle({sine(0.9f)}, 128);
        const float quiet = 0.01f;
        const auto soft = sine(quiet);
        double last = 0.0;
        for (int b = 0; b < 24; ++b) last = gain_db(fx.render({soft})[0], quiet);
        return last;
    };
    REQUIRE(recovery(50.0f, false) > recovery(2000.0f, false) + 3.0);

    // Auto-release is the program-dependent dual slope: after SUSTAINED
    // compression its slow branch is engaged, so it holds the gain down longer
    // than the same release time with the mode off.
    REQUIRE(recovery(50.0f, true) < recovery(50.0f, false) - 1.0);
}

TEST_CASE("Forge dynamics diode: makeup and mix move the level the way they say",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    auto fx = diode_fixture();
    ParamInjector inj = fx.claim_injector();
    diode_baseline(inj);
    const float amp = 0.5f;
    const auto tone = sine(amp);
    REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);

    const double unity = gain_db(fx.settle({tone}, 64)[0], amp);
    REQUIRE(unity < -3.0);  // it is compressing, so mix has something to blend
    REQUIRE(inj.inject(immediate(dyn::diode::kMakeupDb, 12.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(gain_db(fx.settle({tone}, 64)[0], amp) - unity, WithinAbs(12.0, 0.3));

    REQUIRE(inj.inject(immediate(dyn::diode::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::diode::kMixPercent, 0.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(gain_db(fx.settle({tone}, 64)[0], amp), WithinAbs(0.0, 0.05));
}

TEST_CASE("Forge dynamics diode: character drives the bridge into harmonics",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    // The colour control. The bridge is a symmetric pair, so what it generates
    // is ODD harmonics — measured at the third, coherently, which is exact
    // because the tone holds whole cycles in the block.
    const auto third_harmonic = [](float character) {
        auto fx = diode_fixture();
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kCharacter, character)) == InjectStatus::Ok);
        const auto tone = sine(0.7f);
        const auto out = fx.settle({tone}, 64)[0];
        const double fundamental = pulp::test::harmonic_magnitude(out, 1, kToneHz, kSr);
        const double third = pulp::test::harmonic_magnitude(out, 3, kToneHz, kSr);
        return third / std::max(fundamental, 1e-12);
    };
    REQUIRE(third_harmonic(1.0f) > third_harmonic(0.0f) * 1.5);
}

TEST_CASE("Forge dynamics diode: the sidechain high-pass de-sensitises the low end",
          "[host][baked][param-injection][forge][forge-dynamics][diode]") {
    // The documented behaviour: the compressor stops chasing the kick. Needs the
    // long block, because the control's whole range (20..400 Hz) is below the
    // lowest tone a 128-sample block can hold.
    const auto reduction = [](float corner_hz) {
        auto fx = diode_fixture(true, true, kLongFrames);
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kRatio, 20.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kScHpfHz, corner_hz)) == InjectStatus::Ok);
        const auto tone = sine(0.7f, kLongFrames, kLowToneHz);
        return gain_db(fx.settle({tone}, 24)[0], 0.7f);
    };
    // With the corner well above the tone the detector barely sees it, so less
    // gain reduction; with the corner below, it sees it fully.
    REQUIRE(reduction(400.0f) > reduction(20.0f) + 1.0);
}

TEST_CASE("Forge dynamics diode: the ADAA realization changes the alias signature",
          "[host][baked][forge][forge-dynamics][diode]") {
    // The other registration-time choice. ADAA does not move this member's
    // latency — it reports 0 either way — so the proof that the argument is
    // wired is that the two builds distort differently under the same drive.
    const auto third = [](bool adaa) {
        auto fx = diode_fixture(true, adaa);
        ParamInjector inj = fx.claim_injector();
        diode_baseline(inj);
        REQUIRE(inj.inject(immediate(dyn::diode::kThresholdDb, -30.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dyn::diode::kCharacter, 1.0f)) == InjectStatus::Ok);
        const auto tone = sine(0.9f);
        const auto out = fx.settle({tone}, 64)[0];
        require_finite(out);
        return pulp::test::harmonic_magnitude(out, 3, kToneHz, kSr);
    };
    REQUIRE(third(true) != third(false));
}

TEST_CASE("Forge dynamics diode: the registry gain bound is the DSP's own",
          "[host][baked][forge][forge-dynamics][diode]") {
    // Series law 8: the module's own asserted bound — makeup ceiling times both
    // transformer brackets' peak gain, the bridge contributing at most 1 because
    // it is an attenuator — reported rather than re-derived, so the two cannot
    // drift apart.
    REQUIRE_THAT(static_cast<double>(dyn::diode::diode_bridge_compressor_worst_case_gain()),
                 WithinAbs(dyn::diode::Comp::worst_case_gain(), 1e-6));
    REQUIRE(dyn::diode::diode_bridge_compressor_worst_case_gain() > 1.0f);
}

TEST_CASE("Forge dynamics diode: the node's process path allocates nothing",
          "[host][baked][forge][forge-dynamics][diode][rt-safety]") {
    auto fx = diode_fixture();
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    fx.settle({tone}, 8);
    pulp::test::ReusableRenderer<1> renderer(fx, {tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dyn::diode::kThresholdDb, static_cast<float>(-40 + b)));
        inj.inject(immediate(dyn::diode::kRatio, 1.5f + 0.5f * static_cast<float>(b % 37)));
        inj.inject(immediate(dyn::diode::kScHpfHz, 20.0f + 10.0f * static_cast<float>(b % 38)));
        inj.inject(immediate(dyn::diode::kAutoRelease, (b % 2) ? 1.0f : 0.0f));
        inj.inject(immediate(dyn::diode::kCharacter, 0.03f * static_cast<float>(b % 33)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

// ══════════════════════════════════════════════════════════════════════════
//  Family-level invariants
// ══════════════════════════════════════════════════════════════════════════

TEST_CASE("Forge dynamics: every lineage node declares a complete param table",
          "[host][baked][forge][forge-dynamics]") {
    // Guards the failure mode this whole file exists for: a param declared in
    // the table but never read in `process_instance_baked_param` is invisible
    // until someone automates it. Every id below is exercised by a case above;
    // this asserts the TABLE agrees with that list, so adding a param without a
    // test fails here.
    const auto ids = [](const CustomNodeType& t) {
        std::vector<pulp::state::ParamID> v;
        for (const auto& p : t.baked_params) v.push_back(p.id);
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto sane = [](const CustomNodeType& t) {
        for (const auto& p : t.baked_params) {
            REQUIRE(p.min_value < p.max_value);
            REQUIRE(p.default_value >= p.min_value);
            REQUIRE(p.default_value <= p.max_value);
        }
    };

    const auto vca = dyn::vca::make_vca_compressor_node();
    REQUIRE(ids(vca) == std::vector<pulp::state::ParamID>{
                            dyn::vca::kThresholdDb, dyn::vca::kRatio, dyn::vca::kKneeDb,
                            dyn::vca::kTimeMs, dyn::vca::kMakeupDb, dyn::vca::kMix,
                            dyn::vca::kNegativeRatio, dyn::vca::kNegRatioAmount,
                            dyn::vca::kCeilingDb});
    sane(vca);

    const auto fet = dyn::fet::make_fet_compressor_node();
    REQUIRE(ids(fet) == std::vector<pulp::state::ParamID>{
                            dyn::fet::kInputGainDb, dyn::fet::kOutputGainDb, dyn::fet::kRatio,
                            dyn::fet::kAttackUs, dyn::fet::kReleaseMs, dyn::fet::kKneeDb,
                            dyn::fet::kTransformerAmount, dyn::fet::kMix});
    sane(fet);

    const auto diode = dyn::diode::make_diode_bridge_compressor_node();
    REQUIRE(ids(diode) == std::vector<pulp::state::ParamID>{
                              dyn::diode::kThresholdDb, dyn::diode::kRatio, dyn::diode::kKneeDb,
                              dyn::diode::kAttackMs, dyn::diode::kReleaseMs,
                              dyn::diode::kMakeupDb, dyn::diode::kCharacter,
                              dyn::diode::kMixPercent, dyn::diode::kScHpfHz,
                              dyn::diode::kAutoRelease});
    sane(diode);
}

TEST_CASE("Forge dynamics: the four members occupy distinct type ids",
          "[host][baked][forge][forge-dynamics]") {
    std::vector<std::string> ids{
        dyn::make_feedforward_compressor_node().type_id,
        dyn::vca::make_vca_compressor_node().type_id,
        dyn::fet::make_fet_compressor_node().type_id,
        dyn::diode::make_diode_bridge_compressor_node(true).type_id,
        dyn::diode::make_diode_bridge_compressor_node(false).type_id,
    };
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}
