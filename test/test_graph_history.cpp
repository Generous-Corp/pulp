// Value-owned graph edit history and logical-identity document diff.

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/format/processor.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/custom_node_type.hpp>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/processor_restore.hpp>
#include <pulp/host/sample_region_authoring.hpp>
#include <pulp/host/sample_region_parameters.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <memory>
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

class HistoryLevel final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "HistoryLevel";
        d.manufacturer = "Pulp Test";
        d.bundle_id = "com.pulp.test.history-level";
        d.version = "1";
        d.input_buses = {{"In", 1, false}};
        d.output_buses = {{"Out", 1, false}};
        return d;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter({.id = 7, .name = "level", .range = {0, 2, 1}});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
};

const ProcessorTypeKey kHistoryLevel{"com.pulp.test.history-level", 1};

// Input -> HistoryLevel -> Output, plus an unconnected Gain "Trim" to edit.
struct ProcessorGraph {
    SignalGraph graph;
    NodeId processor = 0;
    NodeId trim = 0;
};

void build_processor_graph(ProcessorGraph& fixture) {
    const auto input = fixture.graph.add_input_node(1, "Input");
    fixture.processor = fixture.graph.add_processor_node(std::make_unique<HistoryLevel>(), "Level");
    const auto output = fixture.graph.add_output_node(1, "Output");
    fixture.trim = fixture.graph.add_gain_node("Trim");
    REQUIRE(fixture.processor != 0);
    REQUIRE(fixture.graph.connect(input, 0, fixture.processor, 0));
    REQUIRE(fixture.graph.connect(fixture.processor, 0, output, 0));
}

// A restore registry whose factory fails on the calls `fail` selects.
struct LevelRegistry {
    ProcessorRestoreRegistry registry;
    GraphLoadOptions options;
    int calls = 0;

    template <typename Fail> explicit LevelRegistry(Fail fail) {
        ProcessorRestoreDescriptor descriptor;
        descriptor.type = kHistoryLevel;
        descriptor.input_channels = 1;
        descriptor.output_channels = 1;
        descriptor.create = [this, fail]() -> std::shared_ptr<pulp::format::ProcessorNodeInstance> {
            ++calls;
            if (fail(calls))
                return nullptr;
            return pulp::format::ProcessorNodeInstance::create(std::make_unique<HistoryLevel>());
        };
        REQUIRE(registry.register_type(std::move(descriptor)) == ProcessorRestoreCode::Restored);
        options.processor_registry = &registry;
        options.processor_allowlist = {kHistoryLevel};
        options.include_default_plugin_roots = false;
    }
};

// Input -> six-node sample region -> Output, built after a spacer node that is
// then removed, so every region NodeId sits above a gap and a reload
// renumbers all of them.
struct RegionGraph {
    SignalGraph graph;
    std::unique_ptr<SampleRegionParameterOwner> parameters;
};

void build_region_graph(RegionGraph& fixture) {
    const auto spacer = fixture.graph.add_gain_node("Spacer");
    const auto input = fixture.graph.add_input_node(1, "Input");
    const auto output = fixture.graph.add_output_node(1, "Output");
    REQUIRE(fixture.graph.remove_node(spacer));
    REQUIRE(fixture.graph.connect(input, 0, output, 0));
    REQUIRE(fixture.graph.prepare(48000.0, 16));

    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->disconnect(input, 0, output, 0));
    REQUIRE(register_builtin_sample_region_types(*edit));
    const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto add = edit->add_custom_node("pulp.core.sample-region.add");
    const auto multiply = edit->add_custom_node("pulp.core.sample-region.multiply");
    const auto delay = edit->add_custom_node("pulp.core.unit-delay");
    const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");
    const auto parameter = edit->add_custom_node("pulp.core.sample-region.parameter");
    REQUIRE((region_input && add && multiply && delay && region_output && parameter));
    REQUIRE(edit->connect(input, 0, region_input, 0));
    REQUIRE(edit->connect(region_output, 0, output, 0));

    constexpr pulp::state::ParamID kParam = 771;
    SampleRegionDefinition definition;
    definition.region_id = 771;
    definition.members = {
        {region_input,
         "pulp.core.sample-region.input",
         1,
         {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {add, "pulp.core.sample-region.add", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {multiply, "pulp.core.sample-region.multiply", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {delay, "pulp.core.unit-delay", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {region_output,
         "pulp.core.sample-region.output",
         1,
         {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {parameter,
         "pulp.core.sample-region.parameter",
         1,
         {SampleKernelConfigKind::PromotedParameterId, kParam, 0.0f}},
    };
    definition.input_boundaries = {region_input};
    definition.output_boundaries = {region_output};
    SampleRegionPromotedParameter promoted;
    promoted.param_id = kParam;
    promoted.key = "amount";
    promoted.name = "Amount";
    promoted.unit = "ratio";
    promoted.range = pulp::state::ParamRange::linear(0.0f, 1.0f, 0.5f);
    promoted.bound_node_id = parameter;
    definition.promoted_parameters = {promoted};
    definition.limits.max_member_nodes = 63;
    definition.limits.max_internal_connections = 124;
    definition.limits.max_input_boundaries = 7;
    definition.limits.max_output_boundaries = 7;
    definition.limits.max_delay_nodes = 31;
    definition.limits.max_promoted_parameters = 15;
    definition.limits.max_state_bytes = 127;
    definition.limits.max_logical_boundary_bytes = 1'048'575;
    definition.limits.max_work_per_frame = 190;
    definition.limits.max_work_per_block = 3'129'343;
    REQUIRE(edit->declare_sample_region(definition).accepted);
    REQUIRE(edit->connect_in_sample_region(771, region_input, 0, add, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, delay, 0, multiply, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, parameter, 0, multiply, 1).accepted);
    REQUIRE(edit->connect_in_sample_region(771, multiply, 0, add, 1).accepted);
    REQUIRE(edit->connect_in_sample_region(771, add, 0, delay, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, delay, 0, region_output, 0).accepted);
    REQUIRE(edit->prove_sample_region(definition.region_id).accepted);
    fixture.parameters =
        SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(fixture.parameters);
    REQUIRE(edit->bind_sample_region_parameters(fixture.parameters->binding()).accepted);
    REQUIRE(edit->prepare(48000.0, 16) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
}

// Insert `text` right after the opening bracket of the first array named
// `member`.
std::string insert_into_array(std::string json, std::string_view member, std::string_view text) {
    const auto at = json.find("\"" + std::string(member) + "\"");
    REQUIRE(at != std::string::npos);
    const auto bracket = json.find('[', at);
    REQUIRE(bracket != std::string::npos);
    json.insert(bracket + 1, text);
    return json;
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

TEST_CASE("graph edit history undoes over a node that was already unresolved",
          "[host][graph][history]") {
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

    // The type now declares a different port set, so the live Shaper is
    // already unresolved. Restoring it unresolved loses nothing, so the
    // undo is not refused merely because the document names a missing type.
    REQUIRE(graph.register_custom_node_type(make_type(2)));
    const auto undone = history.undo(graph);
    INFO(undone.detail);
    CHECK(undone.ok());
    CHECK(node_named(graph, "Shaper") != nullptr);
    CHECK(graph.connections().size() == 2);
}

TEST_CASE("graph edit history refuses a restore that would leave a node unresolved",
          "[host][graph][history]") {
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
    REQUIRE(graph.remove_node(shaper));
    REQUIRE(history.commit(std::move(before), graph, "remove shaper").ok());
    const auto input_ports = graph.node(input)->output_ports;

    // The type now declares a different port set, so undo would bring the
    // Shaper back as an unresolved placeholder. The trial restore refuses it
    // and the live graph is never touched.
    REQUIRE(graph.register_custom_node_type(make_type(2)));
    const auto refused = history.undo(graph);
    INFO(refused.detail);
    CHECK(refused.code == GraphHistoryCode::RestoreFailed);
    CHECK(refused.graph_untouched);
    CHECK(refused.rolled_back);
    CHECK(refused.node_ids.empty());
    CHECK(history.can_undo());
    CHECK(node_named(graph, "Shaper") == nullptr);
    REQUIRE(graph.node(input) != nullptr);
    // Untouched, not reloaded: the input's ports keep their incarnation.
    CHECK(graph.node(input)->output_ports.front().incarnation == input_ports.front().incarnation);

    // Control: with the recorded port shape registered again, undo restores.
    REQUIRE(graph.register_custom_node_type(make_type(1)));
    const auto undone = history.undo(graph);
    INFO(undone.detail);
    CHECK(undone.ok());
    CHECK(node_named(graph, "Shaper") != nullptr);
}

TEST_CASE("graph edit history never wipes a Processor graph it cannot restore",
          "[host][graph][history]") {
    ProcessorGraph fixture;
    build_processor_graph(fixture);
    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(fixture.graph);
    REQUIRE(before.valid());
    REQUIRE(fixture.graph.set_node_gain(fixture.trim, 0.5f));
    REQUIRE(history.commit(std::move(before), fixture.graph, "trim").ok());
    const auto live_processor = fixture.graph.processor_node_instance(fixture.processor);
    REQUIRE(live_processor);
    const auto level_identity =
        GraphSerializer::saved_node_identities(fixture.graph).at(fixture.processor);

    // No ProcessorRestoreRegistry: the Processor record cannot be restored.
    const auto refused = history.undo(fixture.graph);
    INFO(refused.detail);
    CHECK(refused.code == GraphHistoryCode::RestoreFailed);
    CHECK(refused.graph_untouched);
    CHECK(refused.rolled_back);
    CHECK(fixture.graph.nodes().size() == 4);
    CHECK(fixture.graph.processor_node_instance(fixture.processor) == live_processor);
    CHECK(fixture.graph.node(fixture.trim)->gain == 0.5f);
    CHECK(history.can_undo());

    // Control: the same undo with a registry restores the recorded state.
    LevelRegistry registry([](int) { return false; });
    const auto undone = history.undo(fixture.graph, registry.options);
    INFO(undone.detail);
    REQUIRE(undone.ok());
    CHECK(fixture.graph.nodes().size() == 4);
    const auto level = undone.node_ids.find(level_identity);
    REQUIRE(level != undone.node_ids.end());
    CHECK(fixture.graph.processor_node_instance(level->second) != nullptr);
    REQUIRE(node_named(fixture.graph, "Trim") != nullptr);
    CHECK(node_named(fixture.graph, "Trim")->gain == 1.0f);
}

TEST_CASE("graph edit history reports rolled_back only for a faithful rollback",
          "[host][graph][history]") {
    ProcessorGraph fixture;
    build_processor_graph(fixture);
    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(fixture.graph);
    REQUIRE(fixture.graph.set_node_gain(fixture.trim, 0.5f));
    REQUIRE(history.commit(std::move(before), fixture.graph, "trim").ok());

    SECTION("the live restore fails and the rollback restores the pre-call graph") {
        // Call 1 is the trial restore, call 2 the live restore, call 3 the
        // rollback.
        LevelRegistry registry([](int call) { return call == 2; });
        const auto refused = history.undo(fixture.graph, registry.options);
        INFO(refused.detail);
        CHECK(registry.calls == 3);
        CHECK(refused.code == GraphHistoryCode::RestoreFailed);
        CHECK_FALSE(refused.graph_untouched);
        CHECK(refused.rolled_back);
        CHECK(fixture.graph.nodes().size() == 4);
        REQUIRE(node_named(fixture.graph, "Trim") != nullptr);
        CHECK(node_named(fixture.graph, "Trim")->gain == 0.5f);
        CHECK(refused.node_ids.at("node:gain:Trim:0") == node_named(fixture.graph, "Trim")->id);
        CHECK(history.can_undo());
    }
    SECTION("the rollback fails too and says so") {
        LevelRegistry registry([](int call) { return call >= 2; });
        const auto refused = history.undo(fixture.graph, registry.options);
        INFO(refused.detail);
        CHECK(registry.calls == 3);
        CHECK(refused.code == GraphHistoryCode::RestoreFailed);
        CHECK_FALSE(refused.graph_untouched);
        CHECK_FALSE(refused.rolled_back);
        CHECK(history.can_undo());
    }
}

TEST_CASE("graph edit history maps authored identities to renumbered NodeIds",
          "[host][graph][history]") {
    SignalGraph graph;
    const auto spacer = graph.add_gain_node("Spacer");
    const auto chain = build_chain(graph);
    REQUIRE(graph.remove_node(spacer));
    const auto identities = GraphSerializer::saved_node_identities(graph);

    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(graph);
    REQUIRE(graph.set_node_gain(chain.gain, 0.5f));
    REQUIRE(history.commit(std::move(before), graph, "gain").ok());
    const auto undone = history.undo(graph);
    REQUIRE(undone.ok());

    // The reload renumbers densely, so the held handle is stale; the result
    // says where each authored node went.
    const auto& gain_identity = identities.at(chain.gain);
    const auto moved = undone.node_ids.find(gain_identity);
    REQUIRE(moved != undone.node_ids.end());
    CHECK(moved->second != chain.gain);
    REQUIRE(graph.node(moved->second) != nullptr);
    CHECK(graph.node(moved->second)->name == "Gain");
    CHECK(undone.node_ids.size() == graph.nodes().size());
}

TEST_CASE("graph document diff digests ignore NodeId numbering", "[host][graph][history]") {
    SignalGraph first;
    build_chain(first);
    SignalGraph second;
    const auto spacer = second.add_gain_node("Spacer");
    const auto chain = build_chain(second);
    REQUIRE(second.remove_node(spacer));
    const auto a = GraphSerializer::to_saved_document(first);
    const auto b = GraphSerializer::to_saved_document(second);

    const auto same = diff_graph_documents(a, b);
    REQUIRE(same.equal());
    CHECK(same.before_digest.rfind("sha256:", 0) == 0);
    CHECK(same.before_digest == same.after_digest);

    // Control: a semantic change moves the digest.
    REQUIRE(second.set_node_gain(chain.gain, 0.5f));
    const auto changed = diff_graph_documents(a, GraphSerializer::to_saved_document(second));
    REQUIRE(changed.ok());
    CHECK_FALSE(changed.equal());
    CHECK(changed.before_digest != changed.after_digest);
}

TEST_CASE("graph document diff names the side a malformed record came from",
          "[host][graph][history]") {
    SignalGraph graph;
    build_chain(graph);
    const auto saved = GraphSerializer::to_saved_document(graph);
    // A connection that is not an object passes inspection but cannot be read.
    const auto broken = insert_into_array(saved, "connections", "1,");
    REQUIRE(GraphSerializer::inspect_document(broken).ok());

    const auto after = diff_graph_documents(saved, broken);
    CHECK(after.code == GraphDocumentDiffCode::AfterRefused);
    CHECK(after.detail.rfind("after: ", 0) == 0);
    const auto before = diff_graph_documents(broken, saved);
    CHECK(before.code == GraphDocumentDiffCode::BeforeRefused);
    CHECK(before.detail.rfind("before: ", 0) == 0);
}

TEST_CASE("saved document inspection refuses node ids the loader refuses",
          "[host][graph][history]") {
    SignalGraph graph;
    build_chain(graph);
    const auto saved = GraphSerializer::to_saved_document(graph);
    REQUIRE(GraphSerializer::inspect_document(saved).ok());
    const auto with_id = [&](std::string_view from, std::string_view to) {
        auto text = saved;
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return text;
    };
    const std::vector<std::string> bad = {
        with_id("\"id\": 2,", "\"id\": 1,"),          // duplicate
        with_id("\"id\": 1,", "\"id\": 0,"),          // zero
        with_id("\"id\": 1,", "\"id\": 4294967297,"), // wraps to 1 as a NodeId
        with_id("\"id\": 1,", "\"id\": 1.5,"),        // not an integer
    };
    for (const auto& document : bad) {
        const auto inspection = GraphSerializer::inspect_document(document);
        CHECK(inspection.refusal == GraphDocumentRefusal::NodeIdInvalid);
        CHECK(std::string(graph_document_refusal_name(inspection.refusal)) == "node_id_invalid");
        CHECK(diff_graph_documents(saved, document).code == GraphDocumentDiffCode::AfterRefused);

        // Refused before the caller's graph is cleared.
        SignalGraph target;
        build_chain(target);
        CHECK_FALSE(GraphSerializer::from_saved_document(target, document).ok);
        CHECK(target.nodes().size() == 3);
    }
}

TEST_CASE("saved document restore binds each identity to its persisted node",
          "[host][graph][history]") {
    SignalGraph graph;
    const auto first = graph.add_gain_node("G");
    const auto second = graph.add_gain_node("G");
    REQUIRE(graph.set_node_gain(first, 0.5f));
    const auto saved = GraphSerializer::to_saved_document(graph);

    // Control: the document as written restores, with the ordinals intact.
    SignalGraph loaded;
    const auto ok = GraphSerializer::from_saved_document(loaded, saved);
    REQUIRE(ok.ok);
    CHECK(ok.persisted_node_ids.at(first) != 0);
    CHECK(loaded.node(ok.persisted_node_ids.at(first))->gain == 0.5f);

    // Swap the two node records: the loader numbers nodes in array order, so
    // node:gain:G:0 would come back bound to the 1.0 gain node.
    auto root = choc::json::parse(saved);
    auto nodes = choc::value::createEmptyArray();
    nodes.addArrayElement(choc::value::Value(root["nodes"][1]));
    nodes.addArrayElement(choc::value::Value(root["nodes"][0]));
    root.setMember("nodes", nodes);
    const auto swapped = choc::json::toString(root, true);
    REQUIRE(GraphSerializer::inspect_document(swapped).ok());
    (void)second;

    SignalGraph target;
    const auto refused = GraphSerializer::from_saved_document(target, swapped);
    INFO(refused.error);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.find("identity_binding_unknown") != std::string::npos);
    CHECK(target.nodes().empty());
}

// Every node kind the saved-identity writer records: built-in nodes, two
// nodes that share one identity key, registered and unresolved custom nodes,
// an unresolved plugin whose unique id contains the delimiter, and a
// Processor node.
struct EveryKindGraph {
    SignalGraph graph;
    NodeId input = 0;
    NodeId gain = 0;
    NodeId custom = 0;
};

constexpr std::string_view kKindType = "pulp.test.identity-kind";

CustomNodeType kind_type() {
    CustomNodeType type;
    type.type_id = std::string(kKindType);
    type.version = 3;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.process = [](auto&, const auto&, int) {};
    return type;
}

void build_every_kind(EveryKindGraph& fixture) {
    auto& graph = fixture.graph;
    fixture.input = graph.add_input_node(1, "Input");
    fixture.gain = graph.add_gain_node("G");
    REQUIRE(graph.add_gain_node("G") != 0);
    REQUIRE(graph.add_output_node(1, "Output") != 0);
    REQUIRE(graph.add_midi_input_node("MIDI In") != 0);
    REQUIRE(graph.add_midi_output_node("MIDI Out") != 0);
    REQUIRE(graph.register_custom_node_type(kind_type()));
    fixture.custom = graph.add_custom_node(kKindType, 3, "Kind");
    REQUIRE(fixture.custom != 0);
    REQUIRE(graph.add_unresolved_custom_node("pulp.test.absent", 2, 1, 1, "Absent") != 0);
    PluginInfo info;
    info.name = "External";
    info.unique_id = "vendor:external:1";
    info.format = PluginFormat::VST3;
    REQUIRE(graph.add_unresolved_plugin_node(info, 2, 2, "External") != 0);
    REQUIRE(graph.add_processor_node(std::make_unique<HistoryLevel>(), "Level") != 0);
    REQUIRE(graph.connect(fixture.input, 0, fixture.gain, 0));
}

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

TEST_CASE("saved document inspection rebuilds identities for every node kind",
          "[host][graph][history][identity]") {
    EveryKindGraph fixture;
    build_every_kind(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    REQUIRE_FALSE(saved.empty());
    const auto inspection = GraphSerializer::inspect_document(saved);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    CHECK(diff_graph_documents(saved, saved).equal());

    // The writer's own identities survive a restore and a second save.
    LevelRegistry registry([](int) { return false; });
    registry.options.load_plugins = false;
    SignalGraph loaded;
    REQUIRE(loaded.register_custom_node_type(kind_type()));
    const auto restored = GraphSerializer::from_saved_document(loaded, saved, registry.options);
    INFO(restored.error);
    REQUIRE(restored.ok);
    const auto resaved = GraphSerializer::to_saved_document(loaded);
    REQUIRE(GraphSerializer::inspect_document(resaved).ok());
    const auto diff = diff_graph_documents(saved, resaved);
    INFO(diff.detail);
    REQUIRE(diff.ok());
    CHECK(diff.nodes_added.empty());
    CHECK(diff.nodes_removed.empty());
}

TEST_CASE("saved document inspection refuses identities that name other nodes",
          "[host][graph][history][identity]") {
    EveryKindGraph fixture;
    build_every_kind(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    const auto identities = GraphSerializer::saved_node_identities(fixture.graph);
    const auto& input = identities.at(fixture.input);
    const auto& gain = identities.at(fixture.gain);
    const auto& custom = identities.at(fixture.custom);

    // Swap the identity strings of two nodes of different types everywhere
    // they appear, so the identity entries and bindings stay consistent. The
    // envelope is read from one parse and written into another, so no view
    // into a value being modified is held.
    const auto original = choc::json::parse(saved);
    auto root = choc::json::parse(saved);
    const auto swapped_name = [&](const std::string& name) {
        return name == input ? gain : name == gain ? input : name;
    };
    const auto listed = original["logical_identities"]["identities"];
    auto entries = choc::value::createEmptyArray();
    for (std::uint32_t i = 0; i < listed.size(); ++i) {
        auto item = choc::value::createObject("Identity");
        item.addMember("id", swapped_name(std::string(listed[i]["id"].getString())));
        item.addMember("node_id", listed[i]["node_id"].getInt64());
        entries.addArrayElement(item);
    }
    auto logical = choc::value::createObject("LogicalIdentities");
    logical.addMember("profile",
                      std::string(original["logical_identities"]["profile"].getString()));
    logical.addMember("identities", entries);
    root.setMember("logical_identities", logical);
    const auto bound = original["bindings"];
    auto bindings = choc::value::createObject("Bindings");
    for (std::uint32_t i = 0; i < bound.size(); ++i) {
        const auto member = bound.getObjectMemberAt(i);
        bindings.addMember(swapped_name(std::string(member.name)), member.value.getInt64());
    }
    root.setMember("bindings", bindings);
    const auto swapped = choc::json::toString(root, true);

    // A custom record whose version no longer matches its identity, and one
    // whose version the loader cannot restore.
    REQUIRE(saved.find("\"version\": 3") != std::string::npos);
    const auto bumped = replace_once(saved, "\"version\": 3", "\"version\": 4");
    const auto zero = replace_once(saved, "\"version\": 3", "\"version\": 0");
    REQUIRE(custom.find("@3:") != std::string::npos);

    for (const auto* document : {&swapped, &bumped, &zero}) {
        const auto inspection = GraphSerializer::inspect_document(*document);
        INFO(inspection.detail);
        CHECK(inspection.refusal == GraphDocumentRefusal::IdentityBindingUnknown);
        const auto diff = diff_graph_documents(saved, *document);
        CHECK(diff.code == GraphDocumentDiffCode::AfterRefused);
        CHECK(diff.refusal == GraphDocumentRefusal::IdentityBindingUnknown);

        // Refused before the caller's graph is cleared.
        SignalGraph target;
        build_chain(target);
        const auto refused = GraphSerializer::from_saved_document(target, *document);
        CHECK_FALSE(refused.ok);
        CHECK(refused.error.find("identity_binding_unknown") != std::string::npos);
        CHECK(target.nodes().size() == 3);
    }
}

TEST_CASE("saved document inspection rebuilds identities the loader would assign",
          "[host][graph][history][identity]") {
    // Hand-written records the writer never emits but the loader restores:
    // an unknown type string restores as a custom node of that type at
    // version 1, a custom record without a version reads as version 1, and an
    // unnamed custom node takes its type id as its name.
    const std::string document = R"({
  "format_version": 2,
  "nodes": [
    {"id": 1, "type": "pulp.test.unknown", "name": "", "num_input_ports": 1,
     "num_output_ports": 1, "gain": 1.0},
    {"id": 2, "type": "custom", "name": "C", "num_input_ports": 1, "num_output_ports": 1,
     "gain": 1.0, "custom": {"type_id": "pulp.test.unversioned"}}
  ],
  "connections": [],
  "identity_schema": "pulp.graph.saved-identity.v6",
  "logical_identities": {
    "profile": "pulp.graph.saved-identity.v6",
    "identities": [
      {"id": "node:custom:pulp.test.unknown:pulp.test.unknown@1:0", "node_id": 1},
      {"id": "node:custom:C:pulp.test.unversioned@1:0", "node_id": 2}
    ]
  },
  "bindings": {
    "node:custom:pulp.test.unknown:pulp.test.unknown@1:0": 1,
    "node:custom:C:pulp.test.unversioned@1:0": 2
  }
})";
    const auto inspection = GraphSerializer::inspect_document(document);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    SignalGraph loaded;
    const auto restored = GraphSerializer::from_saved_document(loaded, document);
    INFO(restored.error);
    CHECK(restored.ok);
}

TEST_CASE("graph document diff compares sample region NodeIds by identity",
          "[host][graph][history][sample-region]") {
    RegionGraph fixture;
    build_region_graph(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    REQUIRE(saved.find("sample_regions") != std::string::npos);

    SignalGraph reloaded;
    REQUIRE(register_builtin_sample_region_types(reloaded));
    const auto loaded = GraphSerializer::from_saved_document(reloaded, saved);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    const auto renumbered = GraphSerializer::to_saved_document(reloaded);
    // Control: the reload really renumbered the region's members.
    REQUIRE(renumbered != saved);

    const auto diff = diff_graph_documents(saved, renumbered);
    INFO(diff.detail);
    REQUIRE(diff.ok());
    CHECK(diff.sections_changed.empty());
    CHECK(diff.equal());
    CHECK(diff.before_digest == diff.after_digest);
}

TEST_CASE("graph edit history redoes over a sample region after renumbering",
          "[host][graph][history][sample-region]") {
    RegionGraph fixture;
    build_region_graph(fixture);
    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(fixture.graph);
    REQUIRE(before.valid());
    REQUIRE(fixture.graph.add_gain_node("Trim") != 0);
    REQUIRE(history.commit(std::move(before), fixture.graph, "add trim").ok());

    const auto undone = history.undo(fixture.graph);
    INFO(undone.detail);
    REQUIRE(undone.ok());
    CHECK(node_named(fixture.graph, "Trim") == nullptr);
    const auto redone = history.redo(fixture.graph);
    INFO(redone.detail);
    REQUIRE(redone.ok());
    CHECK(node_named(fixture.graph, "Trim") != nullptr);
    CHECK(fixture.graph.sample_regions().size() == 1);
    const auto again = history.undo(fixture.graph);
    INFO(again.detail);
    CHECK(again.ok());
}
