// Coverage for the anticipation partition: given 6a's eligibility classification,
// it carves out the renderable eligible interior and the boundary edges whose
// pre-rendered output the live graph would consume. These tests assert that live
// sinks stay out of the interior, every interior->outside edge is captured as a
// splice point, the cost weight reflects the interior, and a live-only graph
// yields nothing worth anticipating.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
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

Connection edge(NodeId s, NodeId d) {
    Connection c;
    c.source_node = s;
    c.source_port = 0;
    c.dest_node = d;
    c.dest_port = 0;
    return c;
}

AnticipationPartition partition_of(std::span<const GraphNode> nodes,
                                   std::span<const Connection> conns) {
    const auto elig = analyze_anticipation_eligibility(nodes, conns);
    return build_anticipation_partition(nodes, conns, elig);
}

bool interior_has(const AnticipationPartition& p, const std::vector<GraphNode>& nodes,
                  NodeId id) {
    return std::any_of(p.interior_nodes.begin(), p.interior_nodes.end(),
                       [&](std::size_t i) { return nodes[i].id == id; });
}

bool has_boundary(const AnticipationPartition& p, NodeId src, NodeId dst) {
    return std::any_of(p.boundary.begin(), p.boundary.end(),
                       [&](const AnticipationBoundary& b) {
                           return b.source_node == src && b.dest_node == dst;
                       });
}

}  // namespace

TEST_CASE("Partition: a latent generator chain has interior nodes and a sink boundary",
          "[host][anticipation][partition]") {
    // gen(2 out) -> gain -> out. Interior = {gen, gain}; the live sink `out` is
    // excluded from the interior and is the boundary the renderer feeds.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(p.interior_nodes.size() == 2);
    CHECK(interior_has(p, nodes, 1));
    CHECK(interior_has(p, nodes, 2));
    CHECK_FALSE(interior_has(p, nodes, 3));   // live sink stays out
    CHECK(has_boundary(p, 2, 3));             // gain -> out is the splice point
    CHECK(p.boundary.size() == 1);
    CHECK(p.cost_weight == 4);                // gen max(0,2)=2 + gain max(2,2)=2
    CHECK(p.worth_anticipating());
}

TEST_CASE("Partition: a fully live graph yields nothing worth anticipating",
          "[host][anticipation][partition]") {
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(p.interior_nodes.empty());
    CHECK(p.boundary.empty());
    CHECK(p.cost_weight == 0);
    CHECK_FALSE(p.worth_anticipating());
}

TEST_CASE("Partition: an eligible interior feeding a live node makes that edge a boundary",
          "[host][anticipation][partition]") {
    // gen -> fx, and in(live) -> fx. fx is excluded (live-fed); gen is interior.
    // The gen -> fx edge crosses out of the interior and is a splice point.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),    // gen (interior)
                                 mk(2, NodeType::AudioInput, 0, 2),  // live
                                 mk(3, NodeType::Plugin, 4, 2),    // fx (excluded)
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 3), edge(2, 3), edge(3, 4)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(interior_has(p, nodes, 1));
    CHECK_FALSE(interior_has(p, nodes, 3));  // fx is live-fed
    CHECK(has_boundary(p, 1, 3));            // gen -> fx
    CHECK_FALSE(has_boundary(p, 3, 4));      // fx is not interior; not a boundary
    CHECK(p.worth_anticipating());
}

TEST_CASE("Partition: independent latent subgraph is isolated from the live one",
          "[host][anticipation][partition]") {
    // Live: in -> g1 -> out1. Latent: gen -> g2 -> out2. Only the latent chain is
    // interior; its boundary is g2 -> out2.
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0),
                                 mk(4, NodeType::Plugin, 0, 2),
                                 mk(5, NodeType::Gain, 2, 2),
                                 mk(6, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3), edge(4, 5), edge(5, 6)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(interior_has(p, nodes, 4));
    CHECK(interior_has(p, nodes, 5));
    CHECK_FALSE(interior_has(p, nodes, 2));  // live chain excluded
    CHECK(has_boundary(p, 5, 6));
    CHECK(p.boundary.size() == 1);
    CHECK(p.worth_anticipating());
}

TEST_CASE("Partition: a malformed connection or unmatched eligibility is rejected",
          "[host][anticipation][partition]") {
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2)};

    SECTION("dangling connection id") {
        std::vector<Connection> conns{edge(1, 99)};
        // 6a already rejects this, so eligibility is not ok and the partition is
        // rejected in turn.
        const auto elig = analyze_anticipation_eligibility(nodes, conns);
        CHECK_FALSE(elig.ok);
        const auto p = build_anticipation_partition(nodes, conns, elig);
        CHECK_FALSE(p.ok);
    }

    SECTION("eligibility sized for a different graph") {
        AnticipationEligibility mismatched;
        mismatched.ok = true;
        mismatched.node_exclusion.assign(5, AnticipationExclusion::None);
        const auto p = build_anticipation_partition(nodes, {}, mismatched);
        CHECK_FALSE(p.ok);
    }

    SECTION("dangling id with an otherwise-matching eligibility") {
        // Exercise the partition's own dangling-id rejection directly: a
        // size-matched, ok eligibility paired with a connection naming an absent
        // node (the case 6a would normally have rejected first).
        AnticipationEligibility ok_elig;
        ok_elig.ok = true;
        ok_elig.node_exclusion.assign(1, AnticipationExclusion::None);
        std::vector<Connection> conns{edge(1, 99)};
        const auto p = build_anticipation_partition(nodes, conns, ok_elig);
        CHECK_FALSE(p.ok);
    }
}

TEST_CASE("Partition: a duplicate node id is rejected as malformed",
          "[host][anticipation][partition]") {
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(1, NodeType::Gain, 2, 2)};  // duplicate id 1
    const auto p = partition_of(nodes, {});
    CHECK_FALSE(p.ok);
}

TEST_CASE("Partition: an interior node feeding both interior and live nodes",
          "[host][anticipation][partition]") {
    // gen -> g (interior), and gen also -> fx where fx is live-fed (excluded). The
    // gen->g edge is internal (not a boundary); the gen->fx edge is a boundary.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),     // gen (interior)
                                 mk(2, NodeType::Gain, 2, 2),       // g (interior)
                                 mk(3, NodeType::AudioInput, 0, 2),   // live
                                 mk(4, NodeType::Plugin, 4, 2),     // fx (live-fed)
                                 mk(5, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(1, 4), edge(3, 4), edge(2, 5),
                                  edge(4, 5)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(interior_has(p, nodes, 1));
    CHECK(interior_has(p, nodes, 2));
    CHECK_FALSE(interior_has(p, nodes, 4));  // live-fed
    CHECK_FALSE(has_boundary(p, 1, 2));      // internal edge, not a splice point
    CHECK(has_boundary(p, 1, 4));            // gen -> live fx
    CHECK(has_boundary(p, 2, 5));            // g -> out
}

TEST_CASE("Partition: a MIDI boundary edge from the interior is captured",
          "[host][anticipation][partition]") {
    // A sequenced (non-live) MIDI generator -> instrument, where the instrument is
    // excluded for another reason (sidechain). The midi edge crossing out of the
    // interior is still recorded as a boundary splice point.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 0),    // midi gen (interior)
                                 mk(2, NodeType::Plugin, 2, 2),    // sidechain src (interior)
                                 mk(3, NodeType::Plugin, 4, 2),    // instrument (sidechained)
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(3, 4)};
    Connection midi_edge = edge(1, 3);
    midi_edge.midi = true;
    conns.push_back(midi_edge);
    Connection sc = edge(2, 3);
    sc.dest_port = 2;
    sc.sidechain = true;
    conns.push_back(sc);
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(interior_has(p, nodes, 1));
    CHECK_FALSE(interior_has(p, nodes, 3));  // sidechained
    CHECK(has_boundary(p, 1, 3));            // the midi edge is a boundary
}

TEST_CASE("Partition: a transport-sensitive node never enters the interior",
          "[host][anticipation][partition]") {
    // ts(transport-sensitive gen) -> gain -> out. The transport-sensitive node is
    // excluded (TransportSensitive) and so is its downstream cone, so NOTHING is
    // ahead-renderable — the interior is empty and there is nothing worth
    // anticipating. This is the partition-level guarantee that a transport-aware
    // node always runs live.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    nodes[0].transport_sensitive = true;
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK_FALSE(interior_has(p, nodes, 1));  // transport-sensitive: never interior
    CHECK_FALSE(interior_has(p, nodes, 2));  // its cone inherits the exclusion
    CHECK(p.interior_nodes.empty());
    CHECK_FALSE(p.worth_anticipating());
}

TEST_CASE("Partition: a transport-sensitive branch stays exterior beside a latent interior",
          "[host][anticipation][partition]") {
    // Two independent branches into one live sink:
    //   gen1 -> gain1 -> out          (transport-insensitive: ahead-renderable)
    //   ts(gen2) -> gain2 -> out      (transport-sensitive: forced exterior)
    // Only the first branch is interior; the transport-sensitive node and its
    // cone never enter the interior, yet the latent branch still anticipates.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),   // gen1 (interior)
                                 mk(2, NodeType::Gain, 2, 2),     // gain1 (interior)
                                 mk(3, NodeType::AudioOutput, 2, 0),  // shared sink
                                 mk(4, NodeType::Plugin, 0, 2),   // gen2 (ts)
                                 mk(5, NodeType::Gain, 2, 2)};     // gain2 (ts cone)
    nodes[3].transport_sensitive = true;
    std::vector<Connection> conns{edge(1, 2), edge(2, 3), edge(4, 5), edge(5, 3)};
    const auto p = partition_of(nodes, conns);
    REQUIRE(p.ok);
    CHECK(interior_has(p, nodes, 1));         // latent branch is interior
    CHECK(interior_has(p, nodes, 2));
    CHECK_FALSE(interior_has(p, nodes, 4));   // transport-sensitive: exterior
    CHECK_FALSE(interior_has(p, nodes, 5));   // its cone: exterior
    CHECK(has_boundary(p, 2, 3));             // gain1 -> out is the splice point
    CHECK(p.worth_anticipating());
}

TEST_CASE("Partition: an empty graph is ok but not worth anticipating",
          "[host][anticipation][partition]") {
    const auto p = partition_of({}, {});
    CHECK(p.ok);
    CHECK(p.interior_nodes.empty());
    CHECK_FALSE(p.worth_anticipating());
}
