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
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true},
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

TEST_CASE("Buffer assignment pins a persistent-output node's region (never recycled)",
          "[graph][buffer-assignment][persistent]") {
    // A mono chain recycles aggressively (only ~3 slots stay live at once for
    // any length). Marking one middle node persistent forces its single output
    // slot to be dedicated — never recycled by a strictly-later node — so its
    // contents survive across blocks. That dedicated slot costs exactly one more
    // slot than the fully-reused layout, an allocator-independent, observable
    // effect of the flag.
    constexpr int kStages = 5;            // input -> p1..p5 -> output
    constexpr std::size_t kPersistIdx = 3;  // a middle processor (dense index 3)
    auto build = [](bool persistent) {
        std::vector<GraphRuntimeNodeSpec> nodes;
        nodes.push_back({1, GraphRuntimeNodeKind::AudioInput, 0, 1});
        for (int i = 0; i < kStages; ++i) {
            nodes.push_back({static_cast<pulp::graph::NodeId>(2 + i),
                             GraphRuntimeNodeKind::Processor, 1, 1});
        }
        nodes.push_back({static_cast<pulp::graph::NodeId>(2 + kStages),
                         GraphRuntimeNodeKind::AudioOutput, 1, 0});
        nodes[kPersistIdx].persistent_output = persistent;
        std::vector<GraphRuntimeConnectionSpec> conns;
        for (pulp::graph::NodeId s = 1; s <= static_cast<pulp::graph::NodeId>(kStages + 1); ++s) {
            conns.push_back({s, 0, static_cast<pulp::graph::NodeId>(s + 1), 0});
        }
        auto plan = build_graph_runtime_plan(nodes, conns);
        REQUIRE(plan.ok());
        return std::pair{plan.plan, build_graph_runtime_buffer_assignment(plan.plan)};
    };

    const auto [plan_p, a_p] = build(true);
    const auto [plan_n, a_n] = build(false);
    check_invariants(plan_p, a_p);
    check_invariants(plan_n, a_n);

    // The persistent node's single output slot is disjoint from every OTHER
    // node's input and output region (dedicated => survives across blocks).
    const auto& pin = a_p.nodes[kPersistIdx];
    REQUIRE(plan_p.nodes[kPersistIdx].output_ports == 1);
    const std::uint32_t lo = pin.output_base;
    const std::uint32_t hi = pin.output_base + 1;
    for (std::size_t i = 0; i < plan_p.nodes.size(); ++i) {
        if (i == kPersistIdx) continue;
        const auto& n = plan_p.nodes[i];
        const auto& s = a_p.nodes[i];
        if (n.input_ports > 0) {
            CHECK((s.input_base + n.input_ports <= lo || hi <= s.input_base));
        }
        if (n.output_ports > 0) {
            CHECK((s.output_base + n.output_ports <= lo || hi <= s.output_base));
        }
    }

    // The flag is load-bearing: forgoing that one node's reuse costs exactly one
    // extra slot over the fully-recycled layout.
    CHECK(a_p.slot_count == a_n.slot_count + 1);
}

TEST_CASE("Persistent-output and feedback pinning coexist without slot collision",
          "[graph][buffer-assignment][persistent][feedback]") {
    // input(0->1) -> A(1->1, persistent, 2 out) -> B(1->1) -> output(1->0), plus
    // a feedback edge B->A. A is both a feedback DEST and a persistent source; B
    // is a feedback SOURCE (pinned). The persistent region (fresh, never recycled)
    // must stay disjoint from every other node region AND from the feedback
    // previous-block slot, which is appended above the high-water mark.
    std::array<GraphRuntimeNodeSpec, 4> nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    nodes[1].persistent_output = true;  // node A
    std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},  // input -> A
        GraphRuntimeConnectionSpec{2, 0, 3, 0},  // A.0 -> B
        GraphRuntimeConnectionSpec{2, 1, 3, 1},  // A.1 -> B
        GraphRuntimeConnectionSpec{3, 0, 4, 0},  // B -> output
        GraphRuntimeConnectionSpec{3, 0, 2, 0},  // B -> A (feedback edge)
    };
    conns[4].feedback = true;
    auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);
    REQUIRE(a.has_feedback);

    // A's dedicated 2-slot output region disjoint from every other node region.
    const auto& A = a.nodes[1];
    REQUIRE(plan.plan.nodes[1].output_ports == 2);
    const std::uint32_t lo = A.output_base, hi = A.output_base + 2;
    for (std::size_t i = 0; i < plan.plan.nodes.size(); ++i) {
        if (i == 1) continue;
        const auto& n = plan.plan.nodes[i];
        const auto& s = a.nodes[i];
        if (n.input_ports > 0) CHECK((s.input_base + n.input_ports <= lo || hi <= s.input_base));
        if (n.output_ports > 0) CHECK((s.output_base + n.output_ports <= lo || hi <= s.output_base));
    }
    // The feedback previous-block slot must not land inside A's persistent region.
    for (std::size_t i = 0; i < plan.plan.connections.size(); ++i) {
        const std::uint32_t fp = a.feedback_prev_slot[i];
        if (fp != pulp::graph::kGraphRuntimeNoSlot) {
            CHECK((fp < lo || fp >= hi));
            CHECK(fp < a.slot_count);
        }
    }
}

TEST_CASE("Buffer assignment propagates latency into per-connection delays",
          "[graph][buffer-assignment][pdc]") {
    // Diamond: AudioInput -> {plugin(latency 64), gain} -> AudioOutput. The gain
    // arm is less latent than the plugin arm, so its edges to the output must be
    // delayed by 64 to time-align the fan-in; every other edge needs no delay.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 2, 2, 0, 0, true, 64},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 2, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},  // in -> plugin
        GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},  // in -> gain
        GraphRuntimeConnectionSpec{1, 1, 3, 1},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},  // plugin -> out
        GraphRuntimeConnectionSpec{2, 1, 4, 1},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},  // gain -> out (delayed)
        GraphRuntimeConnectionSpec{3, 1, 4, 1},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);

    REQUIRE(a.has_delay);
    REQUIRE(a.connection_delay_samples.size() == plan.plan.connections.size());
    // Dense indices: gain == node index 2, output == node index 3.
    for (std::size_t i = 0; i < plan.plan.connections.size(); ++i) {
        const auto& c = plan.plan.connections[i];
        const bool gain_to_out = c.source_index == 2 && c.dest_index == 3;
        CHECK(a.connection_delay_samples[i] == (gain_to_out ? 64u : 0u));
    }
}

TEST_CASE("Buffer assignment reports no delay when no node adds latency",
          "[graph][buffer-assignment][pdc]") {
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
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan);
    check_invariants(plan.plan, a);
    CHECK_FALSE(a.has_delay);
    for (const auto d : a.connection_delay_samples) CHECK(d == 0u);
}

TEST_CASE("Buffer assignment with reuse disabled gives every node disjoint regions",
          "[graph][buffer-assignment][parallel]") {
    // The parallel layout (allow_reuse=false) must never share a scratch slot
    // between two nodes — concurrent same-level nodes would otherwise alias it.
    // Diamond: in -> {g1, g2} -> out (g1/g2 are an independent parallel pair).
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 2, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{1, 0, 3, 0}, GraphRuntimeConnectionSpec{1, 1, 3, 1},
        GraphRuntimeConnectionSpec{2, 0, 4, 0}, GraphRuntimeConnectionSpec{2, 1, 4, 1},
        GraphRuntimeConnectionSpec{3, 0, 4, 0}, GraphRuntimeConnectionSpec{3, 1, 4, 1},
    };
    const auto plan = build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    const auto a = build_graph_runtime_buffer_assignment(plan.plan, /*allow_reuse=*/false);
    check_invariants(plan.plan, a);

    // Mark every slot each node's input/output region occupies; no slot may be
    // claimed by two distinct regions (across all nodes).
    std::vector<int> used(a.slot_count, 0);
    for (std::size_t i = 0; i < plan.plan.nodes.size(); ++i) {
        const auto& node = plan.plan.nodes[i];
        const auto& s = a.nodes[i];
        for (std::uint32_t p = 0; p < node.input_ports; ++p) ++used[s.input_base + p];
        for (std::uint32_t p = 0; p < node.output_ports; ++p) ++used[s.output_base + p];
    }
    for (const auto u : used) CHECK(u <= 1);  // no slot shared by two regions

    // Reuse-free slot_count is exactly the sum of every node's port regions.
    CHECK(a.slot_count == total_ports(nodes));

    // The compact (reuse) layout for the same graph uses strictly fewer slots.
    const auto reuse = build_graph_runtime_buffer_assignment(plan.plan, /*allow_reuse=*/true);
    CHECK(reuse.slot_count < a.slot_count);
}
