#include <catch2/catch_test_macros.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>

#include <cmath>

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

TEST_CASE("subgraph restore rejects out-of-range values and leaves state unchanged") {
    SignalGraph graph;
    auto expanded = instantiate_subgraph(graph, gain_template(), "voice", 0);
    REQUIRE(expanded);
    auto state = expanded.instance.save_state();
    state.parameters["gain"] = 5.0f;
    const auto rejected = restore_subgraph_state(graph, expanded.instance, state);
    CHECK_FALSE(rejected);
    CHECK(expanded.instance.parameters.at("gain") == 1.0f);
    CHECK(graph.node(expanded.instance.parameter_nodes.at("gain"))->gain == 1.0f);
    state.parameters["gain"] = std::nanf("");
    CHECK_FALSE(restore_subgraph_state(graph, expanded.instance, state));
}

TEST_CASE("subgraph restore is atomic when a later entry is bad") {
    SignalGraph graph;
    auto t = gain_template();
    t.nodes.push_back({"gain2", NodeType::Gain, 1, 1.0f});
    t.parameters.push_back({"gain2", "gain2", 1.0f, 0.0f, 2.0f});
    auto expanded = instantiate_subgraph(graph, t, "voice", 0);
    REQUIRE(expanded);
    SubgraphState state = expanded.instance.save_state();
    state.parameters["gain"] = 0.25f;
    state.parameters["gain2"] = 9.0f;
    CHECK_FALSE(restore_subgraph_state(graph, expanded.instance, state));
    CHECK(expanded.instance.parameters.at("gain") == 1.0f);
    CHECK(expanded.instance.parameters.at("gain2") == 1.0f);
    CHECK(graph.node(expanded.instance.parameter_nodes.at("gain"))->gain == 1.0f);
}

TEST_CASE("subgraph rejects duplicate exposed port names") {
    SignalGraph graph;
    auto t = gain_template();
    t.exposed_ports[1].name = "input";
    const auto expanded = instantiate_subgraph(graph, t, "voice");
    CHECK_FALSE(expanded);
    CHECK(expanded.result.message().find("subgraph boundary") != std::string::npos);
    CHECK(graph.nodes().empty());
    CHECK(expanded.instance.nodes.empty());
    CHECK(expanded.instance.ports.empty());
}

TEST_CASE("subgraph rejects duplicate parameter paths and leaves no stale entries") {
    SignalGraph graph;
    auto t = gain_template();
    t.nodes.push_back({"gain2", NodeType::Gain, 1, 1.0f});
    t.parameters.push_back({"gain", "gain2", 1.0f, 0.0f, 2.0f});
    const auto expanded = instantiate_subgraph(graph, t, "voice");
    CHECK_FALSE(expanded);
    CHECK(graph.nodes().empty());
    CHECK(expanded.instance.parameters.empty());
    CHECK(expanded.instance.parameter_nodes.empty());
}

TEST_CASE("subgraph node-spec failure on a later node leaves the graph empty") {
    SignalGraph graph;
    auto t = gain_template();
    t.nodes[2].channels = 0;
    const auto expanded = instantiate_subgraph(graph, t, "voice");
    CHECK_FALSE(expanded);
    CHECK(graph.nodes().empty());
    CHECK(expanded.instance.nodes.empty());
}

TEST_CASE("subgraph rejects non-finite parameter bounds") {
    for (int which = 0; which < 2; ++which) {
        SignalGraph graph;
        auto t = gain_template();
        (which == 0 ? t.parameters[0].minimum : t.parameters[0].maximum) = std::nanf("");
        CHECK_FALSE(instantiate_subgraph(graph, t, "voice"));
        CHECK(graph.nodes().empty());
    }
}

TEST_CASE("subgraph rejects two parameters bound to one node") {
    SignalGraph graph;
    auto t = gain_template();
    t.parameters.push_back({"gain_alias", "gain", 1.0f, 0.0f, 2.0f});
    const auto expanded = instantiate_subgraph(graph, t, "voice");
    CHECK_FALSE(expanded);
    CHECK(graph.nodes().empty());
    CHECK(expanded.instance.parameter_nodes.empty());
}
