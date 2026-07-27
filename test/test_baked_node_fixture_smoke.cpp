#include "harness/baked_node_fixture.hpp"
#include <pulp/host/forge_saturator_catalog.hpp>
#include <pulp/host/forge_dynamics_catalog.hpp>
namespace sat = pulp::host::saturator;
namespace dyn = pulp::host::dynamics;
TEST_CASE("fixture smoke", "[harness]") {
    pulp::test::BakedNodeFixture<1> mono(sat::make_saturator_node(sat::Shape::tanh_soft), 48000.0, 128);
    auto inj = mono.claim_injector();
    REQUIRE(inj.inject(pulp::test::immediate(sat::kDriveDb, 12.0f)) == pulp::host::InjectStatus::Ok);
    const auto tone = pulp::test::sine_block(128, 750.0, 48000.0, 0.5f);
    const auto out = mono.settle({tone});
    REQUIRE(pulp::test::harmonic_magnitude(out[0], 1, 750.0, 48000.0) > 0.0);

    pulp::test::BakedNodeFixture<2> stereo(dyn::make_feedforward_compressor_node(), 48000.0, 128);
    const auto sout = stereo.settle({tone, tone});
    REQUIRE(sout.size() == 2);

    pulp::test::ReusableRenderer<1> r(mono, {tone});
    r.render();
    REQUIRE(r.output(0).size() == 128);
}
