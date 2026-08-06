#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_dynamics_catalog.hpp>

#include <array>
#include <cmath>
#include <numbers>
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

double sine_amplitude(const std::vector<float>& signal, double cycles_per_sample) {
    double ss = 0.0, cc = 0.0, sc = 0.0, sy = 0.0, cy = 0.0;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double angle = 2.0 * std::numbers::pi * cycles_per_sample * static_cast<double>(i);
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        ss += s * s;
        cc += c * c;
        sc += s * c;
        sy += s * signal[i];
        cy += c * signal[i];
    }
    const double determinant = ss * cc - sc * sc;
    const double a = (sy * cc - cy * sc) / determinant;
    const double b = (cy * ss - sy * sc) / determinant;
    return std::hypot(a, b);
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

TEST_CASE("Forge dynamics: true-peak limiting is a registered stereo realization",
          "[host][baked][forge][forge-dynamics][latency]") {
    constexpr std::array<double, 6> rates{8000.0, 44100.0, 48000.0,
                                          96000.0, 192000.0, 384000.0};
    constexpr std::array<float, 3> lookaheads{0.0f, 5.0f, 10.0f};
    std::vector<float> loud(kFrames);
    for (int frame = 0; frame < kFrames; ++frame)
        loud[static_cast<std::size_t>(frame)] =
            1.5f * std::sin(2.0 * std::numbers::pi * 0.4 * static_cast<double>(frame));

    for (const double rate : rates) {
        for (const float lookahead : lookaheads) {
            const auto linked = dyn::true_peak::make_node(lookahead, true);
            REQUIRE(linked.type_id.starts_with("dynamics.true_peak_limiter.la_"));
            REQUIRE(linked.type_id.ends_with(".linked"));
            REQUIRE(linked.num_input_ports == 2);
            REQUIRE(linked.num_output_ports == 2);
            REQUIRE(linked.latency_samples(rate) ==
                    64 + static_cast<int>(std::ceil(lookahead * 0.001 * rate)));

            Fixture fx(linked, rate, kFrames);
            ParamInjector injector = fx.claim_injector();
            REQUIRE(injector.inject(immediate(dyn::true_peak::kCeilingDbtp, -6.0f)) ==
                    InjectStatus::Ok);
            REQUIRE(injector.inject(immediate(dyn::true_peak::kReleaseMs, 50.0f)) ==
                    InjectStatus::Ok);
            const auto output = fx.settle({loud, loud}, 40);
            const double ceiling = std::pow(10.0, -6.0 / 20.0);
            REQUIRE(peak(output[0]) > 0.0f);
            REQUIRE(sine_amplitude(output[0], 0.4) <= ceiling);
            REQUIRE(output[0] == output[1]);
        }
    }

    const auto independent = dyn::true_peak::make_node(5.0f, false);
    REQUIRE(independent.type_id != dyn::true_peak::make_node(5.0f, true).type_id);

    const auto descriptor = dyn::true_peak::descriptor();
    REQUIRE(descriptor.key == "true_peak_limiter");
    REQUIRE(descriptor.realizations.size() == 4);
    REQUIRE(descriptor.params.size() == 2);
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

TEST_CASE("Forge dynamics: feedforward lookahead is a stable realization axis",
          "[host][baked][forge][forge-dynamics][latency]") {
    const auto zero = dyn::make_feedforward_compressor_node(0.0f);
    const auto three = dyn::make_feedforward_compressor_node(3.0f);
    const auto ten = dyn::make_feedforward_compressor_node(10.0f);

    REQUIRE(zero.type_id == dyn::kFeedforwardCompressorTypeId);
    REQUIRE(three.type_id != zero.type_id);
    REQUIRE(ten.type_id != three.type_id);
    REQUIRE(three.type_id == dyn::make_feedforward_compressor_node(3.0f).type_id);
    REQUIRE(dyn::make_feedforward_compressor_node(-1.0f).type_id == zero.type_id);
    REQUIRE(dyn::make_feedforward_compressor_node(99.0f).type_id == ten.type_id);
    for (const auto* realization : {&zero, &three, &ten}) {
        REQUIRE(std::none_of(realization->baked_params.begin(),
                             realization->baked_params.end(),
                             [](const CustomNodeBakedParam& p) { return p.id == 8; }));
        REQUIRE(realization->latency_samples);
    }
    REQUIRE(zero.latency_samples(kSr) == 0);
    REQUIRE(three.latency_samples(kSr) == 144);
    REQUIRE(ten.latency_samples(kSr) == 480);

    // At ratio 1 with no makeup the zero-lookahead realization still passes an
    // impulse at sample zero, preserving its historical behavior and identity.
    const auto type = zero;
    auto fx = make_fixture(type);
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(dyn::kAutoMakeup, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kThresholdDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dyn::kRatio, 1.0f)) == InjectStatus::Ok);

    fx.settle({silence(), silence()}, 4);

    auto impulse = silence();
    impulse[0] = 0.5f;
    const auto first = fx.render({impulse, impulse});
    const auto second = fx.render({silence(), silence()});

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
    REQUIRE(index == 0);
}

TEST_CASE("Forge dynamics: 0, 3, and 10 ms feedforward factories prepare and run",
          "[host][baked][forge][forge-dynamics][latency]") {
    const auto tone = sine(0.25f);
    for (float lookahead_ms : {0.0f, 3.0f, 10.0f}) {
        auto fx = make_fixture(dyn::make_feedforward_compressor_node(lookahead_ms));
        const auto out = fx.settle({tone, tone}, 8);
        for (const auto& channel : out)
            for (float sample : channel) REQUIRE(std::isfinite(sample));
    }
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
        inj.inject(immediate(dyn::kDetectorMode, (b % 2) ? 1.0f : 0.0f));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
TEST_CASE("Forge VCA realization identity is normalized and collision-free",
          "[host][forge][forge-dynamics][identity]") {
    const auto base = dyn::vca::make_vca_compressor_node();
    REQUIRE(base.type_id == dyn::vca::kTypeId);
    REQUIRE(dyn::vca::make_vca_compressor_node().type_id == base.type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(2.0f).type_id != base.type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(2.0f).type_id ==
            dyn::vca::make_vca_compressor_node(2.0f).type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(2.0f, 3.0).type_id !=
            dyn::vca::make_vca_compressor_node(2.0f, 4.0).type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(-1.0f).type_id == base.type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(99.0f).type_id ==
            dyn::vca::make_vca_compressor_node(
                static_cast<float>(dyn::vca::Comp::kLookaheadMsMax)).type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(2.0f, -99.0).type_id ==
            dyn::vca::make_vca_compressor_node(
                2.0f, dyn::vca::Comp::kRatioKMin).type_id);
    REQUIRE(dyn::vca::make_vca_compressor_node(2.0f, 99.0).type_id ==
            dyn::vca::make_vca_compressor_node(
                2.0f, dyn::vca::Comp::kRatioKMax).type_id);
}
