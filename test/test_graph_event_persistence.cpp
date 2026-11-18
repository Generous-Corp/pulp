// Graph event connections in the saved-identity v7 document: the writer,
// inspection, the load-time rebind through connect_graph_event, the document
// diff and edit history, and subgraph templates that declare one.

#include "../core/host/src/graph_serializer_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/format/processor.hpp>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::host;

namespace {

constexpr const char* kEventType = "test.persist.graph-event";
constexpr std::uint16_t kPayload = 77;

// A pass-through Custom type that consumes events. As a producer it emits
// kPayload at the first sample of each block; as a consumer it writes the
// payload of any received event into the first output sample, so a render
// shows whether the event connection is bound.
CustomNodeType event_type(std::shared_ptr<std::uint64_t> next_sample) {
    CustomNodeType type;
    type.type_id = kEventType;
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [next_sample](auto& out, const auto& in, int frames,
                                        const CustomNodeEventBlock& events) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        if (events.graph_writer != nullptr) {
            REQUIRE(events.graph_writer->emit_at(*next_sample, kPayload) ==
                    GraphEventResult::Accepted);
            *next_sample += static_cast<std::uint64_t>(frames);
        }
        if (events.graph_reader != nullptr && !events.graph_reader->events().empty())
            out.channel_ptr(0)[0] = static_cast<float>(events.graph_reader->events()[0].payload);
    };
    return type;
}

struct EventGraph {
    std::shared_ptr<std::uint64_t> next_sample = std::make_shared<std::uint64_t>(0);
    SignalGraph graph;
    NodeId input = 0, producer = 0, consumer = 0, output = 0;
};

void build(EventGraph& f, bool connect_event = true) {
    REQUIRE(f.graph.register_custom_node_type(event_type(f.next_sample)));
    f.input = f.graph.add_input_node(1, "Input");
    f.producer = f.graph.add_custom_node(kEventType, 1, "Producer");
    f.consumer = f.graph.add_custom_node(kEventType, 1, "Consumer");
    f.output = f.graph.add_output_node(1, "Output");
    REQUIRE(f.graph.connect(f.input, 0, f.producer, 0));
    REQUIRE(f.graph.connect(f.producer, 0, f.consumer, 0));
    REQUIRE(f.graph.connect(f.consumer, 0, f.output, 0));
    if (connect_event)
        REQUIRE(f.graph.connect_graph_event({f.producer, f.consumer}) ==
                GraphEventBindingResult::Accepted);
}

// An empty graph that carries the event type, ready to load into.
struct Target {
    std::shared_ptr<std::uint64_t> next_sample = std::make_shared<std::uint64_t>(0);
    SignalGraph graph;
    Target() {
        REQUIRE(graph.register_custom_node_type(event_type(next_sample)));
    }
};

float render_first_sample(SignalGraph& graph, std::uint64_t& next_sample) {
    next_sample = 0;
    REQUIRE(graph.prepare(48000.0, 4));
    std::array<float, 4> source{};
    std::array<float, 4> destination{};
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    pulp::audio::BufferView<const float> in(&source_ptr, 1, source.size());
    pulp::audio::BufferView<float> out(&destination_ptr, 1, destination.size());
    graph.process(out, in, 4);
    return destination[0];
}

const GraphNode* named(const SignalGraph& graph, std::string_view name) {
    for (const auto& node : graph.nodes())
        if (node.name == name)
            return &node;
    return nullptr;
}

std::string authored(const SignalGraph& graph, NodeId id) {
    const auto* node = graph.node(id);
    REQUIRE(node != nullptr);
    return node->authored_id.str();
}

choc::value::Value event_record(const std::string& producer, const std::string& consumer,
                                std::int64_t source_port = 0, std::int64_t destination_port = 0,
                                std::int64_t kind = 1) {
    auto record = choc::value::createObject("EventConnection");
    record.addMember("producer", producer);
    record.addMember("consumer", consumer);
    record.addMember("source_port", source_port);
    record.addMember("destination_port", destination_port);
    record.addMember("kind", kind);
    return record;
}

// The document with its event_connections replaced by `records` (removed
// when `records` is empty).
std::string with_events(const std::string& document,
                        const std::vector<choc::value::Value>& records) {
    auto root = choc::json::parse(document);
    auto events = choc::value::createEmptyArray();
    for (const auto& record : records)
        events.addArrayElement(record);
    auto copy = choc::value::createObject("Graph");
    for (std::uint32_t i = 0; i < root.size(); ++i) {
        const auto member = root.getObjectMemberAt(i);
        if (std::string_view(member.name) != "event_connections")
            copy.addMember(member.name, member.value);
    }
    if (!records.empty())
        copy.addMember("event_connections", events);
    return choc::json::toString(copy, true);
}

// A graph that already holds something, to prove a refusal left it alone.
struct Sentinel {
    Target target;
    NodeId gain = target.graph.add_gain_node("Sentinel");
    bool untouched() const {
        return target.graph.nodes().size() == 1 && named(target.graph, "Sentinel") != nullptr;
    }
};

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

} // namespace

TEST_CASE("a graph event connection round-trips through the saved-identity v7 document",
          "[graph-events][persistence]") {
    EventGraph source;
    build(source);
    const auto document = GraphSerializer::to_saved_document(source.graph);
    REQUIRE_FALSE(document.empty());
    const auto root = choc::json::parse(document);
    REQUIRE(root.hasObjectMember("event_connections"));
    const auto& events = root["event_connections"];
    REQUIRE(events.size() == 1);
    CHECK(events[0]["producer"].getString() == authored(source.graph, source.producer));
    CHECK(events[0]["consumer"].getString() == authored(source.graph, source.consumer));
    CHECK(events[0]["source_port"].getInt64() == 0);
    CHECK(events[0]["destination_port"].getInt64() == 0);
    CHECK(events[0]["kind"].getInt64() == 1);
    REQUIRE(GraphSerializer::inspect_document(document).ok());

    Target restored;
    const auto load = GraphSerializer::from_saved_document(restored.graph, document);
    REQUIRE(load.ok);
    const auto edge = restored.graph.graph_event_edge();
    REQUIRE(edge.has_value());
    CHECK(edge->producer == load.identity_node_ids.at(authored(source.graph, source.producer)));
    CHECK(edge->consumer == load.identity_node_ids.at(authored(source.graph, source.consumer)));
    CHECK(edge->kind == 1);
    // The rebound connection carries events at render time.
    CHECK(render_first_sample(restored.graph, *restored.next_sample) == float(kPayload));
    // Re-saving the restored graph reproduces the document exactly.
    CHECK(GraphSerializer::to_saved_document(restored.graph) == document);

    // Positive control for the oracle: the same graph without the event
    // connection writes no record and renders no event.
    EventGraph plain;
    build(plain, false);
    const auto plain_document = GraphSerializer::to_saved_document(plain.graph);
    CHECK_FALSE(choc::json::parse(plain_document).hasObjectMember("event_connections"));
    Target unbound;
    REQUIRE(GraphSerializer::from_saved_document(unbound.graph, plain_document).ok);
    CHECK_FALSE(unbound.graph.graph_event_edge().has_value());
    CHECK(render_first_sample(unbound.graph, *unbound.next_sample) == 0.0f);
}

TEST_CASE("formats that cannot name event endpoints refuse a graph that holds one",
          "[graph-events][persistence]") {
    EventGraph source;
    build(source);
    CHECK(GraphSerializer::to_json(source.graph).empty());
    CHECK(GraphSerializerTestAccess::legacy_v6_saved_document(source.graph).empty());
    const auto path =
        std::filesystem::temp_directory_path() / "pulp-graph-event-persistence.pulpgraph";
    std::filesystem::remove(path);
    const auto saved = GraphSerializer::to_file(source.graph, path);
    CHECK_FALSE(saved.ok);
    CHECK(saved.code == GraphSerializer::SaveResult::Code::serialization_refused);
    CHECK_FALSE(std::filesystem::exists(path));
    // Positive control: the saved-identity file keeps it.
    REQUIRE(GraphSerializer::to_saved_file(source.graph, path).ok);
    Target restored;
    REQUIRE(GraphSerializer::from_file(restored.graph, path).ok);
    CHECK(restored.graph.graph_event_edge().has_value());
    std::filesystem::remove(path);
}

TEST_CASE("v6 and plain documents still load and cannot smuggle an event connection",
          "[graph-events][persistence][compat]") {
    EventGraph source;
    build(source, false);
    const auto legacy = GraphSerializerTestAccess::legacy_v6_saved_document(source.graph);
    REQUIRE_FALSE(legacy.empty());
    REQUIRE(GraphSerializer::inspect_document(legacy).ok());
    Target from_legacy;
    const auto legacy_load = GraphSerializer::from_saved_document(from_legacy.graph, legacy);
    REQUIRE(legacy_load.ok);
    CHECK(from_legacy.graph.nodes().size() == 4);
    CHECK_FALSE(from_legacy.graph.graph_event_edge().has_value());
    const auto plain = GraphSerializer::to_json(source.graph);
    REQUIRE_FALSE(plain.empty());
    Target from_plain;
    REQUIRE(GraphSerializer::from_json(from_plain.graph, plain).ok);
    CHECK(from_plain.graph.nodes().size() == 4);

    // A v6 identity names nodes structurally, not by authored ID.
    const auto identities = GraphSerializerTestAccess::legacy_v6_identities(source.graph);
    const auto smuggled_v6 = with_events(
        legacy, {event_record(identities.at(source.producer), identities.at(source.consumer))});
    const auto v6_inspection = GraphSerializer::inspect_document(smuggled_v6);
    CHECK(v6_inspection.refusal == GraphDocumentRefusal::EventConnectionInvalid);
    Sentinel v6_target;
    const auto v6_load = GraphSerializer::from_saved_document(v6_target.target.graph, smuggled_v6);
    CHECK_FALSE(v6_load.ok);
    CHECK(contains(v6_load.error, "event_connection_invalid"));
    CHECK(v6_target.untouched());

    const auto smuggled_plain = with_events(plain, {event_record("a", "b")});
    Sentinel plain_target;
    const auto plain_load = GraphSerializer::from_json(plain_target.target.graph, smuggled_plain);
    CHECK_FALSE(plain_load.ok);
    CHECK(contains(plain_load.error, "event_connection_invalid"));
    CHECK(plain_target.untouched());
}

TEST_CASE("planted event connection negatives are refused and the positive control loads",
          "[graph-events][persistence][negative-control]") {
    EventGraph source;
    build(source);
    const auto document = GraphSerializer::to_saved_document(source.graph);
    const auto producer = authored(source.graph, source.producer);
    const auto consumer = authored(source.graph, source.consumer);
    const auto input = authored(source.graph, source.input);

    SECTION("positive control: the record as written loads") {
        const auto rewritten = with_events(document, {event_record(producer, consumer)});
        REQUIRE(GraphSerializer::inspect_document(rewritten).ok());
        Target target;
        REQUIRE(GraphSerializer::from_saved_document(target.graph, rewritten).ok);
        CHECK(target.graph.graph_event_edge().has_value());
    }
    SECTION("a dangling endpoint is refused before the graph is touched") {
        const auto dangling = with_events(document, {event_record(producer, "missing-node")});
        CHECK(GraphSerializer::inspect_document(dangling).refusal ==
              GraphDocumentRefusal::EventConnectionDangling);
        Sentinel target;
        const auto load = GraphSerializer::from_saved_document(target.target.graph, dangling);
        CHECK_FALSE(load.ok);
        CHECK(contains(load.error, "event_connection_dangling"));
        CHECK(target.untouched());
    }
    SECTION("an identical repeated record is refused before the graph is touched") {
        const auto repeated = with_events(
            document, {event_record(producer, consumer), event_record(producer, consumer)});
        CHECK(GraphSerializer::inspect_document(repeated).refusal ==
              GraphDocumentRefusal::EventConnectionDuplicate);
        Sentinel target;
        CHECK_FALSE(GraphSerializer::from_saved_document(target.target.graph, repeated).ok);
        CHECK(target.untouched());
    }
    SECTION("a second distinct record is refused by the graph's one-connection rule") {
        const auto second = with_events(
            document, {event_record(producer, consumer), event_record(input, consumer)});
        REQUIRE(GraphSerializer::inspect_document(second).ok());
        Target target;
        const auto load = GraphSerializer::from_saved_document(target.graph, second);
        CHECK_FALSE(load.ok);
        CHECK(contains(load.error, "event_connection_not_restored"));
        CHECK(target.graph.nodes().empty());
        CHECK(load.persisted_node_ids.empty());
        CHECK(load.identity_node_ids.empty());
    }
    SECTION("an event kind the graph does not admit is refused") {
        const auto kind = with_events(document, {event_record(producer, consumer, 0, 0, 2)});
        REQUIRE(GraphSerializer::inspect_document(kind).ok());
        Target target;
        const auto load = GraphSerializer::from_saved_document(target.graph, kind);
        CHECK_FALSE(load.ok);
        CHECK(contains(load.error, "event_connection_not_restored"));
        CHECK(contains(load.error, "(feedback)"));
        CHECK(target.graph.nodes().empty());
        CHECK(load.identity_node_ids.empty());
    }
    SECTION("an endpoint whose node kind cannot carry events is refused") {
        const auto audio = with_events(document, {event_record(input, consumer)});
        REQUIRE(GraphSerializer::inspect_document(audio).ok());
        Target target;
        const auto load = GraphSerializer::from_saved_document(target.graph, audio);
        CHECK_FALSE(load.ok);
        CHECK(contains(load.error, "(binding)"));
        CHECK(target.graph.nodes().empty());
    }
    SECTION("a record with an unknown member or out-of-range port is malformed") {
        auto extra = event_record(producer, consumer);
        extra.addMember("queue", static_cast<std::int64_t>(3));
        CHECK(GraphSerializer::inspect_document(with_events(document, {extra})).refusal ==
              GraphDocumentRefusal::EventConnectionInvalid);
        CHECK(GraphSerializer::inspect_document(
                  with_events(document, {event_record(producer, consumer, 256)}))
                  .refusal == GraphDocumentRefusal::EventConnectionInvalid);
    }
}

TEST_CASE("an injected failure while rebinding event connections refuses through one cleanup",
          "[graph-events][persistence][negative-control]") {
    EventGraph source;
    build(source);
    const auto document = GraphSerializer::to_saved_document(source.graph);
    Target target;
    GraphSerializerTestAccess::fail_next_event_rebind();
    const auto refused = GraphSerializer::from_saved_document(target.graph, document);
    CHECK_FALSE(refused.ok);
    CHECK(target.graph.nodes().empty());
    CHECK_FALSE(target.graph.graph_event_edge().has_value());
    CHECK(refused.persisted_node_ids.empty());
    CHECK(refused.identity_node_ids.empty());
    // The hook is one-shot: the same document then loads.
    const auto loaded = GraphSerializer::from_saved_document(target.graph, document);
    REQUIRE(loaded.ok);
    CHECK(target.graph.graph_event_edge().has_value());
}

TEST_CASE("the document diff and edit history track the event connection by authored identity",
          "[graph-events][persistence][history]") {
    EventGraph source;
    build(source, false);
    const auto before = GraphSerializer::to_saved_document(source.graph);
    const auto checkpoint = GraphEditHistory::checkpoint(source.graph);
    REQUIRE(source.graph.connect_graph_event({source.producer, source.consumer}) ==
            GraphEventBindingResult::Accepted);
    const auto after = GraphSerializer::to_saved_document(source.graph);
    const auto key = authored(source.graph, source.producer) + "[0] => " +
                     authored(source.graph, source.consumer) + "[0]";

    CHECK(GraphSerializer::inspect_document(before).document.semantic_digest() !=
          GraphSerializer::inspect_document(after).document.semantic_digest());
    const auto added = diff_graph_documents(before, after);
    REQUIRE(added.ok());
    CHECK_FALSE(added.equal());
    CHECK(added.event_connections_added == std::vector<std::string>{key});
    CHECK(added.event_connections_removed.empty());
    CHECK(added.nodes_changed.empty());
    CHECK(added.connections_added.empty());
    CHECK(added.sections_changed.empty());
    CHECK(added.before_digest != added.after_digest);
    const auto removed = diff_graph_documents(after, before);
    CHECK(removed.event_connections_removed == std::vector<std::string>{key});
    const auto changed = diff_graph_documents(
        after,
        with_events(after, {event_record(authored(source.graph, source.producer),
                                         authored(source.graph, source.consumer), 0, 0, 2)}));
    CHECK(changed.event_connections_changed == std::vector<std::string>{key});

    // NodeId renumbering alone is no change: reload into a graph whose ids
    // start elsewhere and compare.
    Target renumbered;
    renumbered.graph.add_gain_node("Burn");
    renumbered.graph.clear();
    REQUIRE(GraphSerializer::from_saved_document(renumbered.graph, after).ok);
    const auto reloaded =
        diff_graph_documents(after, GraphSerializer::to_saved_document(renumbered.graph));
    CHECK(reloaded.equal());

    GraphEditHistory history;
    REQUIRE(history.commit(checkpoint, source.graph, "connect event").ok());
    REQUIRE(history.undo(source.graph).ok());
    CHECK_FALSE(source.graph.graph_event_edge().has_value());
    REQUIRE(history.redo(source.graph).ok());
    const auto redone = source.graph.graph_event_edge();
    REQUIRE(redone.has_value());
    CHECK(authored(source.graph, redone->producer) + "[0] => " +
              authored(source.graph, redone->consumer) + "[0]" ==
          key);
}

TEST_CASE("the document diff keeps event connection records that differ only by kind",
          "[graph-events][persistence][history]") {
    EventGraph source;
    build(source);
    const auto document = GraphSerializer::to_saved_document(source.graph);
    const auto producer = authored(source.graph, source.producer);
    const auto consumer = authored(source.graph, source.consumer);
    const auto both = with_events(document, {event_record(producer, consumer, 0, 0, 1),
                                             event_record(producer, consumer, 0, 0, 2)});
    const auto second_only = with_events(document, {event_record(producer, consumer, 0, 0, 2)});
    // Inspection admits both documents, so the diff is what has to tell
    // them apart.
    REQUIRE(GraphSerializer::inspect_document(both).ok());
    REQUIRE(GraphSerializer::inspect_document(second_only).ok());

    const auto diff = diff_graph_documents(both, second_only);
    REQUIRE(diff.ok());
    CHECK_FALSE(diff.equal());
    CHECK(diff.before_digest != diff.after_digest);
    // Records on one endpoint pair are paired in sorted order: the first
    // changed kind and the second is gone.
    const auto key = producer + "[0] => " + consumer + "[0]";
    CHECK(diff.event_connections_changed == std::vector<std::string>{key});
    CHECK(diff.event_connections_removed == std::vector<std::string>{key + " #2"});
    CHECK(diff.event_connections_added.empty());
    // Record order is not a difference.
    const auto swapped = with_events(document, {event_record(producer, consumer, 0, 0, 2),
                                                event_record(producer, consumer, 0, 0, 1)});
    CHECK(diff_graph_documents(both, swapped).equal());
    // Positive control: a kind change on a single record is still one change.
    const auto changed = diff_graph_documents(
        with_events(document, {event_record(producer, consumer, 0, 0, 1)}), second_only);
    CHECK(changed.event_connections_changed.size() == 1);
    CHECK(changed.event_connections_added.empty());
    CHECK(changed.event_connections_removed.empty());
}

TEST_CASE("an event connection edited outside the history is named in the Diverged detail",
          "[graph-events][persistence][history]") {
    EventGraph source;
    build(source, false);
    GraphEditHistory history;
    auto checkpoint = GraphEditHistory::checkpoint(source.graph);
    REQUIRE(source.graph.connect_graph_event({source.producer, source.consumer}) ==
            GraphEventBindingResult::Accepted);
    REQUIRE(history.commit(std::move(checkpoint), source.graph, "connect event").ok());
    REQUIRE(history.undo(source.graph).ok());
    REQUIRE_FALSE(source.graph.graph_event_edge().has_value());

    // Reconnecting outside the history leaves only the event connection
    // different from what redo expects.
    REQUIRE(source.graph.connect_graph_event({source.producer, source.consumer}) ==
            GraphEventBindingResult::Accepted);
    const auto outside = GraphSerializer::to_saved_document(source.graph);
    const auto step = history.redo(source.graph);
    REQUIRE(step.code == GraphHistoryCode::Diverged);
    INFO(step.detail);
    CHECK(contains(step.detail, "0 node, 0 connection, 1 event connection, 0 spectral asset and "
                                "0 section differences"));
    CHECK(GraphSerializer::to_saved_document(source.graph) == outside);
}

// ── Subgraph templates ─────────────────────────────────────────────────────

namespace {

SubgraphNodeSpec event_member(std::string key) {
    SubgraphNodeSpec spec;
    spec.key = std::move(key);
    spec.type = NodeType::Custom;
    spec.custom_type_id = kEventType;
    spec.custom_type_version = 1;
    return spec;
}

// in -> p -> m -> c -> out, with an optional declared event connection.
SubgraphTemplate event_chain_template(bool declare_event = true) {
    SubgraphTemplate t;
    t.identity = "test.event-chain";
    t.nodes = {{"in", NodeType::AudioInput, 1, 1.0f},
               event_member("p"),
               event_member("m"),
               event_member("c"),
               {"out", NodeType::AudioOutput, 1, 1.0f}};
    t.edges = {{"in", 0, "p", 0}, {"p", 0, "m", 0}, {"m", 0, "c", 0}, {"c", 0, "out", 0}};
    if (declare_event)
        t.event_edge = SubgraphEventEdgeSpec{"p", "c"};
    return t;
}

// The event chain as a child of a parent template.
SubgraphTemplate nested_event_template() {
    SubgraphTemplate child = event_chain_template();
    child.identity = "test.event-chain.child";
    child.nodes = {event_member("p"), event_member("m"), event_member("c")};
    child.edges = {{"p", 0, "m", 0}, {"m", 0, "c", 0}};
    SubgraphTemplate parent;
    parent.identity = "test.event-rack";
    parent.nodes = {{"in", NodeType::AudioInput, 1, 1.0f}, {"out", NodeType::AudioOutput, 1, 1.0f}};
    parent.children = {{"voice", std::make_shared<SubgraphTemplate>(std::move(child)), 2}};
    parent.edges = {{"in", 0, "voice.p", 0}, {"voice.c", 0, "out", 0}};
    return parent;
}

class SilentProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor value;
        value.name = "Silent";
        value.manufacturer = "Pulp Test";
        value.bundle_id = "com.pulp.test.silent-event";
        value.version = "1.0.0";
        value.input_buses = {{"In", 1, false}};
        value.output_buses = {{"Out", 1, false}};
        return value;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>& out, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {
        out.clear();
    }
};

} // namespace

TEST_CASE("a subgraph template's event connection expands, saves and restores",
          "[graph-events][persistence][subgraph]") {
    Target source;
    const std::array templates{event_chain_template()};
    auto expansion = instantiate_subgraph(source.graph, templates[0], "chain-a");
    REQUIRE(expansion);
    REQUIRE(expansion.instance.event_edge.has_value());
    const auto live = source.graph.graph_event_edge();
    REQUIRE(live.has_value());
    CHECK(live->producer == expansion.instance.event_edge->producer);
    CHECK(live->consumer == expansion.instance.event_edge->consumer);
    CHECK(render_first_sample(source.graph, *source.next_sample) == float(kPayload));

    const auto document =
        to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1));
    REQUIRE_FALSE(document.empty());
    REQUIRE(choc::json::parse(document).hasObjectMember("event_connections"));

    Target restored;
    const auto loaded = from_saved_subgraph_document(restored.graph, document, templates);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.subgraphs.size() == 1);
    const auto edge = restored.graph.graph_event_edge();
    REQUIRE(edge.has_value());
    REQUIRE(loaded.subgraphs[0].event_edge.has_value());
    CHECK(loaded.subgraphs[0].event_edge->producer == edge->producer);
    CHECK(loaded.subgraphs[0].event_edge->consumer == edge->consumer);
    CHECK(render_first_sample(restored.graph, *restored.next_sample) == float(kPayload));
    CHECK(to_saved_subgraph_document(restored.graph, loaded.subgraphs) == document);
}

TEST_CASE("a nested child template's event connection round-trips",
          "[graph-events][persistence][subgraph]") {
    Target source;
    const std::array templates{nested_event_template()};
    auto expansion = instantiate_subgraph(source.graph, templates[0], "rack-a");
    REQUIRE(expansion);
    REQUIRE(expansion.instance.event_edge.has_value());
    CHECK(render_first_sample(source.graph, *source.next_sample) == float(kPayload));
    const auto document =
        to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1));
    REQUIRE_FALSE(document.empty());
    Target restored;
    const auto loaded = from_saved_subgraph_document(restored.graph, document, templates);
    REQUIRE(loaded.ok());
    CHECK(render_first_sample(restored.graph, *restored.next_sample) == float(kPayload));
}

TEST_CASE("subgraph event connection declarations the graph cannot bind roll back",
          "[graph-events][subgraph][negative-control]") {
    SECTION("a Processor member cannot be an event endpoint") {
        Target target;
        auto t = event_chain_template(false);
        SubgraphNodeSpec processor;
        processor.key = "proc";
        processor.type = NodeType::Plugin;
        processor.processor_type_id = "test.silent-event";
        processor.processor_type_version = 1;
        processor.processor_descriptor_version = "1.0.0";
        processor.processor_factory =
            [](std::uint32_t) -> std::unique_ptr<pulp::format::Processor> {
            return std::make_unique<SilentProcessor>();
        };
        t.nodes.push_back(std::move(processor));
        t.edges = {{"in", 0, "proc", 0}, {"proc", 0, "c", 0}, {"c", 0, "out", 0}};
        t.event_edge = SubgraphEventEdgeSpec{"proc", "c"};
        const auto refused = instantiate_subgraph(target.graph, t, "proc-chain");
        CHECK_FALSE(refused);
        CHECK(contains(refused.result.rule, "event connection proc -> c was refused"));
        CHECK(target.graph.nodes().empty());
        CHECK(refused.instance.nodes.empty());
        CHECK_FALSE(refused.instance.event_edge.has_value());
    }
    SECTION("a second instance cannot bind a second event connection") {
        Target target;
        const auto first = instantiate_subgraph(target.graph, event_chain_template(), "first");
        REQUIRE(first);
        const auto nodes = target.graph.nodes().size();
        const auto second = instantiate_subgraph(target.graph, event_chain_template(), "second");
        CHECK_FALSE(second);
        CHECK(target.graph.nodes().size() == nodes);
        const auto edge = target.graph.graph_event_edge();
        REQUIRE(edge.has_value());
        CHECK(edge->producer == first.instance.event_edge->producer);
    }
    SECTION("positive control: a template without the declaration expands twice") {
        Target target;
        REQUIRE(instantiate_subgraph(target.graph, event_chain_template(false), "first"));
        REQUIRE(instantiate_subgraph(target.graph, event_chain_template(false), "second"));
    }
    SECTION("an unknown member key is refused before binding") {
        Target target;
        auto t = event_chain_template();
        t.event_edge = SubgraphEventEdgeSpec{"p", "nowhere"};
        CHECK_FALSE(instantiate_subgraph(target.graph, t, "unknown"));
        CHECK(target.graph.nodes().empty());
    }
}

TEST_CASE("subgraph event connection restore refuses lost, re-pointed and undeclared connections",
          "[graph-events][persistence][subgraph][negative-control]") {
    Target source;
    const std::array declared{event_chain_template()};
    auto expansion = instantiate_subgraph(source.graph, declared[0], "chain-a");
    REQUIRE(expansion);
    const auto document =
        to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1));
    REQUIRE_FALSE(document.empty());
    const auto authored_of = [&](NodeId id) { return authored(source.graph, id); };
    const auto p = authored_of(expansion.instance.nodes[1]);
    const auto m = authored_of(expansion.instance.nodes[2]);
    const auto c = authored_of(expansion.instance.nodes[3]);

    SECTION("positive control: the written document restores") {
        Sentinel target;
        // The sentinel holds an unrelated node; a restore replaces the graph.
        REQUIRE(from_saved_subgraph_document(target.target.graph, document, declared).ok());
    }
    SECTION("a lost connection is refused on the candidate") {
        Sentinel target;
        const auto loaded =
            from_saved_subgraph_document(target.target.graph, with_events(document, {}), declared);
        CHECK_FALSE(loaded.ok());
        CHECK(contains(loaded.load.error, "subgraph_instance_mismatch"));
        CHECK(contains(loaded.load.error, "event connection was not restored"));
        CHECK(target.untouched());
    }
    SECTION("a re-pointed connection is refused on the candidate") {
        Sentinel target;
        const auto loaded = from_saved_subgraph_document(
            target.target.graph, with_events(document, {event_record(p, m)}), declared);
        CHECK_FALSE(loaded.ok());
        CHECK(contains(loaded.load.error, "event connection was not restored"));
        CHECK(target.untouched());
    }
    SECTION("an undeclared connection between members is refused on the candidate") {
        Sentinel target;
        const std::array undeclared{event_chain_template(false)};
        const auto loaded = from_saved_subgraph_document(target.target.graph, document, undeclared);
        CHECK_FALSE(loaded.ok());
        CHECK(contains(loaded.load.error, "not declared by the template"));
        CHECK(target.untouched());
    }
    SECTION("a document event connection the graph refuses leaves the graph untouched") {
        Sentinel target;
        const auto plain = GraphSerializer::to_saved_document(source.graph);
        const auto loaded = from_saved_subgraph_document(
            target.target.graph, with_events(plain, {event_record(p, c, 0, 0, 2)}), declared);
        CHECK_FALSE(loaded.ok());
        CHECK(contains(loaded.load.error, "event_connection_not_restored"));
        CHECK(target.untouched());
    }
}

TEST_CASE("the subgraph writer refuses a document its restore would refuse",
          "[graph-events][persistence][subgraph][negative-control]") {
    SECTION("the declared connection was dropped") {
        Target source;
        auto expansion = instantiate_subgraph(source.graph, event_chain_template(), "chain-a");
        REQUIRE(expansion);
        REQUIRE(source.graph.disconnect(expansion.instance.nodes[2], 0, expansion.instance.nodes[3],
                                        0));
        REQUIRE_FALSE(source.graph.graph_event_edge().has_value());
        CHECK(to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1)).empty());
    }
    SECTION("an undeclared connection joins two members") {
        Target source;
        auto expansion = instantiate_subgraph(source.graph, event_chain_template(false), "chain-a");
        REQUIRE(expansion);
        REQUIRE_FALSE(
            to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1)).empty());
        REQUIRE(source.graph.connect_graph_event(
                    {expansion.instance.nodes[1], expansion.instance.nodes[3]}) ==
                GraphEventBindingResult::Accepted);
        CHECK(to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1)).empty());
    }
}

TEST_CASE("a voice template cannot declare an event connection",
          "[graph-events][persistence][subgraph][voices]") {
    // Expansion: refused even for one voice, and the graph is unchanged.
    {
        Target target;
        const auto keep = target.graph.add_gain_node("keep");
        const auto expansion =
            instantiate_subgraph_voices(target.graph, event_chain_template(), "chain", 1);
        INFO(expansion.result.message());
        CHECK_FALSE(expansion);
        CHECK(expansion.result.rule.find("cannot declare an event connection") !=
              std::string::npos);
        REQUIRE(target.graph.nodes().size() == 1);
        CHECK(target.graph.nodes().front().id == keep);
        CHECK_FALSE(target.graph.graph_event_edge().has_value());
        // Positive control: the same template without its event connection.
        CHECK(instantiate_subgraph_voices(target.graph, event_chain_template(false), "chain", 2));
    }

    // Restore: a document whose declaration names an event template is
    // refused on the candidate, and the caller's graph is untouched.
    Target source;
    const std::array templates{event_chain_template()};
    auto expansion = instantiate_subgraph(source.graph, templates[0], "chain.voice.0", 0);
    REQUIRE(expansion);
    const auto document =
        to_saved_subgraph_document(source.graph, std::span(&expansion.instance, 1));
    REQUIRE_FALSE(document.empty());
    auto root = choc::json::parse(document);
    auto declaration = choc::value::createObject("VoiceDeclaration");
    declaration.addMember("identity", std::string("chain"));
    declaration.addMember("template_identity", std::string("test.event-chain"));
    declaration.addMember("voice_count", std::int64_t{1});
    auto voices = choc::value::createEmptyArray();
    voices.addArrayElement(std::string("chain.voice.0"));
    declaration.addMember("voices", voices);
    auto declarations = choc::value::createEmptyArray();
    declarations.addArrayElement(declaration);
    root.addMember("voice_declarations", declarations);
    const auto declared = choc::json::toString(root, true);
    REQUIRE(GraphSerializer::inspect_document(declared).ok());

    Target refused;
    const auto keep = refused.graph.add_gain_node("keep");
    const auto loaded = from_saved_subgraph_document(refused.graph, declared, templates);
    INFO(loaded.load.error);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.load.error.find("a voice template cannot declare an event connection") !=
          std::string::npos);
    CHECK(loaded.voice_banks.empty());
    REQUIRE(refused.graph.nodes().size() == 1);
    CHECK(refused.graph.nodes().front().id == keep);

    // Positive control: the same document without the declaration restores.
    Target control;
    CHECK(from_saved_subgraph_document(control.graph, document, templates).ok());
    // The writer refuses a bank whose voice holds an event connection.
    SubgraphVoiceBank bank{{"chain", "test.event-chain", 1}, {expansion.instance}};
    CHECK(to_saved_subgraph_document(source.graph, {}, std::span(&bank, 1), {}).empty());
}
