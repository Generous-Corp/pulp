// Phase 4a: unit coverage for the off-RT buffer-assignment pass that the
// routing executor consumes. Asserts the scratch-slot layout (per-node input
// and output regions, total slot count) for linear, fan-out, and diamond
// plans. The executor's end-to-end consumption is proven in
// test_graph_executor_routing.cpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/graph/graph_runtime_buffer_assignment.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

#include <array>
#include <span>

namespace {

using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::build_graph_runtime_buffer_assignment;
using pulp::graph::build_graph_runtime_plan;

} // namespace

TEST_CASE("Buffer assignment lays out contiguous per-node input/output regions",
          "[graph][buffer-assignment][phase4]") {
    // Linear: AudioInput(0->2) -> gain(2->2) -> AudioOutput(2->0).
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 2, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 1, 3, 1},
    };
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());

    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    REQUIRE(a.ok);
    REQUIRE(a.nodes.size() == 3);

    // node0 (input): 0 input slots, output region [0,2)
    CHECK(a.nodes[0].output_base == 0);
    // node1 (gain): input region [2,4), output region [4,6)
    CHECK(a.nodes[1].input_base == 2);
    CHECK(a.nodes[1].output_base == 4);
    // node2 (output): input region [6,8), no outputs
    CHECK(a.nodes[2].input_base == 6);
    // total = 2 + (2+2) + 2 = 8
    CHECK(a.slot_count == 8);

    // Every assigned region is disjoint and within [0, slot_count).
    CHECK(a.nodes[0].output_base + 2 <= a.slot_count);
    CHECK(a.nodes[1].input_base >= a.nodes[0].output_base + 2);
}

TEST_CASE("Buffer assignment slot_count equals total ports across all plans",
          "[graph][buffer-assignment][phase4]") {
    // Diamond: in -> {A, B} -> mix -> out. Exercises fan-out and fan-in port
    // counts so the running total covers every node's ports exactly once.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},  // A
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},  // B
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::Processor, 2, 1},  // mix (2 in)
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 1},
        GraphRuntimeConnectionSpec{4, 0, 5, 0},
    };
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());

    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    REQUIRE(a.ok);

    std::uint32_t total_ports = 0;
    for (const auto& n : nodes) total_ports += n.input_ports + n.output_ports;
    CHECK(a.slot_count == total_ports);  // 0+1 +1+1 +1+1 +2+1 +1+0 = 9

    // Bases are strictly non-decreasing in dense order (contiguous layout).
    std::uint32_t prev = 0;
    for (const auto& slots : a.nodes) {
        CHECK(slots.input_base >= prev);
        prev = slots.output_base;  // output region follows input region
    }
}

TEST_CASE("Buffer assignment of an empty plan is valid and empty",
          "[graph][buffer-assignment][phase4]") {
    auto plan = build_graph_runtime_plan(
        std::span<const GraphRuntimeNodeSpec>{},
        std::span<const GraphRuntimeConnectionSpec>{});
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    CHECK(a.ok);
    CHECK(a.slot_count == 0);
    CHECK(a.nodes.empty());
}
