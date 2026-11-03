#include <catch2/catch_test_macros.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

using namespace pulp::host;

namespace {
SubgraphTemplate gain_template() {
    SubgraphTemplate t;
    t.identity = "test.gain";
    t.nodes = {{"in", NodeType::AudioInput, 1, 1.0f},
               {"gain", NodeType::Gain, 1, 1.0f},
               {"out", NodeType::AudioOutput, 1, 1.0f}};
    t.edges = {{"in", 0, "gain", 0}, {"gain", 0, "out", 0}};
    t.exposed_ports = {{"input", "in", 0, true, PortKind::Audio},
                       {"output", "out", 0, false, PortKind::Audio}};
    t.parameters = {{"gain", "gain", 1.0f, 0.0f, 2.0f}};
    return t;
}
} // namespace

TEST_CASE("subgraph expands with instance-local ports and parameters") {
    SignalGraph graph;
    const auto expanded = instantiate_subgraph(graph, gain_template(), "voice", 3);
    REQUIRE(expanded);
    CHECK(expanded.instance.template_identity == "test.gain");
    CHECK(expanded.instance.instance_identity == "voice");
    CHECK(expanded.instance.instance_index == 3);
    CHECK(expanded.instance.ports.at("input").output);
    CHECK_FALSE(expanded.instance.ports.at("output").output);
    CHECK(expanded.instance.parameters.at("gain") == 1.0f);
    CHECK(graph.node(expanded.instance.nodes.front())->name == "voice.in");
}

TEST_CASE("subgraph state restore keeps stable identity") {
    SignalGraph graph;
    auto expanded = instantiate_subgraph(graph, gain_template(), "voice", 7);
    REQUIRE(expanded);
    auto state = expanded.instance.save_state();
    state.parameters["gain"] = 0.5f;
    REQUIRE(restore_subgraph_state(graph, expanded.instance, state));
    CHECK(expanded.instance.parameters.at("gain") == 0.5f);
    state.instance_identity = "other";
    const auto rejected = restore_subgraph_state(graph, expanded.instance, state);
    CHECK(rejected.code == GraphResult::Code::UnknownNode);
    CHECK(rejected.message().find("subgraph boundary") != std::string::npos);
}

TEST_CASE("subgraph rejects invalid boundary and rolls back") {
    SignalGraph graph;
    auto bad = gain_template();
    bad.exposed_ports[0].port = 9;
    const auto expanded = instantiate_subgraph(graph, bad, "voice");
    CHECK_FALSE(expanded);
    CHECK(expanded.result.message().find("subgraph boundary") != std::string::npos);
    CHECK(graph.nodes().empty());
}
