#include <pulp/host/forge_space_catalog.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace {

using ConvolutionInstance = pulp::host::space::convolution::Instance;
using AmbienceInstance = pulp::host::space::nonlin_ambience::Instance;
using CabinetInstance = pulp::host::space::cabinet::Instance;

static_assert(std::is_default_constructible_v<ConvolutionInstance>);
static_assert(std::is_default_constructible_v<AmbienceInstance>);
static_assert(std::is_default_constructible_v<CabinetInstance>);

using ConvolutionFactory = pulp::host::CustomNodeType (*) (
    pulp::host::space::convolution::ImpulseResponse,
    pulp::host::space::convolution::IrPolicy);
static_assert(std::is_same_v<decltype(static_cast<ConvolutionFactory>(
                                  &pulp::host::space::convolution::make_convolution_reverb_node)),
                             ConvolutionFactory>);

using AmbienceFactory = pulp::host::CustomNodeType (*) (std::uint32_t, double);
static_assert(std::is_same_v<decltype(static_cast<AmbienceFactory>(
                                  &pulp::host::space::nonlin_ambience::make_nonlin_ambience_node)),
                             AmbienceFactory>);

TEST_CASE("Forge space compatibility helpers remain public") {
    float last = 0.0f;
    int calls = 0;
    pulp::host::space::nonlin_ambience::forward_if_changed(
        last, 0.0f, [&](float) { ++calls; });
    pulp::host::space::nonlin_ambience::forward_if_changed(
        last, 1.0f, [&](float) { ++calls; });
    REQUIRE(calls == 1);
    REQUIRE(last == 1.0f);

#if defined(PULP_HOST_ENABLE_GPU_CONVOLUTION)
    REQUIRE(pulp::host::space::convolution::gpu_internal_block_size(0) == 0);
    REQUIRE(pulp::host::space::convolution::gpu_internal_block_size(192) == 256);
    static_assert(std::is_default_constructible_v<
                  pulp::host::space::convolution::GpuInstance>);
#endif
}

}  // namespace
