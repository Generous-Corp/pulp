#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <pulp/host/forge_fuzz_catalog.hpp>
#include <pulp/signal/units.hpp>

#include <cmath>
#include <limits>
#include <vector>

namespace fuzz = pulp::host::fuzz;
using Fixture = pulp::test::BakedNodeFixture<1>;
using Catch::Matchers::WithinRel;

TEST_CASE("Forge fuzz: every realization bakes, runs, and declares the contract",
          "[host][baked][forge][forge-fuzz]") {
    for (auto device : {fuzz::Device::germanium, fuzz::Device::silicon}) {
        for (bool oversampled : {false, true}) {
            auto type = fuzz::make_fuzz_node(device, oversampled);
            REQUIRE(type.lowerable);
            REQUIRE(type.baked_params.size() == 6);
            REQUIRE(type.type_id == fuzz::type_id(device, oversampled));
            Fixture fx(type, 48000.0, 128);
            const auto input = pulp::test::sine_block(128, 750.0, 48000.0, 0.4f);
            const auto out = fx.settle({input}, 12).front();
            for (float sample : out) REQUIRE(std::isfinite(sample));
        }
    }
    REQUIRE(fuzz::latency_samples(false) == 0);
    REQUIRE(fuzz::latency_samples(true) > 0);
    REQUIRE(fuzz::make_fuzz_node(fuzz::Device::silicon, false).latency_samples(48000.0) ==
            fuzz::latency_samples(false));
    REQUIRE(fuzz::make_fuzz_node(fuzz::Device::silicon, true).latency_samples(48000.0) ==
            fuzz::latency_samples(true));
}

TEST_CASE("Forge fuzz: seeded drift is injectable and deterministic",
          "[host][baked][param-injection][forge][forge-fuzz][determinism]") {
    const auto render = [](bool drift_enabled) {
        auto type = fuzz::make_fuzz_node(fuzz::Device::germanium, false);
        Fixture fx(type, 48000.0, 128);
        auto injector = fx.claim_injector();
        REQUIRE(injector.inject(pulp::test::immediate(
                    fuzz::kDriftEnabled, drift_enabled ? 1.0f : 0.0f)) ==
                pulp::host::InjectStatus::Ok);
        const auto input = pulp::test::sine_block(128, 750.0, 48000.0, 0.4f);
        return fx.settle({input}, 64).front();
    };
    const auto drift_a = render(true);
    const auto drift_b = render(true);
    REQUIRE(drift_a == drift_b);
    REQUIRE(drift_a != render(false));
}

TEST_CASE("Forge fuzz: non-finite drift injection is finite and uses its default",
          "[host][baked][forge][forge-fuzz][non-finite]") {
    Fixture fx(fuzz::make_fuzz_node(fuzz::Device::germanium, false), 48000.0, 128);
    auto injector = fx.claim_injector();
    REQUIRE(injector.inject(pulp::test::immediate(
                fuzz::kDriftEnabled, std::numeric_limits<float>::quiet_NaN())) ==
            pulp::host::InjectStatus::Ok);
    const auto input = pulp::test::sine_block(128, 750.0, 48000.0, 0.4f);
    const auto output = fx.settle({input}, 16).front();
    for (float sample : output) REQUIRE(std::isfinite(sample));
}

TEST_CASE("Forge fuzz: injection changes the realized circuit and remains RT safe",
          "[host][baked][forge][forge-fuzz][rt-safety]") {
    auto type = fuzz::make_fuzz_node(fuzz::Device::silicon, true);
    Fixture fx(type, 48000.0, 128);
    auto injector = fx.claim_injector();
    const auto input = pulp::test::sine_block(128, 750.0, 48000.0, 0.4f);
    REQUIRE(injector.inject(pulp::test::immediate(fuzz::kFuzz, 0.1f)) == pulp::host::InjectStatus::Ok);
    const auto low = fx.settle({input}, 12).front();
    REQUIRE(injector.inject(pulp::test::immediate(fuzz::kFuzz, 0.95f)) == pulp::host::InjectStatus::Ok);
    const auto high = fx.settle({input}, 12).front();
    REQUIRE(low != high);
    pulp::test::ReusableRenderer<1> renderer(fx, {input});
    pulp::test::RtAllocationProbe probe;
    renderer.render();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Forge fuzz: registry gain is the measured full-grid maximum",
          "[host][baked][forge][forge-fuzz][gain]") {
    constexpr int kPointsPerAxis = 33;
    double measured_max = 0.0;

    for (auto device : {fuzz::Device::germanium, fuzz::Device::silicon}) {
        for (int fi = 0; fi < kPointsPerAxis; ++fi) {
            for (int si = 0; si < kPointsPerAxis; ++si) {
                for (int zi = 0; zi < kPointsPerAxis; ++zi) {
                    pulp::signal::FuzzPair probe;
                    probe.set_device(device);
                    probe.set_fuzz(static_cast<double>(fi) / (kPointsPerAxis - 1));
                    probe.set_bias_starve(static_cast<double>(si) / (kPointsPerAxis - 1));
                    probe.set_source_impedance_kohm(pulp::signal::units::taper_log(
                        static_cast<double>(zi) / (kPointsPerAxis - 1), 0.1, 1000.0));
                    measured_max = std::max(measured_max, probe.loop_gain());
                }
            }
        }
    }

    REQUIRE_THAT(static_cast<double>(fuzz::worst_case_gain()),
                 WithinRel(measured_max, 1e-6));
    REQUIRE(measured_max <= pulp::signal::FuzzPair::kLoopGainCeiling);
}
