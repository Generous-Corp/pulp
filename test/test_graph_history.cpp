// Value-owned graph edit history and logical-identity document diff.

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/custom_node_type.hpp>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <string>

using namespace pulp::host;

namespace {

struct Chain {
    NodeId input = 0;
    NodeId gain = 0;
    NodeId output = 0;
};

Chain build_chain(SignalGraph& graph) {
    Chain chain;
    chain.input = graph.add_input_node(1, "Input");
    chain.gain = graph.add_gain_node("Gain");
    chain.output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(chain.input, 0, chain.gain, 0));
    REQUIRE(graph.connect(chain.gain, 0, chain.output, 0));
    return chain;
}

bool contains(const std::vector<std::string>& values, std::string_view needle) {
    return std::any_of(values.begin(), values.end(),
                       [&](const auto& value) { return value.find(needle) != std::string::npos; });
}

const GraphNode* node_named(const SignalGraph& graph, std::string_view name) {
    for (const auto& node : graph.nodes())
        if (node.name == name)
            return &node;
    return nullptr;
}

} // namespace

TEST_CASE("graph document diff compares by logical identity, not NodeId",
          "[host][graph][history][g3]") {
    SignalGraph first;
    build_chain(first);

    // The same authored graph built with different NodeIds: a throwaway node
    // shifts every later id.
    SignalGraph second;
    const auto spacer = second.add_gain_node("Spacer");
    build_chain(second);
    REQUIRE(second.remove_node(spacer));

    const auto a = GraphSerializer::to_saved_document(first);
    const auto b = GraphSerializer::to_saved_document(second);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    // Control: the documents really differ in NodeId numbering.
    REQUIRE(a != b);

    const auto diff = diff_graph_documents(a, b);
    INFO(diff.detail);
    REQUIRE(diff.ok());
    CHECK(diff.equal());
    CHECK(diff.nodes_added.empty());
    CHECK(diff.connections_added.empty());
    CHECK(diff.sections_changed.empty());
}

TEST_CASE("graph document diff reports node, connection and layout changes",
          "[host][graph][history][g3]") {
    SignalGraph graph;
    const auto chain = build_chain(graph);
    const auto before = GraphSerializer::to_saved_document(graph, {{chain.gain, {1.0f, 2.0f}}});

    REQUIRE(graph.set_node_gain(chain.gain, 0.5f));
    REQUIRE(graph.disconnect(chain.gain, 0, chain.output, 0));
    const auto extra = graph.add_gain_node("Trim");
    REQUIRE(graph.connect(chain.gain, 0, extra, 0));
    REQUIRE(graph.connect(extra, 0, chain.output, 0));
    const auto after = GraphSerializer::to_saved_document(graph, {{chain.gain, {5.0f, 6.0f}}});

    const auto diff = diff_graph_documents(before, after);
    INFO(diff.detail);
    REQUIRE(diff.ok());
    CHECK_FALSE(diff.equal());
    REQUIRE(diff.nodes_added.size() == 1);
    CHECK(diff.nodes_added.front() == "node:gain:Trim:0");
    CHECK(diff.nodes_removed.empty());
    REQUIRE(diff.nodes_changed.size() == 1);
    CHECK(diff.nodes_changed.front().identity == "node:gain:Gain:0");
    CHECK(diff.nodes_changed.front().fields == std::vector<std::string>{"gain"});
    CHECK(diff.layout_changed == std::vector<std::string>{"node:gain:Gain:0"});
    CHECK(contains(diff.connections_removed, "node:gain:Gain:0[0] -> node:audio_out:Output:0[0]"));
    CHECK(contains(diff.connections_added, "node:gain:Gain:0[0] -> node:gain:Trim:0[0]"));
    CHECK(contains(diff.connections_added, "node:gain:Trim:0[0] -> node:audio_out:Output:0[0]"));
    CHECK_FALSE(diff.before_digest.empty());
    CHECK(diff.before_digest != diff.after_digest);

    // Layout alone is reported but is not a semantic difference.
    const auto moved = GraphSerializer::to_saved_document(graph, {{chain.gain, {9.0f, 9.0f}}});
    const auto layout_only = diff_graph_documents(after, moved);
    REQUIRE(layout_only.ok());
    CHECK(layout_only.equal());
    CHECK(layout_only.layout_changed.size() == 1);
}

TEST_CASE("graph document diff refuses documents without the identity envelope",
          "[host][graph][history][g3]") {
    SignalGraph graph;
    build_chain(graph);
    const auto saved = GraphSerializer::to_saved_document(graph);
    // A current-format document without the envelope is refused by the
    // inspector; an admitted profile document without it passes inspection
    // but still has no authored node identity to compare by.
    const auto unenveloped = R"({"format_version":5,"nodes":[],"connections":[]})";
    const auto missing = diff_graph_documents(unenveloped, saved);
    CHECK(missing.code == GraphDocumentDiffCode::LogicalIdentityMissing);
    CHECK(missing.refusal == GraphDocumentRefusal::LogicalIdentityMissing);
    CHECK_FALSE(missing.equal());

    const auto profile = R"({"document_schema":"pulp.graph.authored-io-gain.v1","nodes":[]})";
    REQUIRE(GraphSerializer::inspect_document(profile).ok());
    const auto no_identity = diff_graph_documents(saved, profile);
    CHECK(no_identity.code == GraphDocumentDiffCode::LogicalIdentityMissing);
    CHECK(no_identity.detail.starts_with("after: "));

    const auto legacy = diff_graph_documents(GraphSerializer::to_json(graph), saved);
    CHECK(legacy.code == GraphDocumentDiffCode::BeforeRefused);
    CHECK(legacy.refusal == GraphDocumentRefusal::InvalidFormatVersion);

    const auto broken_after = diff_graph_documents(saved, "{broken");
    CHECK(broken_after.code == GraphDocumentDiffCode::AfterRefused);
    CHECK(broken_after.refusal == GraphDocumentRefusal::InvalidJson);

    const auto broken_before = diff_graph_documents("", saved);
    CHECK(broken_before.code == GraphDocumentDiffCode::BeforeRefused);
    CHECK(std::string(graph_document_diff_code_name(broken_before.code)) == "BeforeRefused");
}

TEST_CASE("graph edit history undoes and redoes by value", "[host][graph][history][g3]") {
    SignalGraph graph;
    const auto chain = build_chain(graph);
    GraphEditHistory history;

    auto before = GraphEditHistory::checkpoint(graph);
    REQUIRE(before.valid());
    REQUIRE(graph.set_node_gain(chain.gain, 0.25f));
    const auto gain_edit = history.commit(std::move(before), graph, "set gain");
    REQUIRE(gain_edit.ok());

    before = GraphEditHistory::checkpoint(graph);
    const auto trim = graph.add_gain_node("Trim");
    REQUIRE(graph.disconnect(chain.gain, 0, chain.output, 0));
    REQUIRE(graph.connect(chain.gain, 0, trim, 0));
    REQUIRE(graph.connect(trim, 0, chain.output, 0));
    const auto insert = history.commit(std::move(before), graph, "insert trim");
    REQUIRE(insert.ok());
    CHECK(contains(insert.port_changes.added, "node:gain:Trim:0/in/"));
    CHECK(history.undo_depth() == 2);
    CHECK(history.undo_label() == "insert trim");

    const auto edited = GraphSerializer::to_saved_document(graph);

    const auto undone = history.undo(graph);
    INFO(undone.detail);
    REQUIRE(undone.ok());
    CHECK(undone.label == "insert trim");
    CHECK(node_named(graph, "Trim") == nullptr);
    CHECK(graph.connections().size() == 2);
    CHECK(contains(undone.port_changes.removed, "node:gain:Trim:0/"));
    CHECK(history.redo_label() == "insert trim");

    const auto undone_gain = history.undo(graph);
    REQUIRE(undone_gain.ok());
    REQUIRE(node_named(graph, "Gain") != nullptr);
    CHECK(node_named(graph, "Gain")->gain == 1.0f);
    CHECK(history.undo(graph).code == GraphHistoryCode::NothingToUndo);

    REQUIRE(history.redo(graph).ok());
    REQUIRE(history.redo(graph).ok());
    CHECK(history.redo(graph).code == GraphHistoryCode::NothingToRedo);
    CHECK(diff_graph_documents(edited, GraphSerializer::to_saved_document(graph)).equal());
    REQUIRE(node_named(graph, "Gain") != nullptr);
    CHECK(node_named(graph, "Gain")->gain == 0.25f);
}

TEST_CASE("graph edit history skips no-op edits and clears redo on a new edit",
          "[host][graph][history][g3]") {
    SignalGraph graph;
    const auto chain = build_chain(graph);
    GraphEditHistory history;

    const auto noop = history.commit(GraphEditHistory::checkpoint(graph), graph, "nothing");
    CHECK(noop.code == GraphHistoryCode::NoChange);
    CHECK_FALSE(history.can_undo());

    auto before = GraphEditHistory::checkpoint(graph);
    REQUIRE(graph.set_node_gain(chain.gain, 0.5f));
    REQUIRE(history.commit(std::move(before), graph, "first").ok());
    REQUIRE(history.undo(graph).ok());
    REQUIRE(history.can_redo());

    const auto gain = node_named(graph, "Gain")->id;
    before = GraphEditHistory::checkpoint(graph);
    REQUIRE(graph.set_node_gain(gain, 0.75f));
    REQUIRE(history.commit(std::move(before), graph, "second").ok());
    CHECK_FALSE(history.can_redo());

    CHECK(history.commit(GraphEditCheckpoint{}, graph, "invalid").code ==
          GraphHistoryCode::CaptureFailed);
}

TEST_CASE("graph edit history refuses to undo over an unrecorded edit",
          "[host][graph][history][g3]") {
    SignalGraph graph;
    const auto chain = build_chain(graph);
    GraphEditHistory history;

    auto before = GraphEditHistory::checkpoint(graph);
    REQUIRE(graph.set_node_gain(chain.gain, 0.5f));
    REQUIRE(history.commit(std::move(before), graph, "set gain").ok());

    // An edit made outside the history must not be silently discarded.
    const auto stray = graph.add_gain_node("Stray");
    REQUIRE(stray != 0);
    const auto refused = history.undo(graph);
    CHECK(refused.code == GraphHistoryCode::Diverged);
    CHECK(std::string(graph_history_code_name(refused.code)) == "Diverged");
    CHECK(node_named(graph, "Stray") != nullptr);
    CHECK(node_named(graph, "Gain")->gain == 0.5f);
    CHECK(history.can_undo());

    // Control: once the stray edit is gone, the same undo succeeds.
    REQUIRE(graph.remove_node(stray));
    REQUIRE(history.undo(graph).ok());
    CHECK(node_named(graph, "Gain")->gain == 1.0f);
}

TEST_CASE("graph edit history is bounded by capacity", "[host][graph][history][g3]") {
    SignalGraph graph;
    const auto chain = build_chain(graph);
    GraphEditHistory history(2);
    for (int i = 1; i <= 3; ++i) {
        auto before = GraphEditHistory::checkpoint(graph);
        REQUIRE(graph.set_node_gain(node_named(graph, "Gain")->id, 0.1f * static_cast<float>(i)));
        REQUIRE(history.commit(std::move(before), graph, "edit " + std::to_string(i)).ok());
    }
    (void)chain;
    CHECK(history.undo_depth() == 2);
    REQUIRE(history.undo(graph).ok());
    REQUIRE(history.undo(graph).ok());
    CHECK(history.undo(graph).code == GraphHistoryCode::NothingToUndo);
    // The oldest edit was dropped, so the graph stops at the first recorded state.
    CHECK(node_named(graph, "Gain")->gain == 0.1f);
}

TEST_CASE("qualified port identities are independent of NodeId numbering",
          "[host][graph][history][g3]") {
    SignalGraph first;
    build_chain(first);
    SignalGraph second;
    const auto spacer = second.add_gain_node("Spacer");
    build_chain(second);
    REQUIRE(second.remove_node(spacer));

    const auto a = qualified_port_identities(first);
    const auto b = qualified_port_identities(second);
    REQUIRE_FALSE(a.empty());
    REQUIRE(validate_port_identities(a).valid);
    CHECK(port_identity_fingerprint(a) == port_identity_fingerprint(b));
    CHECK(contains({a.front().name}, "node:"));

    // Control: renaming a node changes its qualified ports.
    SignalGraph third;
    third.add_input_node(1, "Input");
    third.add_gain_node("Renamed");
    third.add_output_node(1, "Output");
    CHECK(port_identity_fingerprint(qualified_port_identities(third)) !=
          port_identity_fingerprint(a));
}

TEST_CASE("graph edit history rolls back a restore that cannot be reproduced",
          "[host][graph][history][g3]") {
    const auto make_type = [](int outputs) {
        CustomNodeType type;
        type.type_id = "test.history-port-shape";
        type.version = 1;
        type.num_input_ports = 1;
        type.num_output_ports = outputs;
        type.process = [](pulp::audio::BufferView<float>&,
                          const pulp::audio::BufferView<const float>&, int) {};
        return type;
    };
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_type(1)));
    const auto input = graph.add_input_node(1, "Input");
    const auto shaper = graph.add_custom_node("test.history-port-shape", 1, "Shaper");
    const auto output = graph.add_output_node(1, "Output");
    REQUIRE(graph.connect(input, 0, shaper, 0));
    REQUIRE(graph.connect(shaper, 0, output, 0));

    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(graph);
    REQUIRE(graph.disconnect(shaper, 0, output, 0));
    REQUIRE(history.commit(std::move(before), graph, "unplug").ok());

    // The type now declares a different port set, so the recorded document
    // cannot be restored as captured. The graph returns to its pre-call state.
    REQUIRE(graph.register_custom_node_type(make_type(2)));
    const auto refused = history.undo(graph);
    INFO(refused.detail);
    CHECK(refused.code == GraphHistoryCode::RestoreFailed);
    CHECK(refused.rolled_back);
    CHECK(history.can_undo());
    CHECK(node_named(graph, "Shaper") != nullptr);
    CHECK(graph.connections().size() == 1);
}
