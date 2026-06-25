// Unit coverage for the off-RT levelization pass that a future static-multicore
// executor consumes: each node's parallel LEVEL (longest dependency chain) and a
// CSR grouping of nodes by level. These assert the core invariants — every
// non-feedback edge runs strictly lower->higher level, feedback edges never
// raise a level, same-level nodes are independent, and the CSR grouping is a
// consistent partition of all nodes.

#include <catch2/catch_test_macros.hpp>

#include <pulp/graph/graph_runtime_levelization.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

#include <array>
#include <vector>

namespace {

using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeLevelization;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphRuntimePlan;
using pulp::graph::build_graph_runtime_levelization;
using pulp::graph::build_graph_runtime_plan;

GraphRuntimeNodeSpec node(pulp::graph::NodeId id, std::uint32_t in, std::uint32_t out,
                          GraphRuntimeNodeKind kind = GraphRuntimeNodeKind::Processor) {
    return {id, kind, in, out};
}

// Map a NodeId to its dense plan index.
std::uint32_t index_of(const GraphRuntimePlan& plan, pulp::graph::NodeId id) {
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (plan.nodes[i].id == id) return i;
    }
    return 0xFFFFFFFFu;
}

// Assert the invariants every valid levelization must satisfy.
void check_invariants(const GraphRuntimePlan& plan, const GraphRuntimeLevelization& lv) {
    REQUIRE(lv.ok);
    REQUIRE(lv.node_level.size() == plan.nodes.size());
    // Every non-feedback connection goes strictly lower -> higher level; feedback
    // edges are unconstrained.
    for (const auto& c : plan.connections) {
        if (c.feedback) continue;
        CHECK(lv.node_level[c.source_index] < lv.node_level[c.dest_index]);
    }
    // CSR grouping is a partition: offsets monotonic, total == node count, and
    // every node appears exactly once at its own level.
    REQUIRE(lv.level_offsets.size() == lv.level_count + 1);
    CHECK(lv.level_offsets.front() == 0);
    CHECK(lv.level_offsets.back() == plan.nodes.size());
    CHECK(lv.level_nodes.size() == plan.nodes.size());
    std::vector<int> seen(plan.nodes.size(), 0);
    for (std::uint32_t l = 0; l < lv.level_count; ++l) {
        CHECK(lv.level_offsets[l] <= lv.level_offsets[l + 1]);
        for (std::uint32_t k = lv.level_offsets[l]; k < lv.level_offsets[l + 1]; ++k) {
            const std::uint32_t n = lv.level_nodes[k];
            REQUIRE(n < plan.nodes.size());
            CHECK(lv.node_level[n] == l);
            ++seen[n];
        }
    }
    for (const auto s : seen) CHECK(s == 1);
}

} // namespace

TEST_CASE("Levelization: a linear chain assigns one node per ascending level",
          "[graph][levelization]") {
    // in(0->2) -> g1 -> g2 -> out
    const std::array nodes = {
        node(1, 0, 2, GraphRuntimeNodeKind::AudioInput),
        node(2, 2, 2),
        node(3, 2, 2),
        node(4, 2, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{2, 0, 3, 0}, GraphRuntimeConnectionSpec{2, 1, 3, 1},
        GraphRuntimeConnectionSpec{3, 0, 4, 0}, GraphRuntimeConnectionSpec{3, 1, 4, 1},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    check_invariants(plan.plan, lv);
    CHECK(lv.level_count == 4);
    CHECK(lv.node_level[index_of(plan.plan, 1)] == 0);
    CHECK(lv.node_level[index_of(plan.plan, 2)] == 1);
    CHECK(lv.node_level[index_of(plan.plan, 3)] == 2);
    CHECK(lv.node_level[index_of(plan.plan, 4)] == 3);
    // Each level holds exactly one node.
    for (std::uint32_t l = 0; l < lv.level_count; ++l) {
        CHECK(lv.level_offsets[l + 1] - lv.level_offsets[l] == 1);
    }
}

TEST_CASE("Levelization: a diamond's parallel arms share a level",
          "[graph][levelization]") {
    // in -> {g1, g2} -> out. g1 and g2 are independent -> same level (1).
    const std::array nodes = {
        node(1, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(2, 1, 1),
        node(3, 1, 1),
        node(4, 1, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    check_invariants(plan.plan, lv);
    CHECK(lv.level_count == 3);
    CHECK(lv.node_level[index_of(plan.plan, 1)] == 0);
    CHECK(lv.node_level[index_of(plan.plan, 2)] == 1);
    CHECK(lv.node_level[index_of(plan.plan, 3)] == 1);  // shares level with g2
    CHECK(lv.node_level[index_of(plan.plan, 4)] == 2);
    // Level 1 holds both parallel arms.
    CHECK(lv.level_offsets[2] - lv.level_offsets[1] == 2);
}

TEST_CASE("Levelization: an unbalanced fan-in takes the longest path",
          "[graph][levelization]") {
    // in -> a -> b -> out, and in -> out directly. `out` depends on both b
    // (level 2) and in (level 0), so out is level 3 (longest chain wins).
    const std::array nodes = {
        node(1, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(2, 1, 1),
        node(3, 1, 1),
        node(4, 1, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},  // in -> a
        GraphRuntimeConnectionSpec{2, 0, 3, 0},  // a -> b
        GraphRuntimeConnectionSpec{3, 0, 4, 0},  // b -> out
        GraphRuntimeConnectionSpec{1, 0, 4, 0},  // in -> out (short path)
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    check_invariants(plan.plan, lv);
    CHECK(lv.node_level[index_of(plan.plan, 4)] == 3);
}

TEST_CASE("Levelization: a feedback edge does not raise a level",
          "[graph][levelization]") {
    // in -> a -> out, plus a feedback edge a -> a. The feedback edge reads the
    // previous block, so it must not affect a's level (would otherwise be a
    // self-cycle). a stays level 1.
    const std::array nodes = {
        node(1, 0, 1, GraphRuntimeNodeKind::AudioInput),
        node(2, 1, 1),
        node(3, 1, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    check_invariants(plan.plan, lv);
    CHECK(lv.node_level[index_of(plan.plan, 2)] == 1);
    CHECK(lv.level_count == 3);
}

TEST_CASE("Levelization: an empty plan is a valid zero-level schedule",
          "[graph][levelization]") {
    const auto plan = build_graph_runtime_plan({}, {});
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    REQUIRE(lv.ok);
    CHECK(lv.level_count == 0);
    CHECK(lv.node_level.empty());
    CHECK(lv.level_nodes.empty());
    REQUIRE(lv.level_offsets.size() == 1);
    CHECK(lv.level_offsets[0] == 0);
}

TEST_CASE("Levelization: per-node cost weight reflects audio-processing channels",
          "[graph][levelization][cost]") {
    using pulp::graph::graph_runtime_node_cost_weight;
    // Audio-processing kinds weigh max(in,out) channels; pure I/O weighs 0.
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::Processor, 2, 2) == 2);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::Processor, 1, 4) == 4);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::Utility, 3, 1) == 3);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::Custom, 2, 2) == 2);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::AudioInput, 0, 2) == 0);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::AudioOutput, 2, 0) == 0);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::MidiInput, 0, 1) == 0);
    CHECK(graph_runtime_node_cost_weight(GraphRuntimeNodeKind::MidiOutput, 1, 0) == 0);
}

TEST_CASE("Levelization: per-level work weight sums its nodes' cost weights",
          "[graph][levelization][cost]") {
    // in(0/2) -> 3 stereo processors -> out(2/0). Levels: {in}=0, {p1,p2,p3}=2*3,
    // {out}=0. Only the processor level carries weight.
    const std::array nodes = {
        node(1, 0, 2, GraphRuntimeNodeKind::AudioInput),
        node(2, 2, 2),
        node(3, 2, 2),
        node(4, 2, 2),
        node(5, 2, 0, GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{1, 0, 4, 0}, GraphRuntimeConnectionSpec{2, 0, 5, 0},
        GraphRuntimeConnectionSpec{3, 0, 5, 0}, GraphRuntimeConnectionSpec{4, 0, 5, 0},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto lv = build_graph_runtime_levelization(plan.plan);
    REQUIRE(lv.ok);
    REQUIRE(lv.level_work_weight.size() == lv.level_count);
    const std::uint32_t in_lvl = lv.node_level[index_of(plan.plan, 1)];
    const std::uint32_t proc_lvl = lv.node_level[index_of(plan.plan, 2)];
    const std::uint32_t out_lvl = lv.node_level[index_of(plan.plan, 5)];
    CHECK(lv.level_work_weight[in_lvl] == 0);
    CHECK(lv.level_work_weight[proc_lvl] == 6);  // 3 nodes * max(2,2)
    CHECK(lv.level_work_weight[out_lvl] == 0);
}
