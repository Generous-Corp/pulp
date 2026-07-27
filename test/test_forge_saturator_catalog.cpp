#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_saturator_catalog.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace pulp::host;
namespace sat = pulp::host::saturator;
using Catch::Matchers::WithinAbs;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;
constexpr double kToneHz = 750.0;  // 64 samples/period: whole cycles per block

using Fixture = pulp::test::BakedNodeFixture<1>;

Fixture make_fixture(const CustomNodeType& type) { return Fixture(type, kSr, kFrames); }

std::vector<float> sine(float amp) {
    return pulp::test::sine_block(kFrames, kToneHz, kSr, amp);
}

/// Steady-state output of the (mono) node for a repeated input block.
std::vector<float> settle(Fixture& fx, const std::vector<float>& feed, int blocks = 8) {
    return fx.settle({feed}, blocks).front();
}

double harmonic(const std::vector<float>& x, int k) {
    return pulp::test::harmonic_magnitude(x, k, kToneHz, kSr);
}

double third_harmonic_ratio(const std::vector<float>& x) {
    return harmonic(x, 3) / harmonic(x, 1);
}

double max_difference(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double difference = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = std::max(difference, std::abs(static_cast<double>(a[i] - b[i])));
    return difference;
}

float peak(const std::vector<float>& b) {
    float m = 0.0f;
    for (float v : b) m = std::max(m, std::fabs(v));
    return m;
}

}  // namespace

TEST_CASE("Forge saturator: every realization bakes and runs",
          "[host][baked][forge][forge-saturator]") {
    for (auto shape : {sat::Shape::tanh_soft, sat::Shape::atan_soft, sat::Shape::cubic_soft,
                       sat::Shape::sinh_arc}) {
        auto fx = make_fixture(sat::make_saturator_node(shape));
        const auto out = settle(fx, sine(0.5f));
        REQUIRE(peak(out) > 0.0f);
        for (float v : out) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("Forge saturator: every musical tone control is stable and injected",
          "[host][baked][param-injection][forge][forge-saturator]") {
    const auto type = sat::make_saturator_node(sat::Shape::tanh_soft);
    REQUIRE(type.lowerable);
    REQUIRE(type.baked_params.size() == 8);
    const auto has = [&](pulp::state::ParamID id, float lo, float hi, float def) {
        const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                     [=](const auto& p) { return p.id == id; });
        REQUIRE(it != type.baked_params.end());
        REQUIRE(it->min_value == lo);
        REQUIRE(it->max_value == hi);
        REQUIRE(it->default_value == def);
    };
    has(sat::kToneTracking, 0.0f, 1.0f, 1.0f);
    has(sat::kToneDeHz, 0.0f, 8000.0f, 3000.0f);
    has(sat::kPreBoostDb, 0.0f, 18.0f, 9.0f);

    const auto t = sine(0.2f);
    const auto render = [&](float tracking, float de_hz, float boost_db) {
        auto fx = make_fixture(type);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(sat::kDriveDb, 18.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(sat::kTonePreHz, 500.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(sat::kToneTracking, tracking)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(sat::kToneDeHz, de_hz)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(sat::kPreBoostDb, boost_db)) == InjectStatus::Ok);
        return settle(fx, t, 32);
    };

    const auto tracked = render(1.0f, 7000.0f, 18.0f);
    const auto untracked = render(0.0f, 7000.0f, 18.0f);
    REQUIRE(max_difference(tracked, untracked) > 1e-4);

    const auto de_low = render(0.0f, 500.0f, 18.0f);
    const auto de_high = render(0.0f, 7000.0f, 18.0f);
    REQUIRE(max_difference(de_low, de_high) > 1e-4);

    const auto no_boost = render(0.0f, 7000.0f, 0.0f);
    const auto full_boost = render(0.0f, 7000.0f, 18.0f);
    REQUIRE(max_difference(no_boost, full_boost) > 1e-4);
}

TEST_CASE("Forge saturator: hostile non-finite injections fall back to finite defaults",
          "[host][baked][forge][forge-saturator][non-finite]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    auto inj = fx.claim_injector();
    for (auto id : {sat::kToneTracking, sat::kToneDeHz, sat::kPreBoostDb})
        REQUIRE(inj.inject(immediate(id, std::numeric_limits<float>::quiet_NaN())) ==
                InjectStatus::Ok);
    for (float sample : settle(fx, sine(0.4f))) REQUIRE(std::isfinite(sample));
}

TEST_CASE("Forge saturator: injecting drive changes harmonic content",
          "[host][baked][param-injection][forge][forge-saturator]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 0.0f)) == InjectStatus::Ok);
    const double low = third_harmonic_ratio(settle(fx, tone));

    REQUIRE(inj.inject(immediate(sat::kDriveDb, 24.0f)) == InjectStatus::Ok);
    const double high = third_harmonic_ratio(settle(fx, tone));

    REQUIRE(high > low * 3.0);
}

TEST_CASE("Forge saturator alias identities are stable and distinct",
          "[host][forge][forge-saturator][identity]") {
    const auto adaa = sat::make_saturator_node(sat::Shape::tanh_soft, sat::AliasPolicy::adaa);
    const auto x2 = sat::make_saturator_node(sat::Shape::tanh_soft, sat::AliasPolicy::oversample_2x);
    const auto off = sat::make_saturator_node(sat::Shape::tanh_soft, sat::AliasPolicy::off);
    REQUIRE(adaa.type_id == sat::kTanhTypeId);
    REQUIRE(x2.type_id != adaa.type_id);
    REQUIRE(off.type_id != adaa.type_id);
    REQUIRE(off.type_id != x2.type_id);
    REQUIRE(sat::make_saturator_node(sat::Shape::tanh_soft,
                                     sat::AliasPolicy::oversample_2x).type_id == x2.type_id);
}

TEST_CASE("Forge saturator: the drive floor is transparent through the graph",
          "[host][baked][param-injection][forge][forge-saturator]") {
    // The registry's unity-gain claim, measured over the production path rather
    // than over the DSP class.
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);

    const float amp = 0.01f;  // −40 dBFS
    const auto out = settle(fx, sine(amp));
    REQUIRE_THAT(20.0 * std::log10(harmonic(out, 1) / amp), WithinAbs(0.0, 0.05));
}

TEST_CASE("Forge saturator: injecting bias adds even harmonics without DC",
          "[host][baked][param-injection][forge][forge-saturator]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 12.0f)) == InjectStatus::Ok);

    double previous = -1.0;
    for (float bias : {0.0f, 0.2f, 0.5f}) {
        REQUIRE(inj.inject(immediate(sat::kBias, bias)) == InjectStatus::Ok);
        const auto out = settle(fx, tone);
        const double h2 = harmonic(out, 2) / harmonic(out, 1);
        REQUIRE(h2 > previous);
        previous = h2;
    }

    // ...and silence still produces silence, at the largest bias.
    REQUIRE(inj.inject(immediate(sat::kBias, 1.0f)) == InjectStatus::Ok);
    const std::vector<float> silence(static_cast<std::size_t>(kFrames), 0.0f);
    const auto quiet = settle(fx, silence);
    for (float v : quiet) REQUIRE(std::abs(v) <= 1e-6f);
}

TEST_CASE("Forge saturator: injecting mix crossfades dry against wet",
          "[host][baked][param-injection][forge][forge-saturator]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::cubic_soft));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, 24.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(sat::kMix, 1.0f)) == InjectStatus::Ok);
    const double wet = third_harmonic_ratio(settle(fx, tone));

    REQUIRE(inj.inject(immediate(sat::kMix, 0.0f)) == InjectStatus::Ok);
    const auto dry = settle(fx, tone);
    // Fully dry is the input back, unshaped.
    REQUIRE(third_harmonic_ratio(dry) < wet * 0.05);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE_THAT(static_cast<double>(dry[static_cast<std::size_t>(k)]),
                     WithinAbs(static_cast<double>(tone[static_cast<std::size_t>(k)]), 1e-5));
}

TEST_CASE("Forge saturator: injecting output trim scales by exactly that many dB",
          "[host][baked][param-injection][forge][forge-saturator]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.2f);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(sat::kOutputTrimDb, 0.0f)) == InjectStatus::Ok);
    const double unity = harmonic(settle(fx, tone), 1);

    for (float trim : {-12.0f, -6.0f, 6.0f, 12.0f}) {
        REQUIRE(inj.inject(immediate(sat::kOutputTrimDb, trim)) == InjectStatus::Ok);
        const double scaled = harmonic(settle(fx, tone), 1);
        REQUIRE_THAT(20.0 * std::log10(scaled / unity), WithinAbs(trim, 0.05));
    }
}

TEST_CASE("Forge saturator: the tone pair cancels through the graph",
          "[host][baked][param-injection][forge][forge-saturator]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(sat::kDriveDb, -12.0f)) == InjectStatus::Ok);
    const float amp = 0.01f;

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 0.0f)) == InjectStatus::Ok);
    const double flat = harmonic(settle(fx, sine(amp)), 1);

    REQUIRE(inj.inject(immediate(sat::kTonePreHz, 3000.0f)) == InjectStatus::Ok);
    const double bracketed = harmonic(settle(fx, sine(amp), 32), 1);

    REQUIRE_THAT(20.0 * std::log10(bracketed / flat), WithinAbs(0.0, 0.2));
}

TEST_CASE("Forge saturator: the registry's worst-case gain matches the DSP bound",
          "[host][baked][forge][forge-saturator]") {
    // Series law 8: the registry constant must cite an invariant the module's
    // own suite asserts, never an estimate. This is the two agreeing.
    for (auto shape : {sat::Shape::tanh_soft, sat::Shape::atan_soft, sat::Shape::cubic_soft,
                       sat::Shape::sinh_arc}) {
        pulp::signal::Saturator probe;
        probe.set_shape(shape);
        probe.set_bias(1.0);
        REQUIRE_THAT(static_cast<double>(sat::saturator_worst_case_gain(shape)),
                     WithinAbs(probe.worst_case_gain(), 1e-5));
        // Unbiased it is exactly 1.0 for every shape.
        probe.set_bias(0.0);
        REQUIRE_THAT(probe.worst_case_gain(), WithinAbs(1.0, 1e-12));
    }
}

TEST_CASE("Forge saturator: the node's process path allocates nothing",
          "[host][baked][forge][forge-saturator][rt-safety]") {
    auto fx = make_fixture(sat::make_saturator_node(sat::Shape::tanh_soft));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    settle(fx, tone);  // warm every lazily-touched path first

    // Buffers and views are built OUTSIDE the probe, which is what
    // `ReusableRenderer` exists for. The fixture's convenience `render()`
    // constructs its own output vectors, so driving it from inside a probe
    // would report the harness's allocations as the node's.
    pulp::test::ReusableRenderer<1> renderer(fx, {tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(sat::kDriveDb, static_cast<float>(-12 + b)));
        inj.inject(immediate(sat::kBias, 0.02f * static_cast<float>(b)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
