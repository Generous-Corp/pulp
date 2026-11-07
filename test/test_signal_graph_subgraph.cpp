#include <catch2/catch_test_macros.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/voice_identity.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

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

SubgraphTemplate source_stage_template() {
    SubgraphTemplate t;
    t.identity = "test.source-stage";
    t.nodes = {{"input", NodeType::AudioInput, 1, 1.0f}, {"gain", NodeType::Gain, 1, 1.0f}};
    t.edges = {{"input", 0, "gain", 0}};
    t.exposed_ports = {{"input", "input", 0, true, PortKind::Audio},
                       {"output", "gain", 0, true, PortKind::Audio}};
    t.parameters = {{"gain", "gain", 1.0f, 0.0f, 2.0f}};
    return t;
}

SubgraphTemplate sink_stage_template() {
    SubgraphTemplate t;
    t.identity = "test.sink-stage";
    t.nodes = {{"gain", NodeType::Gain, 1, 1.0f}, {"output", NodeType::AudioOutput, 1, 1.0f}};
    t.edges = {{"gain", 0, "output", 0}};
    t.exposed_ports = {{"input", "gain", 0, false, PortKind::Audio},
                       {"output", "output", 0, false, PortKind::Audio}};
    t.parameters = {{"gain", "gain", 1.0f, 0.0f, 2.0f}};
    return t;
}

SubgraphTemplate nested_gain_template() {
    auto child = std::make_shared<SubgraphTemplate>(gain_template());
    SubgraphTemplate parent;
    parent.identity = "test.rack";
    parent.children = {{"filter", std::move(child), 4}};
    parent.exposed_ports = {{"input", "filter.input", 0, true, PortKind::Audio},
                            {"output", "filter.output", 0, false, PortKind::Audio}};
    return parent;
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

TEST_CASE("subgraph rejects non-Gain parameter targets before changing the graph") {
    for (const auto* target : {"in", "out"}) {
        CAPTURE(target);
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "existing input");
        const auto gain = graph.add_gain_node("existing gain");
        REQUIRE(graph.set_node_gain(gain, 0.25f));
        REQUIRE(graph.connect(input, 0, gain, 0));
        auto t = gain_template();
        t.parameters.push_back({"invalid", target, 0.5f, 0.0f, 2.0f});

        const auto expanded = instantiate_subgraph(graph, t, "voice");

        CHECK_FALSE(expanded);
        CHECK(expanded.result.code == GraphResult::Code::UnknownPort);
        CHECK(expanded.result.message().find("subgraph boundary") != std::string::npos);
        CHECK(expanded.result.message().find("not a gain node") != std::string::npos);
        CHECK(expanded.instance.nodes.empty());
        CHECK(expanded.instance.ports.empty());
        CHECK(expanded.instance.parameters.empty());
        CHECK(expanded.instance.parameter_nodes.empty());
        CHECK(expanded.instance.parameter_bounds.empty());
        CHECK(graph.nodes().size() == 2);
        REQUIRE(graph.node(input));
        CHECK(graph.node(input)->name == "existing input");
        REQUIRE(graph.node(gain));
        CHECK(graph.node(gain)->gain == 0.25f);
        CHECK(graph.connections().size() == 1);
        // Rollback alone would still consume IDs; rejection must precede allocation.
        CHECK(graph.add_gain_node("next") == gain + 1);
    }
}

TEST_CASE("subgraph Gain parameter default is applied and restores successfully") {
    SignalGraph graph;
    auto t = gain_template();
    t.parameters[0].default_value = 0.5f;
    auto expanded = instantiate_subgraph(graph, t, "voice");
    REQUIRE(expanded);
    const auto id = expanded.instance.parameter_nodes.at("gain");
    REQUIRE(graph.node(id));
    CHECK(graph.node(id)->type == NodeType::Gain);
    CHECK(graph.node(id)->gain == 0.5f);
    CHECK(expanded.instance.parameters.at("gain") == 0.5f);
    auto state = expanded.instance.save_state();
    state.parameters.at("gain") = 0.75f;
    REQUIRE(restore_subgraph_state(graph, expanded.instance, state));
    CHECK(graph.node(id)->gain == 0.75f);
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

TEST_CASE("subgraph connects exposed ports by name") {
    SignalGraph graph;
    const auto source = instantiate_subgraph(graph, source_stage_template(), "source");
    const auto sink = instantiate_subgraph(graph, sink_stage_template(), "sink");
    REQUIRE(source);
    REQUIRE(sink);
    REQUIRE(graph.connections().size() == 2);

    const auto connected =
        connect_subgraph_ports(graph, source.instance, "output", sink.instance, "input");
    REQUIRE(connected);
    CHECK(graph.connections().size() == 3);
    CHECK(graph.connections().back().source_node == source.instance.ports.at("output").node);
    CHECK(graph.connections().back().dest_node == sink.instance.ports.at("input").node);
}

TEST_CASE("subgraph port connection negatives are planted and mutation-free") {
    SignalGraph graph;
    const auto source = instantiate_subgraph(graph, source_stage_template(), "source");
    const auto sink = instantiate_subgraph(graph, sink_stage_template(), "sink");
    REQUIRE(source);
    REQUIRE(sink);
    const auto before = graph.connections().size();

    const auto missing =
        connect_subgraph_ports(graph, source.instance, "missing", sink.instance, "input");
    CHECK_FALSE(missing);
    CHECK(missing.code == GraphResult::Code::UnknownPort);
    CHECK(missing.message().find("subgraph boundary") != std::string::npos);
    CHECK(graph.connections().size() == before);

    const auto wrong_direction =
        connect_subgraph_ports(graph, sink.instance, "output", source.instance, "output");
    CHECK_FALSE(wrong_direction);
    CHECK(wrong_direction.code == GraphResult::Code::UnknownPort);
    CHECK(wrong_direction.message().find("not an output") != std::string::npos);
    CHECK(graph.connections().size() == before);

    auto wrong_kind = sink.instance;
    wrong_kind.ports.at("input").kind = PortKind::Event;
    const auto mismatched =
        connect_subgraph_ports(graph, source.instance, "output", wrong_kind, "input");
    CHECK_FALSE(mismatched);
    CHECK(mismatched.code == GraphResult::Code::KindMismatch);
    CHECK(mismatched.message().find("kinds do not match") != std::string::npos);
    CHECK(graph.connections().size() == before);
}

TEST_CASE("subgraph ports reject clear-reuse and cross-graph instances") {
    SignalGraph graph;
    const auto old_source = instantiate_subgraph(graph, source_stage_template(), "source");
    const auto old_sink = instantiate_subgraph(graph, sink_stage_template(), "sink");
    REQUIRE(old_source);
    REQUIRE(old_sink);

    graph.clear();
    const auto replacement_source = instantiate_subgraph(graph, source_stage_template(), "source");
    const auto replacement_sink = instantiate_subgraph(graph, sink_stage_template(), "sink");
    REQUIRE(replacement_source);
    REQUIRE(replacement_sink);
    REQUIRE(replacement_source.instance.nodes.front() == old_source.instance.nodes.front());
    const auto before_reuse = graph.connections().size();

    const auto stale = connect_subgraph_ports(graph, old_source.instance, "output",
                                              replacement_sink.instance, "input");
    CHECK_FALSE(stale);
    CHECK(stale.code == GraphResult::Code::UnknownPort);
    CHECK(stale.message().find("stale or unavailable") != std::string::npos);
    CHECK(graph.connections().size() == before_reuse);

    SignalGraph other_graph;
    const auto foreign_source =
        instantiate_subgraph(other_graph, source_stage_template(), "source");
    const auto foreign_sink = instantiate_subgraph(other_graph, sink_stage_template(), "sink");
    REQUIRE(foreign_source);
    REQUIRE(foreign_sink);
    const auto before_foreign = other_graph.connections().size();
    const auto foreign = connect_subgraph_ports(other_graph, replacement_source.instance, "output",
                                                foreign_sink.instance, "input");
    CHECK_FALSE(foreign);
    CHECK(foreign.code == GraphResult::Code::UnknownPort);
    CHECK(foreign.message().find("stale or unavailable") != std::string::npos);
    CHECK(other_graph.connections().size() == before_foreign);
}

TEST_CASE("nested subgraph macros namespace ports and state deterministically") {
    SignalGraph graph;
    auto expanded = instantiate_subgraph(graph, nested_gain_template(), "rack", 2);
    REQUIRE(expanded);
    REQUIRE(expanded.instance.nodes.size() == 3);
    CHECK(graph.node(expanded.instance.nodes.front())->name == "rack.filter.in");
    CHECK(expanded.instance.ports.at("input").output);
    CHECK_FALSE(expanded.instance.ports.at("output").output);
    CHECK(expanded.instance.parameters.at("filter.gain") == 1.0f);
    CHECK(expanded.instance.child_template_identities.at("filter") == "test.gain");
    CHECK(expanded.instance.child_instance_indices.at("filter") == 4);

    auto state = expanded.instance.save_state();
    state.parameters.at("filter.gain") = 0.25f;
    REQUIRE(restore_subgraph_state(graph, expanded.instance, state));
    CHECK(graph.node(expanded.instance.parameter_nodes.at("filter.gain"))->gain == 0.25f);

    auto before = expanded.instance.save_state();
    before.child_template_identities.at("filter") = "test.other";
    const auto rejected = restore_subgraph_state(graph, expanded.instance, before);
    CHECK_FALSE(rejected);
    CHECK(rejected.message().find("subgraph boundary") != std::string::npos);
    CHECK(expanded.instance.parameters.at("filter.gain") == 0.25f);
}

TEST_CASE("nested subgraph rejects cycles and duplicate child keys before graph mutation") {
    SignalGraph graph;
    auto cyclic = std::make_shared<SubgraphTemplate>();
    cyclic->identity = "test.cycle";
    cyclic->children = {{"loop", cyclic, 0}};
    const auto cycle_result = instantiate_subgraph(graph, *cyclic, "cycle");
    CHECK_FALSE(cycle_result);
    CHECK(cycle_result.result.message().find("subgraph boundary") != std::string::npos);
    CHECK(graph.nodes().empty());

    auto duplicate = nested_gain_template();
    duplicate.children.push_back(
        {"filter", std::make_shared<SubgraphTemplate>(gain_template()), 5});
    const auto duplicate_result = instantiate_subgraph(graph, duplicate, "duplicate");
    CHECK_FALSE(duplicate_result);
    CHECK(duplicate_result.result.message().find("subgraph boundary") != std::string::npos);
    CHECK(graph.nodes().empty());
}

TEST_CASE("G4 voice bank fixture expands eight slots with stable exposed identity") {
    SignalGraph graph;
    std::vector<SubgraphInstance> slots;
    std::unordered_set<std::string> identity_digests;
    slots.reserve(8);

    for (std::uint32_t slot = 0; slot < 8; ++slot) {
        auto expanded = instantiate_subgraph(graph, gain_template(),
                                             "voice-bank.voice-" + std::to_string(slot), slot);
        REQUIRE(expanded);
        REQUIRE(expanded.instance.ports.contains("input"));
        REQUIRE(expanded.instance.ports.contains("output"));
        REQUIRE(expanded.instance.parameters.contains("gain"));

        VoiceIdentityKey key;
        key.template_namespace = "test";
        key.template_name = "voice-bank";
        key.template_revision = 1;
        key.bank_schema_version = 1;
        key.slot_ordinal = slot;
        key.path = {"node", "gain"};
        key.has_parameter = true;
        key.parameter = "gain";
        const auto identity = canonical_voice_identity(key);
        REQUIRE(identity.valid);
        CHECK(identity_digests.insert(identity.sha256).second);
        CHECK(identity.sha256 == canonical_voice_identity(key).sha256);

        slots.emplace_back(std::move(expanded.instance));
    }

    REQUIRE(slots.size() == 8);
    CHECK(graph.nodes().size() == 24);
    for (std::size_t left = 0; left < slots.size(); ++left)
        for (std::size_t right = left + 1; right < slots.size(); ++right) {
            CHECK(slots[left].ports.at("input").node != slots[right].ports.at("input").node);
            CHECK(slots[left].parameter_nodes.at("gain") !=
                  slots[right].parameter_nodes.at("gain"));
        }

    auto changed = slots.at(3).save_state();
    changed.parameters.at("gain") = 0.25f;
    REQUIRE(restore_subgraph_state(graph, slots.at(3), changed));
    for (std::size_t slot = 0; slot < slots.size(); ++slot) {
        const auto gain = graph.node_gain(slots.at(slot).parameter_nodes.at("gain"));
        CHECK(gain == (slot == 3 ? 0.25f : 1.0f));
    }

    auto stale = slots.at(3).save_state();
    stale.instance_identity = "voice-bank.other";
    CHECK_FALSE(restore_subgraph_state(graph, slots.at(3), stale));
    CHECK(graph.node_gain(slots.at(3).parameter_nodes.at("gain")) == 0.25f);
}
