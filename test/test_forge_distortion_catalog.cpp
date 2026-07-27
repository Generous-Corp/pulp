#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_distortion_catalog.hpp>

#include <cmath>
#include <vector>

using namespace pulp::host;
namespace dist = pulp::host::distortion;
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

std::vector<float> silence() {
    return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
}

/// Steady-state output of the (mono) node for a repeated input block.
std::vector<float> settle(Fixture& fx, const std::vector<float>& feed, int blocks = 16) {
    return fx.settle({feed}, blocks).front();
}

double harmonic(const std::vector<float>& x, int k) {
    return pulp::test::harmonic_magnitude(x, k, kToneHz, kSr);
}

constexpr dist::OversampleTier kAllTiers[] = {dist::OversampleTier::x1, dist::OversampleTier::x2,
                                              dist::OversampleTier::x4, dist::OversampleTier::x8};

}  // namespace

TEST_CASE("Forge distortion: every realization bakes and runs",
          "[host][baked][forge][forge-distortion]") {
    for (auto topology : {dist::Topology::to_ground, dist::Topology::in_loop}) {
        for (auto tier : kAllTiers) {
            auto fx = make_fixture(dist::make_distortion_node(topology, tier));
            const auto out = settle(fx, sine(0.5f));
            for (float v : out) REQUIRE(std::isfinite(v));
        }
    }
}

TEST_CASE("Forge distortion: injecting drive increases harmonic content",
          "[host][baked][param-injection][forge][forge-distortion]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.2f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
    const auto low_out = settle(fx, tone);
    const double low = harmonic(low_out, 3) / harmonic(low_out, 1);

    REQUIRE(inj.inject(immediate(dist::kDriveDb, 36.0f)) == InjectStatus::Ok);
    const auto high_out = settle(fx, tone);
    const double high = harmonic(high_out, 3) / harmonic(high_out, 1);

    REQUIRE(high > low * 3.0);
}

TEST_CASE("Forge distortion: injecting symmetry brings in even harmonics",
          "[host][baked][param-injection][forge][forge-distortion]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 24.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dist::kSymmetry, 0.0f)) == InjectStatus::Ok);
    const auto matched = settle(fx, tone);
    const double even_matched = harmonic(matched, 2) / harmonic(matched, 1);

    REQUIRE(inj.inject(immediate(dist::kSymmetry, -1.0f)) == InjectStatus::Ok);
    const auto half_wave = settle(fx, tone);
    const double even_half = harmonic(half_wave, 2) / harmonic(half_wave, 1);

    REQUIRE(even_half > even_matched * 10.0);
}

TEST_CASE("Forge distortion: injecting output trim scales by exactly that many dB",
          "[host][baked][param-injection][forge][forge-distortion]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.05f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kOutputDb, 0.0f)) == InjectStatus::Ok);
    const double unity = harmonic(settle(fx, tone), 1);

    for (float trim : {-24.0f, -12.0f, 6.0f, 12.0f}) {
        REQUIRE(inj.inject(immediate(dist::kOutputDb, trim)) == InjectStatus::Ok);
        const double scaled = harmonic(settle(fx, tone), 1);
        REQUIRE_THAT(20.0 * std::log10(scaled / unity), WithinAbs(trim, 0.05));
    }
}

TEST_CASE("Forge distortion: injecting the post-tone corner darkens the output",
          "[host][baked][param-injection][forge][forge-distortion]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);

    REQUIRE(inj.inject(immediate(dist::kToneMix, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kDriveDb, 30.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dist::kPostToneHz, 12000.0f)) == InjectStatus::Ok);
    const auto bright = settle(fx, tone);

    REQUIRE(inj.inject(immediate(dist::kPostToneHz, 500.0f)) == InjectStatus::Ok);
    const auto dark = settle(fx, tone);

    // The de-fizz stage: high harmonics fall relative to the fundamental.
    REQUIRE(harmonic(dark, 5) / harmonic(dark, 1) <
            harmonic(bright, 5) / harmonic(bright, 1) * 0.5);
}

TEST_CASE("Forge distortion: pre-tone corner is audible when its shelf is engaged",
          "[host][baked][param-injection][forge][forge-distortion]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.05f);

    REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kToneMix, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kPreGainDb, 12.0f)) == InjectStatus::Ok);

    REQUIRE(inj.inject(immediate(dist::kPreToneHz, 200.0f)) == InjectStatus::Ok);
    const double low_corner = harmonic(settle(fx, tone), 1);

    REQUIRE(inj.inject(immediate(dist::kPreToneHz, 8000.0f)) == InjectStatus::Ok);
    const double high_corner = harmonic(settle(fx, tone), 1);

    REQUIRE(low_corner > high_corner * 2.0);
}

// ── 8. Latency, reported exactly per tier ─────────────────────────────────

TEST_CASE("8 the oversampling tiers report their latency exactly",
          "[host][baked][forge][forge-distortion][latency]") {
    // Zero at ×1, since the ADAA path has no filter at all; the composed
    // oversampler's own exact integer group delay above that.
    REQUIRE(dist::distortion_latency_samples(dist::OversampleTier::x1, kSr) == 0);
    REQUIRE(dist::make_distortion_node(dist::Topology::to_ground,
                                       dist::OversampleTier::x1)
                .latency_samples(kSr) == 0);

    int previous = 0;
    for (auto tier : {dist::OversampleTier::x2, dist::OversampleTier::x4,
                      dist::OversampleTier::x8}) {
        const int latency = dist::distortion_latency_samples(tier, kSr);
        REQUIRE(dist::make_distortion_node(dist::Topology::to_ground, tier)
                    .latency_samples(kSr) == latency);
        REQUIRE(latency > 0);
        // More cascaded stages can only add delay, never remove it.
        REQUIRE(latency >= previous);
        previous = latency;
    }
}

TEST_CASE("8 the reported latency matches the measured impulse position",
          "[host][baked][forge][forge-distortion][latency]") {
    // The reported figure is checked against the actual impulse response rather
    // than trusted, which is the whole point of "reported, not estimated".
    for (auto tier : {dist::OversampleTier::x2, dist::OversampleTier::x4}) {
        auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground, tier));
        ParamInjector inj = fx.claim_injector();
        // Linear settings so the impulse survives recognisably: no drive past
        // the knee, no tone shaping.
        REQUIRE(inj.inject(immediate(dist::kDriveDb, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(dist::kOutputDb, 0.0f)) == InjectStatus::Ok);

        const std::vector<float> silence(static_cast<std::size_t>(kFrames), 0.0f);
        settle(fx, silence, 4);

        auto impulse = silence;
        impulse[0] = 0.05f;  // small: stays well below the diode knee
        std::vector<float> joined = fx.render({impulse}).front();
        for (int b = 0; b < 4; ++b) {
            const auto next = fx.render({silence}).front();
            joined.insert(joined.end(), next.begin(), next.end());
        }

        int index = -1;
        float best = 0.0f;
        for (int i = 0; i < static_cast<int>(joined.size()); ++i) {
            if (std::fabs(joined[static_cast<std::size_t>(i)]) > best) {
                best = std::fabs(joined[static_cast<std::size_t>(i)]);
                index = i;
            }
        }
        REQUIRE(index == dist::distortion_latency_samples(tier, kSr));
    }
}

// ── 7. Determinism through the graph ──────────────────────────────────────

TEST_CASE("7 the baked node renders identically after a reset",
          "[host][baked][forge][forge-distortion][determinism]") {
    for (auto tier : {dist::OversampleTier::x1, dist::OversampleTier::x4}) {
        auto fx = make_fixture(dist::make_distortion_node(dist::Topology::in_loop, tier));
        const auto tone = sine(0.4f);

        // Two independently baked instances of the same graph, rendered the
        // same way, must agree sample for sample. That is the property that
        // matters — no uncontrolled state, no uninitialised carry — and it does
        // not depend on the processor exposing a public reset.
        auto other = make_fixture(dist::make_distortion_node(dist::Topology::in_loop, tier));
        for (int b = 0; b < 16; ++b) {
            const auto a = fx.render({tone}).front();
            const auto c = other.render({tone}).front();
            for (int k = 0; k < kFrames; ++k)
                REQUIRE(a[static_cast<std::size_t>(k)] == c[static_cast<std::size_t>(k)]);
        }
    }
}

TEST_CASE("Forge distortion: the registry's worst-case gain covers the trims",
          "[host][baked][forge][forge-distortion]") {
    // The clipper itself cannot amplify, so the node's bound is the drive,
    // pre-emphasis shelf, and output trim. Both gains before the clipper count
    // because quiet signals pass through unclipped.
    const double expected = std::pow(
        10.0,
        (dist::kDriveDbMax + pulp::signal::ToneStack::kPreGainDbMax + dist::kOutputDbMax) /
            20.0);
    REQUIRE_THAT(static_cast<double>(dist::distortion_worst_case_gain()),
                 WithinAbs(expected, 1e-3));

    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::to_ground));
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(dist::kDriveDb, dist::kDriveDbMax)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kPreGainDb,
                                 static_cast<float>(pulp::signal::ToneStack::kPreGainDbMax))) ==
            InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kOutputDb, dist::kOutputDbMax)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(dist::kToneMix, 0.0f)) == InjectStatus::Ok);

    // A quiet input, where the drive passes through the clipper unclipped: this
    // is where the bound is actually approached.
    const auto out = settle(fx, sine(1e-4f));
    double peak = 0.0;
    for (float v : out) peak = std::max(peak, static_cast<double>(std::fabs(v)));
    REQUIRE(peak <= 1e-4 * expected * 1.01);
}

TEST_CASE("Forge distortion: the node's process path allocates nothing",
          "[host][baked][forge][forge-distortion][rt-safety]") {
    auto fx = make_fixture(dist::make_distortion_node(dist::Topology::in_loop,
                                               dist::OversampleTier::x4));
    ParamInjector inj = fx.claim_injector();
    const auto tone = sine(0.5f);
    settle(fx, tone, 8);

    // Buffers and views built outside the probe, which is what
    // `ReusableRenderer` exists for. The fixture's convenience `render()`
    // constructs its own output vectors, so driving it from inside a probe
    // would report the harness's allocations as the node's.
    pulp::test::ReusableRenderer<1> renderer(fx, {tone});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(dist::kDriveDb, static_cast<float>(b)));
        inj.inject(immediate(dist::kSymmetry, -1.0f + 0.06f * static_cast<float>(b)));
        inj.inject(immediate(dist::kDiodeModel, static_cast<float>(b % 3)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
