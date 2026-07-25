#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_dynamics_catalog.hpp>

#include <cmath>
#include <vector>

using namespace pulp::host;
namespace dyn = pulp::host::dynamics;
using Catch::Matchers::WithinAbs;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 750.0;  // 64 samples/period: whole cycles per block

// TRUE STEREO: the node's two ports are L and R of one logical wire, so the
// fixture is built with two channels rather than instanced twice. The
// stereo-link case below depends on that being true.
using Fixture = pulp::test::BakedNodeFixture<2>;

Fixture make_fixture(const CustomNodeType& type) { return Fixture(type, kSr, kFrames); }

std::vector<float> sine(float amp) {
    return pulp::test::sine_block(kFrames, kToneHz, kSr, amp);
}

std::vector<float> silence() {
    return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
}

double harmonic(const std::vector<float>& x, int k) {
    return pulp::test::harmonic_magnitude(x, k, kToneHz, kSr);
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

/// Gain, in dB, of a settled block relative to its input amplitude.
double gain_db(const std::vector<float>& out, float in_amp) {
    return 20.0 * std::log10(std::max(static_cast<double>(peak(out)), 1e-12) / in_amp);
}

}  // namespace

TEST_CASE("Forge dynamics: the compressor bakes and runs",
          "[host][baked][forge][forge-dynamics]") {
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    const auto tone = sine(0.5f);
    const auto out = fx.settle({tone, tone});
    for (int ch = 0; ch < 2; ++ch)
        for (float v : out[static_cast<std::size_t>(ch)]) REQUIRE(std::isfinite(v));
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge dynamics: injecting threshold deepens gain reduction",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kMakeupDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 8.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    const double high = gain_db(fx.settle({tone, tone})[0], amp);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -40.0f)) == InjectStatus::Ok);
    const double low = gain_db(fx.settle({tone, tone})[0], amp);

    REQUIRE_THAT(high, WithinAbs(0.0, 0.2));  // above threshold: untouched
    REQUIRE(low < high - 6.0);                // well below: substantial reduction
}

TEST_CASE("Forge dynamics: injecting ratio deepens gain reduction",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();
    const float amp = 0.5f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);

    double previous = 1.0;
    for (float ratio : {1.0f, 2.0f, 8.0f, 100.0f}) {
        REQUIRE(inj.inject(immediate(dyn::kRatio, ratio)) == InjectStatus::Ok);
        const double g = gain_db(fx.settle({tone, tone})[0], amp);
        REQUIRE(g < previous);
        previous = g;
    }
}

TEST_CASE("Forge dynamics: the stereo link survives the graph",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    // The true-stereo wiring assertion. A hard-panned transient must pull BOTH
    // channels down when linked, and only its own channel when unlinked. A node
    // wired dual-mono would pass every other test in this file and fail here.
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -30.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 8.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kProgramDependent, 0.0f)) == InjectStatus::Ok);
    // A fast release, because the two halves of this test share one fixture:
    // the unlinked measurement follows the linked one, and at the default
    // 120 ms release the right channel's detector is still recovering from the
    // linked pass 170 ms later — which reads as "the link is stuck on".
    REQUIRE(inj.inject(immediate(dyn::kReleaseMs,
                                 static_cast<float>(
                                     pulp::signal::FeedforwardCompressor::kReleaseMsMin))) ==
            InjectStatus::Ok);

    // A quiet tone on the right so there is something to attenuate, a loud one
    // on the left to drive the detector.
    const float quiet = 0.02f;  // −34 dBFS: on its own, below threshold
    const auto loud_left = sine(0.9f);
    const auto quiet_right = sine(quiet);

    REQUIRE(inj.inject(immediate(dyn::kStereoLink, 1.0f)) == InjectStatus::Ok);
    const double linked_right = gain_db(fx.settle({loud_left, quiet_right})[1], quiet);

    REQUIRE(inj.inject(immediate(dyn::kStereoLink, 0.0f)) == InjectStatus::Ok);
    const double unlinked_right = gain_db(fx.settle({loud_left, quiet_right})[1], quiet);

    REQUIRE_THAT(unlinked_right, WithinAbs(0.0, 0.2));  // its own channel is quiet
    REQUIRE(linked_right < unlinked_right - 6.0);       // the loud channel pulled it down
}

TEST_CASE("Forge dynamics: injecting lookahead changes reported latency",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    // The node's ceiling is what `prepare()` sized the ring for, so a value
    // beyond it clamps rather than allocating.
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 1.0f)) == InjectStatus::Ok);

    // At ratio 1 with no makeup the node is a pure delay, so an impulse's
    // arrival index IS the latency.
    REQUIRE(inj.inject(immediate(dyn::kLookaheadMs, 2.0f)) == InjectStatus::Ok);
    fx.settle({silence(), silence()}, 4);

    auto impulse = silence();
    impulse[0] = 0.5f;
    const auto first = fx.render({impulse, impulse});
    const auto second = fx.render({silence(), silence()});

    const int expected = static_cast<int>(std::llround(2.0 * kSr / 1000.0));
    std::vector<float> joined = first[0];
    joined.insert(joined.end(), second[0].begin(), second[0].end());
    int index = -1;
    float best = 0.0f;
    for (int i = 0; i < static_cast<int>(joined.size()); ++i) {
        if (std::fabs(joined[static_cast<std::size_t>(i)]) > best) {
            best = std::fabs(joined[static_cast<std::size_t>(i)]);
            index = i;
        }
    }
    REQUIRE(index == expected);
}

TEST_CASE("Forge dynamics: auto-makeup restores level through the graph",
          "[host][baked][param-injection][forge][forge-dynamics]") {
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();
    const float amp = 1.0f;
    const auto tone = sine(amp);

    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, -18.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 4.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kKneeDb, 6.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAttackMs, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kReleaseMs, 50.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kProgramDependent, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 1.0f)) == InjectStatus::Ok);

    REQUIRE_THAT(gain_db(fx.settle({tone, tone}, 128)[0], amp), WithinAbs(0.0, 0.3));
}

TEST_CASE("Forge dynamics: the registry's worst-case gain matches the makeup ceiling",
          "[host][baked][forge][forge-dynamics]") {
    // Series law 8: a tested invariant, not an estimate. Feedforward topology,
    // so the gain computer can only reduce and the bound is exactly the makeup
    // ceiling.
    const double expected =
        std::pow(10.0, pulp::signal::FeedforwardCompressor::kMakeupDbMax / 20.0);
    REQUIRE_THAT(static_cast<double>(dyn::feedforward_compressor_worst_case_gain()),
                 WithinAbs(expected, 1e-4));
}

TEST_CASE("Forge dynamics: the node's process path allocates nothing",
          "[host][baked][forge][forge-dynamics][rt-safety]") {
    auto fx = make_fixture(dyn::make_feedforward_compressor_node());
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    fx.settle({tone, tone}, 8);

    // Buffers and views built outside the probe, which is what
    // `ReusableRenderer` exists for. The fixture's convenience `render()`
    // constructs its own output vectors, so driving it from inside a probe
    // would report the harness's allocations as the node's.
    pulp::test::ReusableRenderer<2> renderer(fx, {tone, tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dyn::kThresholdDb, static_cast<float>(-40 + b)));
        inj.inject(immediate(dyn::kLookaheadMs, 0.25f * static_cast<float>(b % 40)));
        inj.inject(immediate(dyn::kDetectorMode, (b % 2) ? 1.0f : 0.0f));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
