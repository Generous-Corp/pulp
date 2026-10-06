#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/bounded_delay_descriptor.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <vector>

using namespace pulp::host;

TEST_CASE("DSPX-02 bounded integer descriptor is exact and graph reachable",
          "[dspx-02][descriptor][graph]") {
    SignalGraph graph;
    BoundedDelayDescriptor descriptor;
    descriptor.delay_samples = 2;
    descriptor.maximum_delay_samples = 8;
    descriptor.state_budget_bytes = 64;

    REQUIRE(descriptor.validate() == BoundedDelayRefusalReason::None);
    BoundedDelayRegistration registration;
    REQUIRE(make_bounded_delay_registration(descriptor, registration) ==
            BoundedDelayRefusalReason::None);
    CHECK(registration.custom_type.type_id == BoundedDelayDescriptor::kTypeId);
    CHECK(registration.custom_type.version == BoundedDelayDescriptor::kVersion);
    REQUIRE(register_bounded_delay(graph, descriptor) == BoundedDelayRefusalReason::None);
    const auto* type =
        graph.custom_node_type(BoundedDelayDescriptor::kTypeId, BoundedDelayDescriptor::kVersion);
    REQUIRE(type != nullptr);
    CHECK(type->num_input_ports == 1);
    CHECK(type->num_output_ports == 1);
    const auto input = graph.add_input_node(1, "In");
    const auto delay = graph.add_custom_node(BoundedDelayDescriptor::kTypeId,
                                             BoundedDelayDescriptor::kVersion, "Delay");
    const auto output = graph.add_output_node(1, "Out");
    REQUIRE(input != 0);
    REQUIRE(delay != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(input, 0, delay, 0));
    REQUIRE(graph.connect(delay, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> in{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> out(4, -1.0f);
    std::array<const float*, 1> input_channels{in.data()};
    std::array<float*, 1> output_channels{out.data()};
    pulp::audio::BufferView<const float> input_view(input_channels.data(), 1, 4);
    pulp::audio::BufferView<float> output_view(output_channels.data(), 1, 4);
    graph.process(output_view, input_view, 4);
    CHECK(out == std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f});
}

TEST_CASE("DSPX-02 bounded delay refuses unsupported kinds and unsafe bounds",
          "[dspx-02][descriptor][negative]") {
    BoundedDelayDescriptor descriptor;
    descriptor.kind = BoundedDelayKind::Fractional;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::UnsupportedKind);

    descriptor = {};
    descriptor.interpolation = BoundedDelayInterpolation::Linear;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::UnsupportedInterpolation);

    descriptor = {};
    descriptor.minimum_delay_samples = 4;
    descriptor.maximum_delay_samples = 2;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::InvalidDelayBounds);

    descriptor = {};
    descriptor.delay_samples = 3;
    descriptor.maximum_delay_samples = 2;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::DelayOutOfBounds);

    descriptor = {};
    descriptor.tap_count = 2;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::InvalidTapCount);

    descriptor = {};
    descriptor.delay_samples = 32;
    descriptor.state_budget_bytes = 32;
    CHECK(descriptor.validate() == BoundedDelayRefusalReason::StateBudgetExceeded);

    SignalGraph graph;
    descriptor = {};
    descriptor.kind = BoundedDelayKind::MultiTap;
    CHECK(register_bounded_delay(graph, descriptor) == BoundedDelayRefusalReason::UnsupportedKind);
    CHECK(graph.custom_node_type(BoundedDelayDescriptor::kTypeId,
                                 BoundedDelayDescriptor::kVersion) == nullptr);
}
