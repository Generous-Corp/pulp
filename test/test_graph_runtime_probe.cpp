#include <catch2/catch_test_macros.hpp>
#include <pulp/graph/graph_runtime_probe.hpp>

TEST_CASE("GraphRuntimeProbe keeps a bounded attached edge sample ring",
          "[graph][graph-runtime][probe]") {
    pulp::graph::GraphRuntimeProbe<2> probe;
    pulp::graph::GraphRuntimeProbeSample sample{};

    REQUIRE_FALSE(probe.try_push({}));
    probe.attach();
    REQUIRE(probe.try_push(
        {.timestamp_samples = 64, .edge_index = 3, .frames = 64, .peak = 0.5f, .rms = 0.25f}));
    REQUIRE(probe.try_pop(sample));
    CHECK(sample.sequence == 0);
    CHECK(sample.edge_index == 3);
    CHECK(sample.frames == 64);
    REQUIRE_FALSE(probe.try_pop(sample));
    REQUIRE(probe.try_push({}));
    REQUIRE(probe.try_push({}));
    REQUIRE_FALSE(probe.try_push({}));
    CHECK(probe.dropped() == 1);
    probe.detach();
    REQUIRE_FALSE(probe.try_push({}));
}
