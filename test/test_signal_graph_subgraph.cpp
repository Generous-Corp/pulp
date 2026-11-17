#include "../core/host/src/graph_serializer_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/subgraph_persistence.hpp>
#include <pulp/host/voice_identity.hpp>
#include <pulp/format/processor.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace pulp::host;

namespace {
class TwoParamProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor value;
        value.name = "Two Param";
        value.manufacturer = "Pulp Test";
        value.bundle_id = "com.pulp.test.two-param";
        value.version = "2.0.0";
        value.output_buses = {{"Out", 1, false}};
        return value;
    }
    void define_parameters(pulp::state::StateStore& store) override {
        store.add_parameter(
            {.id = 1, .name = "one", .range = pulp::state::ParamRange::linear(0.0f, 2.0f, 1.0f)});
        store.add_parameter(
            {.id = 2, .name = "two", .range = pulp::state::ParamRange::linear(0.0f, 2.0f, 0.5f)});
    }
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
};

SubgraphTemplate two_param_template() {
    SubgraphTemplate t;
    t.identity = "test.two-param";
    SubgraphNodeSpec node;
    node.key = "processor";
    node.type = NodeType::Plugin;
    node.processor_type_id = "test.two-param";
    node.processor_type_version = 2;
    node.processor_descriptor_version = "2.0.0";
    node.processor_factory = [](std::uint32_t) -> std::unique_ptr<pulp::format::Processor> {
        return std::make_unique<TwoParamProcessor>();
    };
    t.nodes.push_back(std::move(node));
    t.parameters = {{"one", "processor", 1.0f, 0.0f, 2.0f, 1, "one"},
                    {"two", "processor", 0.5f, 0.0f, 2.0f, 2, "two"}};
    return t;
}

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

TEST_CASE("subgraph binds two distinct named Processor parameters transactionally") {
    SignalGraph graph;
    auto t = two_param_template();
    auto expanded = instantiate_subgraph(graph, t, "voice", 3);
    REQUIRE(expanded);
    CHECK(expanded.instance.processor_parameter_ids.at("one") == 1);
    CHECK(expanded.instance.processor_parameter_ids.at("two") == 2);
    auto state = expanded.instance.save_state();
    state.parameters["one"] = 1.5f;
    state.parameters["two"] = 0.75f;
    REQUIRE(restore_subgraph_state(graph, expanded.instance, state));
    auto* processor = graph.processor_node_instance(expanded.instance.nodes.front()).get();
    REQUIRE(processor);
    CHECK(processor->state_store().get_value(1) == 1.5f);
    CHECK(processor->state_store().get_value(2) == 0.75f);

    t.parameters[1].processor_parameter_id = 1;
    const auto duplicate = instantiate_subgraph(graph, t, "duplicate", 4);
    CHECK_FALSE(duplicate);
    CHECK(graph.nodes().size() == 1);
    t = two_param_template();
    t.parameters[1].maximum = 3.0f;
    const auto mismatched = instantiate_subgraph(graph, t, "mismatch", 5);
    CHECK_FALSE(mismatched);
    CHECK(graph.nodes().size() == 1);
}

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

namespace {

struct PersistedRack {
    SignalGraph graph;
    std::vector<SubgraphInstance> instances;
    std::string document;
};

// Two top-level instances (one nested), a node outside any instance, and an
// edge composed through exposed ports.
void build_rack(PersistedRack& rack) {
    auto voice = instantiate_subgraph(rack.graph, gain_template(), "voice.a", 1);
    auto nested = instantiate_subgraph(rack.graph, nested_gain_template(), "rack.b", 2);
    REQUIRE(voice);
    REQUIRE(nested);
    auto voice_state = voice.instance.save_state();
    voice_state.parameters.at("gain") = 0.5f;
    REQUIRE(restore_subgraph_state(rack.graph, voice.instance, voice_state));
    auto nested_state = nested.instance.save_state();
    nested_state.parameters.at("filter.gain") = 1.5f;
    REQUIRE(restore_subgraph_state(rack.graph, nested.instance, nested_state));
    REQUIRE(rack.graph.add_gain_node("loose") != 0);
    rack.instances = {voice.instance, nested.instance};
    rack.document = to_saved_subgraph_document(rack.graph, rack.instances);
    REQUIRE_FALSE(rack.document.empty());
}

std::vector<SubgraphTemplate> rack_templates() {
    return {gain_template(), nested_gain_template()};
}

// Tamper with the first match inside the subgraph records only, so a value
// that also appears in the flattened graph payload is never touched.
std::string replaced(std::string text, const std::string& from, const std::string& to) {
    const auto records = text.find("\"subgraphs\"");
    REQUIRE(records != std::string::npos);
    const auto at = text.find(from, records);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

// A refused load leaves no graph behind and no instance handles.
void require_refused(const std::string& document, const std::string& rule) {
    SignalGraph graph;
    const auto templates = rack_templates();
    const auto loaded = from_saved_subgraph_document(graph, document, templates);
    INFO(loaded.load.error);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.load.error.find(rule) != std::string::npos);
    CHECK(loaded.subgraphs.empty());
    CHECK(graph.nodes().empty());
    CHECK(loaded.load.identity_node_ids.empty());
    CHECK(loaded.load.persisted_node_ids.empty());
    CHECK(loaded.load.editor_layout.empty());
    CHECK(loaded.load.unrestored_node_ids.empty());
}

} // namespace

TEST_CASE("saved subgraph document restores instances with stable identities") {
    PersistedRack rack;
    build_rack(rack);
    const auto inspection = GraphSerializer::inspect_document(rack.document);
    REQUIRE(inspection.ok());

    SignalGraph restored;
    const auto templates = rack_templates();
    const auto loaded = from_saved_subgraph_document(restored, rack.document, templates);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.subgraphs.size() == 2);
    CHECK(restored.nodes().size() == rack.graph.nodes().size());

    const auto& nested = loaded.subgraphs[0];
    const auto& voice = loaded.subgraphs[1];
    CHECK(nested.instance_identity == "rack.b");
    CHECK(nested.template_identity == "test.rack");
    CHECK(nested.instance_index == 2);
    CHECK(nested.child_template_identities.at("filter") == "test.gain");
    CHECK(nested.child_instance_indices.at("filter") == 4);
    CHECK(nested.parameters.at("filter.gain") == 1.5f);
    CHECK(restored.node(nested.parameter_nodes.at("filter.gain"))->gain == 1.5f);
    CHECK(restored.node(nested.nodes.front())->name == "rack.b.filter.in");
    CHECK(voice.instance_identity == "voice.a");
    CHECK(voice.instance_index == 1);
    CHECK(voice.parameters.at("gain") == 0.5f);
    CHECK(restored.node(voice.parameter_nodes.at("gain"))->gain == 0.5f);
    CHECK(voice.parameter_bounds.at("gain").maximum == 2.0f);

    // Restored handles are live: ports compose and state restores in place.
    REQUIRE(connect_subgraph_ports(restored, voice, "input", nested, "output"));
    auto mutable_voice = voice;
    auto state = mutable_voice.save_state();
    state.parameters.at("gain") = 0.25f;
    REQUIRE(restore_subgraph_state(restored, mutable_voice, state));
    CHECK(restored.node(mutable_voice.parameter_nodes.at("gain"))->gain == 0.25f);

    // Re-saving the untouched restore reproduces the same semantic identity.
    SignalGraph again;
    const auto reload = from_saved_subgraph_document(again, rack.document, templates);
    REQUIRE(reload.ok());
    const auto resaved = to_saved_subgraph_document(again, reload.subgraphs);
    REQUIRE_FALSE(resaved.empty());
    const auto reinspected = GraphSerializer::inspect_document(resaved);
    REQUIRE(reinspected.ok());
    CHECK(reinspected.document.semantic_digest() == inspection.document.semantic_digest());

    // The flattened graph stays loadable by the plain saved-document reader.
    SignalGraph flat;
    CHECK(GraphSerializer::from_saved_document(flat, rack.document).ok);
    CHECK(flat.nodes().size() == rack.graph.nodes().size());
}

TEST_CASE("saved subgraph members are authored IDs and a second instance gets new ones") {
    PersistedRack rack;
    build_rack(rack);
    const auto root = choc::json::parse(rack.document);
    const auto records = root["subgraphs"];
    std::unordered_set<std::string> ids;
    for (const auto& node : rack.graph.nodes())
        ids.insert(node.authored_id.str());
    for (std::uint32_t i = 0; i < records.size(); ++i)
        for (std::uint32_t m = 0; m < records[i]["members"].size(); ++m)
            CHECK(ids.contains(std::string(records[i]["members"][m].getString())));

    // A second instance of the same template is a copy: same names under a
    // different instance identity, every node with a new authored ID.
    auto copy = instantiate_subgraph(rack.graph, gain_template(), "voice.b", 3);
    REQUIRE(copy);
    for (const auto node : copy.instance.nodes)
        CHECK(ids.insert(rack.graph.node(node)->authored_id.str()).second);

    // Restoring keeps every member's ID.
    SignalGraph restored;
    const auto loaded = from_saved_subgraph_document(restored, rack.document, rack_templates());
    REQUIRE(loaded.ok());
    for (const auto& instance : loaded.subgraphs)
        for (std::size_t i = 0; i < instance.nodes.size(); ++i) {
            const auto& original =
                instance.instance_identity == "voice.a" ? rack.instances[0] : rack.instances[1];
            CHECK(restored.node(instance.nodes[i])->authored_id.str() ==
                  rack.graph.node(original.nodes[i])->authored_id.str());
        }
}

TEST_CASE("saved subgraph documents written before v7 still restore") {
    PersistedRack rack;
    build_rack(rack);
    // Rebuild the document a v6 writer produced: the legacy envelope plus the
    // same records, with members spelled as structural v6 identities.
    const auto legacy_ids = GraphSerializerTestAccess::legacy_v6_identities(rack.graph);
    std::unordered_map<std::string, std::string> v6_of;
    for (const auto& node : rack.graph.nodes())
        v6_of.emplace(node.authored_id.str(), legacy_ids.at(node.id));
    const auto current = choc::json::parse(rack.document);
    auto root = choc::json::parse(GraphSerializerTestAccess::legacy_v6_saved_document(rack.graph));
    auto records = choc::value::createEmptyArray();
    for (std::uint32_t i = 0; i < current["subgraphs"].size(); ++i) {
        choc::value::Value record(current["subgraphs"][i]);
        auto members = choc::value::createEmptyArray();
        for (std::uint32_t m = 0; m < record["members"].size(); ++m)
            members.addArrayElement(v6_of.at(std::string(record["members"][m].getString())));
        record.setMember("members", members);
        records.addArrayElement(record);
    }
    root.addMember("subgraphs", records);
    const auto legacy = choc::json::toString(root, true);
    const auto inspection = GraphSerializer::inspect_document(legacy);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    REQUIRE(inspection.document.profile() == "pulp.graph.saved-identity.v6");

    SignalGraph restored;
    const auto loaded = from_saved_subgraph_document(restored, legacy, rack_templates());
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.subgraphs.size() == 2);
    CHECK(loaded.subgraphs[1].instance_identity == "voice.a");
    CHECK(restored.node(loaded.subgraphs[1].parameter_nodes.at("gain"))->gain == 0.5f);

    // A v6 member identity is derived from its name, so a v6 record whose
    // instance identity was renamed no longer matches the names it owns.
    {
        SignalGraph refused;
        const auto renamed =
            from_saved_subgraph_document(refused,
                                         replaced(legacy, "\"instance_identity\": \"voice.a\"",
                                                  "\"instance_identity\": \"voice.z\""),
                                         rack_templates());
        INFO(renamed.load.error);
        CHECK_FALSE(renamed.ok());
        CHECK(renamed.load.error.find("subgraph_instance_mismatch") != std::string::npos);
        CHECK(refused.nodes().empty());
    }

    // The data-only migration translates the members, and the migrated
    // document restores to the same IDs the v6 load assigned.
    const auto migrated = GraphSerializer::migrate_saved_document(legacy);
    INFO(migrated.detail);
    REQUIRE(migrated.ok());
    REQUIRE(migrated.migrated);
    SignalGraph from_migrated;
    const auto again =
        from_saved_subgraph_document(from_migrated, migrated.document, rack_templates());
    INFO(again.load.error);
    REQUIRE(again.ok());
    for (std::size_t s = 0; s < 2; ++s)
        for (std::size_t i = 0; i < again.subgraphs[s].nodes.size(); ++i)
            CHECK(from_migrated.node(again.subgraphs[s].nodes[i])->authored_id.str() ==
                  restored.node(loaded.subgraphs[s].nodes[i])->authored_id.str());
}

TEST_CASE("saved subgraph document refuses unknown or duplicate templates before mutation") {
    PersistedRack rack;
    build_rack(rack);
    SignalGraph graph;
    const auto existing = graph.add_gain_node("existing");
    REQUIRE(existing != 0);

    const std::vector<SubgraphTemplate> missing = {gain_template()};
    const auto unknown = from_saved_subgraph_document(graph, rack.document, missing);
    CHECK_FALSE(unknown.ok());
    CHECK(unknown.load.error.find("subgraph_template_unknown") != std::string::npos);
    CHECK(graph.nodes().size() == 1);
    CHECK(graph.node(existing) != nullptr);

    const std::vector<SubgraphTemplate> duplicated = {gain_template(), gain_template(),
                                                      nested_gain_template()};
    const auto duplicate = from_saved_subgraph_document(graph, rack.document, duplicated);
    CHECK_FALSE(duplicate.ok());
    CHECK(duplicate.load.error.find("subgraph_template_duplicate") != std::string::npos);
    CHECK(graph.nodes().size() == 1);
}

// The quoted authored ID of the node named `name`.
std::string quoted_id(const SignalGraph& graph, std::string_view name) {
    for (const auto& node : graph.nodes())
        if (node.name == name)
            return "\"" + node.authored_id.str() + "\"";
    FAIL("no node named " << name);
    return {};
}

TEST_CASE("saved subgraph inspection refuses dangling, shared, and malformed records") {
    PersistedRack rack;
    build_rack(rack);
    const auto dangling_member = replaced(rack.document, quoted_id(rack.graph, "voice.a.gain"),
                                          "\"nid-00000000000000000000000000000000\"");
    CHECK(GraphSerializer::inspect_document(dangling_member).refusal ==
          GraphDocumentRefusal::SubgraphBindingDangling);
    require_refused(dangling_member, "subgraph_binding_dangling");

    const auto shared = replaced(rack.document, quoted_id(rack.graph, "rack.b.filter.in"),
                                 quoted_id(rack.graph, "voice.a.in"));
    CHECK(GraphSerializer::inspect_document(shared).refusal ==
          GraphDocumentRefusal::SubgraphBindingDuplicate);
    require_refused(shared, "subgraph_binding_duplicate");

    const auto malformed = replaced(rack.document, "\"gain\": 0.5", "\"gain\": \"loud\"");
    CHECK(GraphSerializer::inspect_document(malformed).refusal ==
          GraphDocumentRefusal::SubgraphRecordInvalid);
    require_refused(malformed, "subgraph_record_invalid");

    const auto twin = replaced(rack.document, "\"instance_identity\": \"voice.a\"",
                               "\"instance_identity\": \"rack.b\"");
    CHECK(GraphSerializer::inspect_document(twin).refusal ==
          GraphDocumentRefusal::SubgraphBindingDuplicate);
}

TEST_CASE("saved subgraph restore refuses records that do not match their template") {
    PersistedRack rack;
    build_rack(rack);

    // Members bound in a different order no longer match the template
    // structure at their positions.
    {
        auto root = choc::json::parse(rack.document);
        const auto records = root["subgraphs"];
        auto rewritten = choc::value::createEmptyArray();
        for (std::uint32_t r = 0; r < records.size(); ++r) {
            choc::value::Value record(records[r]);
            if (record["instance_identity"].getString() == std::string_view("voice.a")) {
                const auto members = record["members"];
                auto swapped = choc::value::createEmptyArray();
                swapped.addArrayElement(members[2]);
                swapped.addArrayElement(members[1]);
                swapped.addArrayElement(members[0]);
                record.setMember("members", swapped);
            }
            rewritten.addArrayElement(record);
        }
        root.setMember("subgraphs", rewritten);
        require_refused(choc::json::toString(root, true), "does not match the template");
    }
    // Nested child identity and index are bound, not advisory.
    require_refused(replaced(rack.document, "\"template_identity\": \"test.gain\",\n",
                             "\"template_identity\": \"test.other\",\n"),
                    "subgraph_instance_mismatch");
    require_refused(replaced(rack.document, "\"instance_index\": 4", "\"instance_index\": 5"),
                    "subgraph_instance_mismatch");
    // Restoring under a different template identity is a different expansion.
    require_refused(replaced(rack.document, "\"template_identity\": \"test.rack\"",
                             "\"template_identity\": \"test.gain\""),
                    "subgraph_instance_mismatch");
    // Declared values are bounded by the template, not by the document.
    require_refused(replaced(rack.document, "\"gain\": 0.5", "\"gain\": 9.0"),
                    "subgraph_state_rejected");
    require_refused(replaced(rack.document, "\"gain\": 0.5", "\"gain\": 0.5,\n \"extra\": 1.0"),
                    "subgraph_state_rejected");
}

TEST_CASE("a template mismatch after the flattened load returns no graph, instances or maps") {
    PersistedRack rack;
    build_rack(rack);
    // Lay out every node so a stale editor_layout entry would be visible.
    std::unordered_map<NodeId, std::pair<float, float>> layout;
    for (const auto& node : rack.graph.nodes())
        layout.emplace(node.id, std::pair{static_cast<float>(node.id) * 10.0f, 5.0f});
    const auto document = to_saved_subgraph_document(rack.graph, rack.instances, layout);
    REQUIRE_FALSE(document.empty());
    const auto mismatched = replaced(document, "\"template_identity\": \"test.rack\"",
                                     "\"template_identity\": \"test.gain\"");

    SignalGraph graph;
    REQUIRE(graph.add_gain_node("existing") != 0);
    const auto refused = from_saved_subgraph_document(graph, mismatched, rack_templates());
    INFO(refused.load.error);
    CHECK_FALSE(refused.ok());
    CHECK(refused.load.error.find("subgraph_instance_mismatch") != std::string::npos);
    CHECK(graph.nodes().empty());
    CHECK(refused.subgraphs.empty());
    CHECK(refused.load.identity_node_ids.empty());
    CHECK(refused.load.persisted_node_ids.empty());
    CHECK(refused.load.editor_layout.empty());
    CHECK(refused.load.unrestored_node_ids.empty());
    CHECK(refused.load.missing_plugins.empty());

    // Control: the unedited document fills every map the refusal must empty.
    SignalGraph control;
    const auto loaded = from_saved_subgraph_document(control, document, rack_templates());
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    CHECK(loaded.load.identity_node_ids.size() == rack.graph.nodes().size());
    CHECK(loaded.load.persisted_node_ids.size() == rack.graph.nodes().size());
    CHECK(loaded.load.editor_layout.size() == rack.graph.nodes().size());
}

TEST_CASE("saved subgraph document restores a member whose display name changed") {
    PersistedRack rack;
    build_rack(rack);
    const auto& voice = rack.instances[0];
    const auto gain = voice.parameter_nodes.at("gain");
    REQUIRE(rack.graph.rename_node(gain, "Lead level"));
    const auto document = to_saved_subgraph_document(rack.graph, rack.instances);
    REQUIRE_FALSE(document.empty());

    SignalGraph restored;
    const auto loaded = from_saved_subgraph_document(restored, document, rack_templates());
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.subgraphs.size() == 2);
    const auto& again = loaded.subgraphs[1];
    REQUIRE(again.instance_identity == "voice.a");
    const auto* node = restored.node(again.parameter_nodes.at("gain"));
    REQUIRE(node != nullptr);
    CHECK(node->name == "Lead level");
    CHECK(node->gain == 0.5f);
    CHECK(node->authored_id.str() == rack.graph.node(gain)->authored_id.str());
    // The restored instance saves again with the rename intact.
    const auto resaved = to_saved_subgraph_document(restored, loaded.subgraphs);
    REQUIRE_FALSE(resaved.empty());
    SignalGraph twice;
    REQUIRE(from_saved_subgraph_document(twice, resaved, rack_templates()).ok());
}

TEST_CASE("saved subgraph restore refuses an instance whose internal edges changed") {
    PersistedRack rack;
    build_rack(rack);
    const auto& voice = rack.instances[0];
    REQUIRE(rack.graph.disconnect(voice.nodes[0], 0, voice.nodes[1], 0));
    const auto edited = to_saved_subgraph_document(rack.graph, rack.instances);
    REQUIRE_FALSE(edited.empty());
    require_refused(edited, "internal edges");
}

TEST_CASE("saved subgraph writer refuses stale, foreign, and duplicate instances") {
    PersistedRack rack;
    build_rack(rack);
    auto duplicate = rack.instances;
    duplicate[1].instance_identity = duplicate[0].instance_identity;
    CHECK(to_saved_subgraph_document(rack.graph, duplicate).empty());

    auto shared = rack.instances;
    shared[1].nodes.push_back(shared[0].nodes.front());
    CHECK(to_saved_subgraph_document(rack.graph, shared).empty());

    SignalGraph foreign;
    CHECK(to_saved_subgraph_document(foreign, rack.instances).empty());

    REQUIRE(rack.graph.remove_node(rack.instances[0].nodes.back()));
    CHECK(to_saved_subgraph_document(rack.graph, rack.instances).empty());
}

TEST_CASE("subgraph records require the saved-identity envelope") {
    // A legacy graph is already refused for its missing identity; a document
    // admitted under another identity profile must not carry records either.
    const std::string profile = R"({"document_schema": "pulp.graph.authored-io-gain.v1")";
    REQUIRE(GraphSerializer::inspect_document(profile + "}").ok());
    const auto inspection = GraphSerializer::inspect_document(profile + R"(, "subgraphs": []})");
    CHECK_FALSE(inspection.ok());
    CHECK(inspection.refusal == GraphDocumentRefusal::SubgraphRecordInvalid);
}

TEST_CASE("saved subgraph writer records live parameter values rather than the restore cache") {
    PersistedRack rack;
    build_rack(rack);
    const auto& voice = rack.instances[0];
    // An edit that bypasses restore_subgraph_state leaves the instance's
    // cached value at 0.5.
    REQUIRE(rack.graph.set_node_gain(voice.parameter_nodes.at("gain"), 0.25f));
    REQUIRE(voice.parameters.at("gain") == 0.5f);
    const auto document = to_saved_subgraph_document(rack.graph, rack.instances);
    REQUIRE_FALSE(document.empty());

    SignalGraph restored;
    const auto templates = rack_templates();
    const auto loaded = from_saved_subgraph_document(restored, document, templates);
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    const auto& restored_voice = loaded.subgraphs[1];
    REQUIRE(restored_voice.instance_identity == "voice.a");
    CHECK(restored_voice.parameters.at("gain") == 0.25f);
    CHECK(restored.node(restored_voice.parameter_nodes.at("gain"))->gain == 0.25f);

    // A live value outside the declared bounds cannot be restored, so the
    // writer refuses instead of writing a document that will not load.
    REQUIRE(rack.graph.set_node_gain(voice.parameter_nodes.at("gain"), 3.0f));
    CHECK(to_saved_subgraph_document(rack.graph, rack.instances).empty());
}

TEST_CASE("saved subgraph writer reads Processor parameters from the StateStore") {
    SignalGraph graph;
    auto expansion = instantiate_subgraph(graph, two_param_template(), "two.a");
    REQUIRE(expansion);
    const auto& instance = expansion.instance;
    const auto processor = graph.processor_node_instance(instance.parameter_nodes.at("two"));
    REQUIRE(processor);
    processor->state_store().set_value(2, 1.25f);
    REQUIRE(instance.parameters.at("two") == 0.5f);

    const auto document = to_saved_subgraph_document(graph, {&instance, 1});
    REQUIRE_FALSE(document.empty());
    const auto root = choc::json::parse(document);
    const auto number = [](const auto& value) {
        return value.isInt32() || value.isInt64() ? static_cast<double>(value.getInt64())
                                                  : value.getFloat64();
    };
    CHECK(number(root["subgraphs"][0]["parameters"]["two"]) == 1.25);
    CHECK(number(root["subgraphs"][0]["parameters"]["one"]) == 1.0);
}

TEST_CASE("saved subgraph writer refuses handles that predate a reload") {
    // A template with no declared parameters, so only the handle check can
    // notice that the held NodeIds now name other nodes.
    auto plain = gain_template();
    plain.identity = "test.plain";
    plain.parameters.clear();
    SignalGraph graph;
    const auto spacer = graph.add_gain_node("spacer");
    auto expansion = instantiate_subgraph(graph, plain, "plain.a");
    REQUIRE(expansion);
    REQUIRE(graph.add_gain_node("loose") != 0);
    REQUIRE(graph.remove_node(spacer));
    std::vector<SubgraphInstance> held{expansion.instance};
    const auto document = to_saved_subgraph_document(graph, held);
    REQUIRE_FALSE(document.empty());

    // Reloading renumbers every node down by one; each held NodeId still
    // exists but now names a different node.
    REQUIRE(GraphSerializer::from_saved_document(graph, document).ok);
    for (const auto node : held.front().nodes)
        REQUIRE(graph.node(node) != nullptr);
    CHECK(graph.node(held.front().nodes.front())->name != "plain.a.in");
    CHECK(to_saved_subgraph_document(graph, held).empty());

    // Control: the instance restored from the document is live and writes.
    SignalGraph restored;
    const std::vector<SubgraphTemplate> templates{plain};
    const auto loaded = from_saved_subgraph_document(restored, document, templates);
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    CHECK_FALSE(to_saved_subgraph_document(restored, loaded.subgraphs).empty());
}
