// Coverage for the anticipative-rendering safety contract (masterwork Phase 6):
// the static analysis that decides which nodes may be rendered ahead of the audio
// deadline. The contract is conservative — a node is eligible ONLY if its whole
// feedforward dependency cone is free of live inputs, feedback, and sidechains —
// so these tests assert both the seeded exclusions and their downstream
// propagation, plus that a genuinely independent latent subgraph stays eligible.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/signal_graph.hpp>

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

}  // namespace

TEST_CASE("Anticipation: everything downstream of a live AudioInput is excluded",
          "[host][anticipation]") {
    // in(live) -> gain -> out. None can be anticipated: the input isn't known ahead.
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.node_exclusion[0] == AnticipationExclusion::LiveAudioInput);
    CHECK(a.node_exclusion[1] == AnticipationExclusion::LiveAudioInput);  // inherited
    CHECK(a.node_exclusion[2] == AnticipationExclusion::LiveAudioInput);
    CHECK_FALSE(a.any_passes_static_exclusions());
}

TEST_CASE("Anticipation: a generator subgraph with no live input is eligible",
          "[host][anticipation]") {
    // A source plugin (0 inputs) -> gain -> out. Nothing live/feedback/sidechain,
    // so the chain can be rendered ahead.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.passes_static_exclusions(0));
    CHECK(a.passes_static_exclusions(1));
    CHECK(a.passes_static_exclusions(2));
    CHECK(a.any_passes_static_exclusions());
}

TEST_CASE("Anticipation: a MIDI instrument and its tail are excluded as live",
          "[host][anticipation]") {
    // midi(live) -> instrument plugin -> reverb -> out. The notes aren't known
    // ahead, so the whole voice is non-anticipatable.
    std::vector<GraphNode> nodes{mk(1, NodeType::MidiInput, 0, 0),
                                 mk(2, NodeType::Plugin, 0, 2),
                                 mk(3, NodeType::Plugin, 2, 2),
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3), edge(3, 4)};
    conns[0].midi = true;  // the MIDI feed into the instrument
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.node_exclusion[0] == AnticipationExclusion::LiveMidiInput);
    CHECK(a.node_exclusion[1] == AnticipationExclusion::LiveMidiInput);
    CHECK(a.node_exclusion[2] == AnticipationExclusion::LiveMidiInput);
    CHECK(a.node_exclusion[3] == AnticipationExclusion::LiveMidiInput);
}

TEST_CASE("Anticipation: a feedback edge excludes both endpoints and the tail",
          "[host][anticipation]") {
    // gen -> a -> out, with a self-feedback on a. `a` depends on its own past, so
    // it (and out, downstream) can't run arbitrarily ahead. gen stays eligible.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    Connection fb = edge(2, 2);
    fb.feedback = true;
    conns.push_back(fb);
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.passes_static_exclusions(0));  // the generator is upstream of the loop, still eligible
    CHECK(a.node_exclusion[1] == AnticipationExclusion::Feedback);
    CHECK(a.node_exclusion[2] == AnticipationExclusion::Feedback);  // downstream
}

TEST_CASE("Anticipation: a sidechain input excludes the consumer and its tail",
          "[host][anticipation]") {
    // gen -> comp(sidechain from gen2) -> out. The sidechained compressor reads a
    // live-treated signal, so it and its tail are excluded; the two generators
    // feeding it stay eligible.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),   // main gen
                                 mk(2, NodeType::Plugin, 0, 2),   // sidechain gen
                                 mk(3, NodeType::Plugin, 4, 2),   // compressor
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 3), edge(3, 4)};
    Connection sc = edge(2, 3);
    sc.dest_port = 2;
    sc.sidechain = true;
    conns.push_back(sc);
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.passes_static_exclusions(0));
    CHECK(a.passes_static_exclusions(1));
    CHECK(a.node_exclusion[2] == AnticipationExclusion::Sidechain);
    CHECK(a.node_exclusion[3] == AnticipationExclusion::Sidechain);  // downstream
}

TEST_CASE("Anticipation: independent live and latent subgraphs are classified apart",
          "[host][anticipation]") {
    // Live: in -> g1 -> out1. Latent/eligible: gen -> g2 -> out2. The two share no
    // edges, so the live exclusion must not bleed into the generator chain.
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0),
                                 mk(4, NodeType::Plugin, 0, 2),
                                 mk(5, NodeType::Gain, 2, 2),
                                 mk(6, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3), edge(4, 5), edge(5, 6)};
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.node_exclusion[0] == AnticipationExclusion::LiveAudioInput);
    CHECK(a.node_exclusion[1] == AnticipationExclusion::LiveAudioInput);
    CHECK(a.node_exclusion[2] == AnticipationExclusion::LiveAudioInput);
    CHECK(a.passes_static_exclusions(3));
    CHECK(a.passes_static_exclusions(4));
    CHECK(a.passes_static_exclusions(5));
}

TEST_CASE("Anticipation: a sidechain fed by a live input excludes the consumer",
          "[host][anticipation]") {
    // in(live) --sidechain--> comp -> out. The sidechain source is itself live, so
    // the consumer is excluded both by the sidechain seed and by propagation from
    // the live input — the first-reason-wins reason is diagnostic, exclusion is
    // the load-bearing bit.
    std::vector<GraphNode> nodes{mk(1, NodeType::AudioInput, 0, 2),
                                 mk(2, NodeType::Plugin, 4, 2),
                                 mk(3, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(2, 3)};
    Connection sc = edge(1, 2);
    sc.dest_port = 2;
    sc.sidechain = true;
    conns.push_back(sc);
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.node_exclusion[0] == AnticipationExclusion::LiveAudioInput);
    CHECK_FALSE(a.passes_static_exclusions(1));  // excluded (live and/or sidechain)
    CHECK_FALSE(a.passes_static_exclusions(2));  // downstream
}

TEST_CASE("Anticipation: a two-node feedback cycle excludes both members",
          "[host][anticipation]") {
    // gen -> a -> b -> out, with b feeding back into a (b->a marked feedback). Both
    // a and b are on the loop and must be excluded; gen upstream stays eligible.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),
                                 mk(2, NodeType::Gain, 2, 2),
                                 mk(3, NodeType::Gain, 2, 2),
                                 mk(4, NodeType::AudioOutput, 2, 0)};
    std::vector<Connection> conns{edge(1, 2), edge(2, 3), edge(3, 4)};
    Connection fb = edge(3, 2);  // b -> a, the loop's back-edge
    fb.feedback = true;
    conns.push_back(fb);
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.passes_static_exclusions(0));  // generator upstream of the loop
    CHECK(a.node_exclusion[1] == AnticipationExclusion::Feedback);  // a (feedback dest)
    CHECK(a.node_exclusion[2] == AnticipationExclusion::Feedback);  // b (feedback source)
    CHECK(a.node_exclusion[3] == AnticipationExclusion::Feedback);  // out, downstream
}

TEST_CASE("Anticipation: a live input modulating a latent plugin's param excludes it",
          "[host][anticipation]") {
    // gen -> fx -> out, with in(live) automating fx's gain param. The modulation
    // source is live, so fx (and its tail) can't be rendered ahead even though the
    // audio path is a latent generator.
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2),    // latent gen
                                 mk(2, NodeType::Plugin, 2, 2),    // fx
                                 mk(3, NodeType::AudioOutput, 2, 0),
                                 mk(4, NodeType::AudioInput, 0, 1)};  // live modulator
    std::vector<Connection> conns{edge(1, 2), edge(2, 3)};
    Connection mod = edge(4, 2);
    mod.automation = true;
    mod.automation_param_id = 1;
    conns.push_back(mod);
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    REQUIRE(a.ok);
    CHECK(a.passes_static_exclusions(0));  // the generator itself is independent
    CHECK(a.node_exclusion[1] == AnticipationExclusion::LiveAudioInput);  // via the mod edge
    CHECK(a.node_exclusion[2] == AnticipationExclusion::LiveAudioInput);  // downstream
    CHECK(a.node_exclusion[3] == AnticipationExclusion::LiveAudioInput);  // the modulator
}

TEST_CASE("Anticipation: a connection naming an absent node is malformed",
          "[host][anticipation]") {
    std::vector<GraphNode> nodes{mk(1, NodeType::Plugin, 0, 2)};
    std::vector<Connection> conns{edge(1, 99)};  // 99 doesn't exist
    const auto a = analyze_anticipation_eligibility(nodes, conns);
    CHECK_FALSE(a.ok);
}

TEST_CASE("Anticipation: an empty graph analyzes cleanly with nothing eligible",
          "[host][anticipation]") {
    const auto a = analyze_anticipation_eligibility({}, {});
    CHECK(a.ok);
    CHECK(a.node_exclusion.empty());
    CHECK_FALSE(a.any_passes_static_exclusions());
}
