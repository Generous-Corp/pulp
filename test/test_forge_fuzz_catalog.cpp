#include <catch2/catch_test_macros.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"
#include <pulp/host/forge_fuzz_catalog.hpp>

#include <cmath>
#include <vector>

namespace fuzz = pulp::host::fuzz;
using Fixture = pulp::test::BakedNodeFixture<1>;

TEST_CASE("Forge fuzz: every realization bakes, runs, and declares the contract",
          "[host][baked][forge][forge-fuzz]") {
    for (auto device : {fuzz::Device::germanium, fuzz::Device::silicon}) {
        for (bool oversampled : {false, true}) {
            auto type = fuzz::make_fuzz_node(device, oversampled);
            REQUIRE(type.lowerable);
            REQUIRE(type.baked_params.size() == 5);
            REQUIRE(type.type_id == fuzz::type_id(device, oversampled));
            Fixture fx(type, 48000.0, 128);
            const auto input = pulp::test::sine_block(128, 750.0, 48000.0, 0.4f);
            const auto out = fx.settle({input}, 12).front();
            for (float sample : out) REQUIRE(std::isfinite(sample));
        }
    }
    REQUIRE(fuzz::latency_samples(false) == 0);
    REQUIRE(fuzz::latency_samples(true) > 0);
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
