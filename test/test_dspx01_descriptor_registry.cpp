#include <catch2/catch_test_macros.hpp>

#include <pulp/host/sample_region_proof.hpp>
#include <pulp/host/signal_graph.hpp>

#include <cstdint>
#include <limits>

using namespace pulp::host;

namespace {

CustomNodeType valid_custom_type() {
    CustomNodeType type;
    type.type_id = "pulp.dspx01.contract";
    type.version = 2;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "DSPX-01 contract";
    type.process = [](auto&, const auto&, int) {};
    return type;
}

void scalar_copy(void*, const PreparedSampleKernelConfig&, const SampleFrameContext&,
                 const float* inputs, float* outputs) noexcept {
    outputs[0] = inputs[0];
}

SampleKernelDescriptor valid_sample_kernel() {
    SampleKernelDescriptor descriptor;
    descriptor.type_id = "pulp.dspx01.scalar";
    descriptor.version = 1;
    descriptor.num_input_ports = 1;
    descriptor.num_output_ports = 1;
    descriptor.authored_config_kind = SampleKernelConfigKind::None;
    descriptor.metadata.category = "dspx01-test";
    descriptor.process = scalar_copy;
    return descriptor;
}

} // namespace

TEST_CASE("DSPX-01 custom descriptors register and reach graph topology",
          "[dspx-01][host][descriptor][registry]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(valid_custom_type()));

    const auto* resolved = graph.custom_node_type("pulp.dspx01.contract", 2);
    REQUIRE(resolved != nullptr);
    CHECK(resolved->version == 2);
    CHECK(graph.custom_node_type_count() == 1);

    const auto node = graph.add_custom_node("pulp.dspx01.contract", 2, "contract");
    CHECK(node != 0);

    const auto metadata = graph.custom_node_types();
    REQUIRE(metadata.size() == 1);
    CHECK(metadata.front().type_id == "pulp.dspx01.contract");
    CHECK(metadata.front().version == 2);
}

TEST_CASE("DSPX-01 invalid descriptor combinations refuse without registry mutation",
          "[dspx-01][host][descriptor][negative]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(valid_custom_type()));

    auto invalid = valid_custom_type();
    invalid.type_id.clear();
    CHECK_FALSE(graph.register_custom_node_type(std::move(invalid)));

    auto incomplete_bake = valid_custom_type();
    incomplete_bake.lowerable = true;
    incomplete_bake.create = [] { return static_cast<void*>(nullptr); };
    incomplete_bake.destroy = [](void*) {};
    incomplete_bake.baked_params = {CustomNodeBakedParam{1, 0.0f, 1.0f, 0.5f}};
    CHECK_FALSE(graph.register_custom_node_type(std::move(incomplete_bake)));

    CHECK(graph.custom_node_type_count() == 1);
    CHECK(graph.custom_node_type("pulp.dspx01.contract", 2) != nullptr);
}

TEST_CASE("DSPX-01 scalar descriptor identity is valid and hostile parser shapes refuse",
          "[dspx-01][host][descriptor][sample-region][negative]") {
    CHECK(valid_sample_kernel().is_valid_registration());

    SampleRegionParserShape hostile;
    hostile.regions = std::numeric_limits<std::uint64_t>::max();
    const auto proof = prove_sample_region_parser_shape(hostile);
    CHECK_FALSE(proof.accepted);
    CHECK((proof.reason == SampleRegionRefusalReason::RegionLimitExceeded ||
           proof.reason == SampleRegionRefusalReason::ArithmeticOverflow));
}
