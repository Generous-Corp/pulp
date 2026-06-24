// Unit coverage for the off-RT buffer-assignment pass that the routing executor
// consumes: a per-port scratch-slot layout that reuses slots across
// non-overlapping node lifetimes. These assert the invariants the executor
// depends on (regions in range, a node's own input and output regions never
// overlap, reuse never grows the slot count, one persistent slot per feedback
// edge). End-to-end correctness of the reused layout is proven by the
// SignalGraph bit-exact parity tests in test_graph_executor_routing.cpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/graph/graph_runtime_buffer_assignment.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

#include <array>
#include <span>
#include <vector>

namespace {

using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphRuntimePlan;
using pulp::graph::build_graph_runtime_buffer_assignment;
using pulp::graph::build_graph_runtime_plan;

std::uint32_t total_ports(std::span<const GraphRuntimeNodeSpec> nodes) {
    std::uint32_t t = 0;
    for (const auto& n : nodes) t += n.input_ports + n.output_ports;
    return t;
}

// Invariants every valid assignment must satisfy.
void check_invariants(const GraphRuntimePlan& plan,
                      const pulp::graph::GraphRuntimeBufferAssignment& a) {
    REQUIRE(a.ok);
    REQUIRE(a.nodes.size() == plan.nodes.size());
    for (std::size_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        const auto& s = a.nodes[i];
        // Regions stay within the pool.
        if (node.input_ports > 0) CHECK(s.input_base + node.input_ports <= a.slot_count);
        if (node.output_ports > 0) CHECK(s.output_base + node.output_ports <= a.slot_count);
        // A node's own input and output regions must be disjoint — the binding
        // reads inputs while writing outputs, so aliasing would corrupt it.
        if (node.input_ports > 0 && node.output_ports > 0) {
            const bool disjoint = s.input_base + node.input_ports <= s.output_base ||
                                  s.output_base + node.output_ports <= s.input_base;
            CHECK(disjoint);
        }
    }
}

} // namespace

TEST_CASE("Buffer assignment reuses scratch and never grows past total ports",
          "[graph][buffer-assignment]") {
    // Linear: AudioInput(0->2) -> gain(2->2) -> AudioOutput(2->0).
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 2, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{2, 0, 3, 0}, GraphRuntimeConnectionSpec{2, 1, 3, 1},
    };
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);

    // Reuse never grows past the unique-slot upper bound, and a serial chain
    // actually recycles (the output node's input region reuses freed scratch).
    CHECK(a.slot_count <= total_ports(nodes));   // <= 8
    CHECK(a.slot_count < total_ports(nodes));    // reuse happened
}

TEST_CASE("Buffer assignment reuse scales sub-linearly on a long chain",
          "[graph][buffer-assignment]") {
    // A long unbranched stereo chain: input -> g1 -> g2 -> ... -> output.
    // Without reuse this is 2*(N) ports; with reuse only a few live regions
    // exist at once, so slot_count stays small and constant-ish.
    constexpr int kStages = 12;
    std::vector<GraphRuntimeNodeSpec> nodes;
    nodes.push_back({1, GraphRuntimeNodeKind::AudioInput, 0, 2});
    for (int i = 0; i < kStages; ++i) {
        nodes.push_back({static_cast<pulp::graph::NodeId>(2 + i),
                         GraphRuntimeNodeKind::Processor, 2, 2});
    }
    nodes.push_back({static_cast<pulp::graph::NodeId>(2 + kStages),
                     GraphRuntimeNodeKind::AudioOutput, 2, 0});

    std::vector<GraphRuntimeConnectionSpec> conns;
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
        conns.push_back({nodes[i].id, 0, nodes[i + 1].id, 0});
        conns.push_back({nodes[i].id, 1, nodes[i + 1].id, 1});
    }
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);

    CHECK(a.slot_count < total_ports(nodes));
    // Only a couple of stereo regions are live at any point in a serial chain,
    // so the footprint is a small constant regardless of chain length.
    CHECK(a.slot_count <= 8);
}

TEST_CASE("Buffer assignment appends one previous-block slot per feedback edge",
          "[graph][buffer-assignment][feedback]") {
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true, /*event=*/false},
    };
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);
    CHECK(a.has_feedback);
    REQUIRE(a.feedback_prev_slot.size() == plan.plan.connections.size());

    std::uint32_t feedback_slots = 0;
    for (std::size_t i = 0; i < plan.plan.connections.size(); ++i) {
        if (plan.plan.connections[i].feedback) {
            CHECK(a.feedback_prev_slot[i] != pulp::graph::kGraphRuntimeNoSlot);
            CHECK(a.feedback_prev_slot[i] < a.slot_count);
            ++feedback_slots;
        } else {
            CHECK(a.feedback_prev_slot[i] == pulp::graph::kGraphRuntimeNoSlot);
        }
    }
    CHECK(feedback_slots == 1);
}

TEST_CASE("Buffer assignment of an empty plan is valid and empty",
          "[graph][buffer-assignment]") {
    auto plan = build_graph_runtime_plan(
        std::span<const GraphRuntimeNodeSpec>{},
        std::span<const GraphRuntimeConnectionSpec>{});
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    CHECK(a.ok);
    CHECK(a.slot_count == 0);
    CHECK(a.nodes.empty());
}
