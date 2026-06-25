// Coverage for the anticipation sub-graph extraction: the renderable interior
// carved from a partition, with one synthesized AudioOutput sink whose input
// ports carry the distinct boundary outputs. These tests assert the interior
// nodes and internal edges are copied, each distinct boundary port gets its own
// sink input/channel, synthesized ids never collide with existing ones, and a
// graph with no eligible interior renders nothing.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <vector>

using namespace pulp::host;

namespace {

GraphNode mk(NodeId id, NodeType type, int in, int out) {
    GraphNode n;
    n.id = id;
    n.type = type;
    n.num_input_ports = in;
    n.num_output_ports = out;
    return n;
}

Connection edge_p(NodeId s, PortIndex sp, NodeId d, PortIndex dp) {
    Connection c;
    c.source_node = s;
    c.source_port = sp;
    c.dest_node = d;
    c.dest_port = dp;
    return c;
}

AnticipationSubgraph subgraph_of(std::span<const GraphNode> nodes,
                                 std::span<const Connection> conns) {
    const auto elig = analyze_anticipation_eligibility(nodes, conns);
    const auto part = build_anticipation_partition(nodes, conns, elig);
    return build_anticipation_subgraph(nodes, conns, part);
}

const GraphNode* find_node(const AnticipationSubgraph& g, NodeId id) {
    const auto it = std::find_if(g.nodes.begin(), g.nodes.end(),
                                 [&](const GraphNode& n) { return n.id == id; });
    return it == g.nodes.end() ? nullptr : &*it;
}

bool has_conn(const AnticipationSubgraph& g, NodeId s, PortIndex sp, NodeId d) {
    return std::any_of(g.connections.begin(), g.connections.end(),
                       [&](const Connection& c) {
                           return c.source_node == s && c.source_port == sp &&
                                  c.dest_node == d;
                       });
}

bool has_conn_port(const AnticipationSubgraph& g, NodeId s, PortIndex sp, NodeId d,
                   PortIndex dp) {
    return std::any_of(g.connections.begin(), g.connections.end(),
                       [&](const Connection& c) {
                           return c.source_node == s && c.source_port == sp &&
                                  c.dest_node == d && c.dest_port == dp;
                       });
}

int count_sinks(const AnticipationSubgraph& g) {
    return static_cast<int>(std::count_if(
        g.nodes.begin(), g.nodes.end(),
        [](const GraphNode& n) { return n.type == NodeType::AudioOutput; }));
}

}  // namespace

TEST_CASE("Subgraph: a stereo generator chain copies the interior and synthesizes one multi-input sink",
          "[host][anticipation][subgraph]") {
    // gen(0/2) -> gain(2/2) -> out(2/0), stereo. Interior = {gen, gain}; out is a
    // live sink. Two boundary ports (gain:0, gain:1) -> one two-input sink.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge_p(1, 0, 2, 0), edge_p(1, 1, 2, 1),
                                  edge_p(2, 0, 3, 0), edge_p(2, 1, 3, 1)};
    const auto g = subgraph_of(nodes, conns);
    REQUIRE(g.ok);

    // Interior copied verbatim.
    REQUIRE(find_node(g, 1) != nullptr);
    REQUIRE(find_node(g, 2) != nullptr);
    CHECK(find_node(g, 3) == nullptr);  // the live sink is not in the sub-graph

    // Internal edges copied (both ports of gen -> gain).
    CHECK(has_conn(g, 1, 0, 2));
    CHECK(has_conn(g, 1, 1, 2));

    // Two distinct boundary ports -> ONE synthesized AudioOutput sink with two
    // input ports, each boundary port wired to its OWN sink dest_port so it lands
    // on its own output-bus channel (never summed). outputs[i] == channel i.
    REQUIRE(g.outputs.size() == 2);
    CHECK(g.renders_anything());
    CHECK(count_sinks(g) == 1);
    const NodeId sink = g.outputs[0].sink_node;
    CHECK(g.outputs[1].sink_node == sink);  // one shared sink
    const GraphNode* sink_node = find_node(g, sink);
    REQUIRE(sink_node != nullptr);
    CHECK(sink_node->type == NodeType::AudioOutput);
    CHECK(sink_node->num_input_ports == 2);  // one channel per captured port
    CHECK(sink_node->num_output_ports == 0);
    CHECK(sink_node->id > 3);  // fresh, above every original id
    for (std::size_t i = 0; i < g.outputs.size(); ++i) {
        const auto& o = g.outputs[i];
        CHECK(o.source_node == 2);
        // boundary port i -> sink dest_port i (== output-bus channel i)
        CHECK(has_conn_port(g, o.source_node, o.source_port, sink,
                            static_cast<PortIndex>(i)));
    }
    CHECK(g.outputs[0].source_port != g.outputs[1].source_port);  // ports 0 and 1
}

TEST_CASE("Subgraph: several consumers of one interior port share a single sink",
          "[host][anticipation][subgraph]") {
    // gen -> gain, and gain:0 feeds two live sinks. The interior port (gain,0) is
    // captured once, not per consumer.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 1),
                                 mk(2, NodeType::Gain, 1, 1),
                                 mk(3, NodeType::AudioOutput, 1, 0),
                                 mk(4, NodeType::AudioOutput, 1, 0)};
    std::vector<Connection> conns{edge_p(1, 0, 2, 0), edge_p(2, 0, 3, 0),
                                  edge_p(2, 0, 4, 0)};
    const auto g = subgraph_of(nodes, conns);
    REQUIRE(g.ok);
    REQUIRE(g.outputs.size() == 1);  // (gain,0) captured once
    CHECK(g.outputs[0].source_node == 2);
    CHECK(g.outputs[0].source_port == 0);
}

TEST_CASE("Subgraph: a fully live graph renders nothing",
          "[host][anticipation][subgraph]") {
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge_p(1, 0, 2, 0), edge_p(2, 0, 3, 0)};
    const auto g = subgraph_of(nodes, conns);
    REQUIRE(g.ok);
    CHECK(g.nodes.empty());
    CHECK(g.outputs.empty());
    CHECK_FALSE(g.renders_anything());
}

TEST_CASE("Subgraph: an interior feeding a live node captures the boundary port",
          "[host][anticipation][subgraph]") {
    // gen -> fx, in(live) -> fx. fx is excluded; gen is interior. The gen -> fx
    // edge is a boundary, so gen's output port is captured by a sink.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::AudioInput, 0, 2),
                                 mk(3, NodeType::Plugin, 4, 2),
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge_p(1, 0, 3, 0), edge_p(1, 1, 3, 1),
                                  edge_p(2, 0, 3, 2), edge_p(2, 1, 3, 3),
                                  edge_p(3, 0, 4, 0), edge_p(3, 1, 4, 1)};
    const auto g = subgraph_of(nodes, conns);
    REQUIRE(g.ok);
    CHECK(find_node(g, 1) != nullptr);
    CHECK(find_node(g, 3) == nullptr);  // fx excluded
    REQUIRE(g.outputs.size() == 2);     // gen's two ports captured
    for (const auto& o : g.outputs) CHECK(o.source_node == 1);
}

TEST_CASE("Subgraph: a not-ok partition yields a not-ok sub-graph",
          "[host][anticipation][subgraph]") {
    AnticipationPartition bad;  // ok == false
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2)};
    const auto g = build_anticipation_subgraph(nodes, {}, bad);
    CHECK_FALSE(g.ok);
}
