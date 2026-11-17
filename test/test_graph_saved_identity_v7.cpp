// Saved-identity v7: persisted authored node IDs, their validation, and the
// deterministic migration from v6 documents.

#include "../core/host/src/graph_serializer_test_access.hpp"
#include "../core/host/src/signal_graph_authored_id_internal.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace pulp::host;

namespace {

std::string replace_once(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

std::string replace_all(std::string text, std::string_view from, std::string_view to) {
    REQUIRE(text.find(from) != std::string::npos);
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

const GraphNode* named(const SignalGraph& graph, std::string_view name) {
    for (const auto& node : graph.nodes())
        if (node.name == name)
            return &node;
    return nullptr;
}

std::string id_of(const SignalGraph& graph, std::string_view name) {
    const auto* node = named(graph, name);
    REQUIRE(node != nullptr);
    return node->authored_id.str();
}

// Input -> A -> B -> Output with two gains that share the name "G".
struct TwinGraph {
    SignalGraph graph;
    NodeId input = 0;
    NodeId a = 0;
    NodeId b = 0;
    NodeId output = 0;
};

void build_twins(TwinGraph& fixture) {
    auto& graph = fixture.graph;
    fixture.input = graph.add_input_node(1, "Input");
    fixture.a = graph.add_gain_node("G");
    fixture.b = graph.add_gain_node("G");
    fixture.output = graph.add_output_node(1, "Output");
    REQUIRE(graph.set_node_gain(fixture.a, 0.25f));
    REQUIRE(graph.set_node_gain(fixture.b, 0.75f));
    REQUIRE(graph.connect(fixture.input, 0, fixture.a, 0));
    REQUIRE(graph.connect(fixture.a, 0, fixture.b, 0));
    REQUIRE(graph.connect(fixture.b, 0, fixture.output, 0));
}

} // namespace

TEST_CASE("every created node gets a fresh, well-formed authored ID",
          "[host][graph][identity][v7]") {
    SignalGraph graph;
    std::vector<NodeId> nodes = {
        graph.add_input_node(1, "Input"),
        graph.add_output_node(1, "Output"),
        graph.add_gain_node("G"),
        graph.add_gain_node("G"),
        graph.add_midi_input_node("MIDI In"),
        graph.add_midi_output_node("MIDI Out"),
        graph.add_unresolved_custom_node("pulp.test.absent", 1, 1, 1, "Absent"),
        graph.add_control_index_input_node("Index"),
    };
    PluginInfo info;
    info.name = "External";
    info.unique_id = "vendor:external:1";
    nodes.push_back(graph.add_unresolved_plugin_node(info, 2, 2, "External"));
    std::set<std::string> ids;
    for (const auto node : nodes) {
        REQUIRE(node != 0);
        const auto& id = graph.node(node)->authored_id.str();
        CHECK(id.size() == 36);
        CHECK(id.rfind("nid-", 0) == 0);
        CHECK(ids.insert(id).second);
    }
    // The serializer reports the same IDs.
    for (const auto& [node, id] : GraphSerializer::saved_node_identities(graph))
        CHECK(graph.node(node)->authored_id.str() == id);

    // A second graph built the same way mints different IDs.
    SignalGraph other;
    CHECK_FALSE(ids.contains(other.node(other.add_gain_node("G"))->authored_id.str()));
}

TEST_CASE("the entropy-free authored-ID salt separates graphs deterministically",
          "[host][graph][identity][v7][wasm]") {
    using detail::authored_id_from_salt;
    using detail::authored_id_salt_from;
    constexpr std::uint64_t clock = 1'763'000'000'000'000'000ULL;
    constexpr std::uintptr_t address = 0x10'0000;
    constexpr std::uint64_t identity = 7;

    // Same inputs, same salt and IDs: nothing but the arguments is read, so
    // no entropy device and no clock contributes.
    const auto salt = authored_id_salt_from(clock, address, identity);
    CHECK(salt == authored_id_salt_from(clock, address, identity));
    CHECK(authored_id_from_salt(salt, 1) == authored_id_from_salt(salt, 1));

    // Each input alone separates two graphs. A WebAssembly build that loads
    // the same module twice can repeat an address, so the identity must
    // separate graphs by itself, and the other way round.
    const auto first = authored_id_from_salt(salt, 1);
    CHECK(first != authored_id_from_salt(authored_id_salt_from(clock, address + 64, identity), 1));
    CHECK(first != authored_id_from_salt(authored_id_salt_from(clock, address, identity + 1), 1));
    CHECK(first != authored_id_from_salt(authored_id_salt_from(clock + 1, address, identity), 1));

    // Address and identity cannot cancel each other out.
    std::set<detail::AuthoredIdSalt> salts;
    for (std::uint64_t i = 0; i < 64; ++i)
        for (std::uintptr_t a = 0; a < 64; ++a)
            salts.insert(authored_id_salt_from(clock, address + a * 16, identity + i));
    CHECK(salts.size() == 64 * 64);

    // Many mints from one salt are unique and well formed.
    std::set<std::string> ids;
    for (std::uint64_t counter = 1; counter <= 10'000; ++counter) {
        const auto id = authored_id_from_salt(salt, counter);
        REQUIRE(id.size() == 36);
        REQUIRE(id.rfind("nid-", 0) == 0);
        REQUIRE(ids.insert(id).second);
    }

    // Two live graphs seeded without entropy (as under Emscripten) differ.
    SignalGraph a;
    SignalGraph b;
    const auto a_salt = detail::authored_id_salt_without_entropy(
        reinterpret_cast<std::uintptr_t>(&a), a.authoring_receipt().graph_identity);
    const auto b_salt = detail::authored_id_salt_without_entropy(
        reinterpret_cast<std::uintptr_t>(&b), b.authoring_receipt().graph_identity);
    CHECK(a_salt != b_salt);
    CHECK(authored_id_from_salt(a_salt, 1) != authored_id_from_salt(b_salt, 1));
}

TEST_CASE("a rename keeps the authored ID and diffs as one changed node",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto id = fixture.graph.node(fixture.a)->authored_id.str();
    const auto before = GraphSerializer::to_saved_document(fixture.graph);
    const auto legacy_before = GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph);

    REQUIRE(fixture.graph.rename_node(fixture.a, "Renamed: \xC3\xA9t\xC3\xA9 / gain"));
    CHECK(fixture.graph.node(fixture.a)->authored_id.str() == id);
    CHECK_FALSE(fixture.graph.rename_node(fixture.a, ""));
    CHECK_FALSE(fixture.graph.rename_node(9999, "missing"));

    const auto after = GraphSerializer::to_saved_document(fixture.graph);
    const auto diff = diff_graph_documents(before, after);
    INFO(diff.detail);
    REQUIRE(diff.ok());
    CHECK(diff.nodes_added.empty());
    CHECK(diff.nodes_removed.empty());
    REQUIRE(diff.nodes_changed.size() == 1);
    CHECK(diff.nodes_changed.front().identity == id);
    CHECK(diff.nodes_changed.front().fields == std::vector<std::string>{"name"});
    CHECK(diff.connections_added.empty());
    CHECK(diff.connections_removed.empty());

    // Control: the v6 identity is rebuilt from the name, so the same rename
    // reads as a removed node and an added one.
    const auto legacy = diff_graph_documents(
        legacy_before, GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph));
    REQUIRE(legacy.ok());
    CHECK(legacy.nodes_added.size() == 1);
    CHECK(legacy.nodes_removed.size() == 1);
}

TEST_CASE("deleting either of two same-named nodes leaves the survivor's ID",
          "[host][graph][identity][v7]") {
    for (const bool delete_first : {true, false}) {
        INFO("delete_first=" << delete_first);
        TwinGraph fixture;
        build_twins(fixture);
        const auto doomed = delete_first ? fixture.a : fixture.b;
        const auto survivor = delete_first ? fixture.b : fixture.a;
        const auto doomed_id = fixture.graph.node(doomed)->authored_id.str();
        const auto survivor_id = fixture.graph.node(survivor)->authored_id.str();
        const auto before = GraphSerializer::to_saved_document(fixture.graph);
        const auto legacy_before =
            GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph);

        REQUIRE(fixture.graph.remove_node(doomed));
        CHECK(fixture.graph.node(survivor)->authored_id.str() == survivor_id);
        const auto after = GraphSerializer::to_saved_document(fixture.graph);
        const auto diff = diff_graph_documents(before, after);
        INFO(diff.detail);
        REQUIRE(diff.ok());
        CHECK(diff.nodes_removed == std::vector<std::string>{doomed_id});
        CHECK(diff.nodes_added.empty());
        // The survivor's gain did not change, so it is not reported.
        CHECK(diff.nodes_changed.empty());
        for (const auto& key : diff.connections_removed)
            CHECK(key.find(doomed_id) != std::string::npos);

        // Control: under the v6 rule, deleting the first twin hands its
        // identity to the survivor, which then reads as a changed node.
        if (delete_first) {
            const auto legacy = diff_graph_documents(
                legacy_before, GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph));
            REQUIRE(legacy.ok());
            REQUIRE(legacy.nodes_changed.size() == 1);
            CHECK(legacy.nodes_changed.front().identity == "node:gain:G:0");
        }
    }
}

TEST_CASE("authored IDs survive save, load, NodeId renumbering, undo and redo",
          "[host][graph][identity][v7]") {
    SignalGraph graph;
    const auto spacer = graph.add_gain_node("Spacer");
    const auto input = graph.add_input_node(1, "Input");
    const auto gain = graph.add_gain_node("Gain");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.remove_node(spacer));
    const auto ids = GraphSerializer::saved_node_identities(graph);
    const auto saved = GraphSerializer::to_saved_document(graph);

    SignalGraph loaded;
    const auto restored = GraphSerializer::from_saved_document(loaded, saved);
    INFO(restored.error);
    REQUIRE(restored.ok);
    // Control: the load renumbered.
    REQUIRE(restored.persisted_node_ids.at(input) != input);
    for (const auto& [old_node, id] : ids) {
        const auto live = restored.persisted_node_ids.at(old_node);
        CHECK(loaded.node(live)->authored_id.str() == id);
        CHECK(restored.identity_node_ids.at(id) == live);
    }
    CHECK(GraphSerializer::to_saved_document(loaded) != saved);
    CHECK(diff_graph_documents(saved, GraphSerializer::to_saved_document(loaded)).equal());

    // Undo and redo restore the IDs the recorded documents carry.
    GraphEditHistory history;
    auto before = GraphEditHistory::checkpoint(loaded);
    const auto trim = loaded.add_gain_node("Trim");
    const auto trim_id = loaded.node(trim)->authored_id.str();
    REQUIRE(loaded.rename_node(restored.identity_node_ids.at(ids.at(gain)), "Gain 2"));
    REQUIRE(history.commit(std::move(before), loaded, "add trim and rename").ok());
    const auto undone = history.undo(loaded);
    INFO(undone.detail);
    REQUIRE(undone.ok());
    CHECK(named(loaded, "Trim") == nullptr);
    CHECK(id_of(loaded, "Gain") == ids.at(gain));
    const auto redone = history.redo(loaded);
    INFO(redone.detail);
    REQUIRE(redone.ok());
    CHECK(id_of(loaded, "Trim") == trim_id);
    CHECK(id_of(loaded, "Gain 2") == ids.at(gain));
    CHECK(redone.node_ids.at(trim_id) == named(loaded, "Trim")->id);
}

TEST_CASE("a copy gets a new authored ID and a pasted duplicate ID is refused",
          "[host][graph][identity][v7]") {
    SignalGraph graph;
    const auto original = graph.add_gain_node("G");
    REQUIRE(graph.set_node_gain(original, 0.5f));
    // The same configuration added again is a new node.
    const auto copy = graph.add_gain_node("G");
    REQUIRE(graph.set_node_gain(copy, 0.5f));
    CHECK(graph.node(copy)->authored_id.str() != graph.node(original)->authored_id.str());
    const auto saved = GraphSerializer::to_saved_document(graph);
    REQUIRE(GraphSerializer::inspect_document(saved).ok());

    // A record pasted with the ID it was copied from is refused rather than
    // renamed. Node records come first, so the first match is the record.
    const auto copy_id = graph.node(copy)->authored_id.str();
    REQUIRE(saved.find(copy_id) < saved.find("\"logical_identities\""));
    const auto pasted = replace_once(saved, copy_id, graph.node(original)->authored_id.str());
    const auto inspection = GraphSerializer::inspect_document(pasted);
    INFO(inspection.detail);
    CHECK(inspection.refusal == GraphDocumentRefusal::IdentityBindingDuplicate);
    SignalGraph target;
    CHECK_FALSE(GraphSerializer::from_saved_document(target, pasted).ok);
    // The tolerant loader refuses it too.
    CHECK_FALSE(GraphSerializer::from_json(target, pasted).ok);
}

TEST_CASE("names with delimiters and Unicode do not affect authored IDs",
          "[host][graph][identity][v7]") {
    // In v6 a plugin named "a:b" with unique id "c" and one named "a" with
    // unique id "b:c" share the key "plugin:a:b:c".
    SignalGraph graph;
    PluginInfo first;
    first.unique_id = "c";
    PluginInfo second;
    second.unique_id = "b:c";
    const auto p1 = graph.add_unresolved_plugin_node(first, 1, 1, "a:b");
    const auto p2 = graph.add_unresolved_plugin_node(second, 1, 1, "a");
    const auto composed = graph.add_gain_node("caf\xC3\xA9");    // NFC e-acute
    const auto decomposed = graph.add_gain_node("cafe\xCC\x81"); // NFD e + combining acute
    const auto emoji = graph.add_gain_node("\xF0\x9F\x8E\x9B: [x] \"q\" /\\");
    REQUIRE((p1 && p2 && composed && decomposed && emoji));
    const auto legacy = GraphSerializerTestAccess::legacy_v6_identities(graph);
    // Control: the v6 keys collide and differ only by ordinal.
    CHECK(legacy.at(p1).substr(0, legacy.at(p1).size() - 1) ==
          legacy.at(p2).substr(0, legacy.at(p2).size() - 1));

    const auto saved = GraphSerializer::to_saved_document(graph);
    REQUIRE(GraphSerializer::inspect_document(saved).ok());
    GraphLoadOptions options;
    options.load_plugins = false;
    options.include_default_plugin_roots = false;
    SignalGraph loaded;
    const auto restored = GraphSerializer::from_saved_document(loaded, saved, options);
    INFO(restored.error);
    REQUIRE(restored.ok);
    for (const auto node : {p1, p2, composed, decomposed, emoji}) {
        const auto& id = graph.node(node)->authored_id.str();
        const auto live = restored.identity_node_ids.at(id);
        CHECK(loaded.node(live)->name == graph.node(node)->name);
    }
    CHECK(diff_graph_documents(saved, GraphSerializer::to_saved_document(loaded)).equal());
}

TEST_CASE("v7 inspection refuses planted identity defects", "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    const auto a = fixture.graph.node(fixture.a)->authored_id.str();
    const auto b = fixture.graph.node(fixture.b)->authored_id.str();
    // Control: the document as written is admitted and restores.
    REQUIRE(GraphSerializer::inspect_document(saved).ok());
    {
        SignalGraph loaded;
        REQUIRE(GraphSerializer::from_saved_document(loaded, saved).ok);
    }

    const auto root = choc::json::parse(saved);
    const auto rebuild = [&](auto&& edit) {
        auto copy = choc::json::parse(saved);
        edit(copy);
        return choc::json::toString(copy, true);
    };
    const auto with_records = [&](auto&& edit) {
        return rebuild([&](choc::value::Value& doc) {
            const auto records = root["nodes"];
            auto nodes = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < records.size(); ++i) {
                choc::value::Value record(records[i]);
                edit(record);
                nodes.addArrayElement(record);
            }
            doc.setMember("nodes", nodes);
        });
    };
    const auto is_a = [&](const choc::value::Value& record) {
        return record["authored_id"].getString() == a;
    };

    struct Planted {
        std::string name;
        std::string document;
        GraphDocumentRefusal refusal;
    };
    const std::vector<Planted> planted = {
        // Two records carry one ID.
        {"duplicate record IDs", with_records([&](choc::value::Value& r) {
             if (r["authored_id"].getString() == b)
                 r.setMember("authored_id", a);
         }),
         GraphDocumentRefusal::IdentityBindingDuplicate},
        // A record without its ID, or with one outside the admitted spelling.
        {"missing record ID", with_records([&](choc::value::Value& r) {
             if (is_a(r)) {
                 auto stripped = choc::value::createObject("Node");
                 for (std::uint32_t m = 0; m < r.size(); ++m) {
                     const auto member = r.getObjectMemberAt(m);
                     if (std::string_view(member.name) != "authored_id")
                         stripped.addMember(member.name, member.value);
                 }
                 r = stripped;
             }
         }),
         GraphDocumentRefusal::IdentityEnvelopeInvalid},
        {"malformed record ID", replace_all(saved, a, "nid:with spaces"),
         GraphDocumentRefusal::IdentityEnvelopeInvalid},
        // Records swap IDs while the envelope keeps the old spelling, so each
        // identity entry and binding names the other twin.
        {"identities name other records", with_records([&](choc::value::Value& r) {
             const std::string id(r["authored_id"].getString());
             if (id == a)
                 r.setMember("authored_id", b);
             else if (id == b)
                 r.setMember("authored_id", a);
         }),
         GraphDocumentRefusal::IdentityBindingUnknown},
        // A binding that disagrees with its identity entry.
        {"binding crosses its identity entry", rebuild([&](choc::value::Value& doc) {
             // Hold the view: member names point into it.
             const auto bound = root["bindings"];
             auto bindings = choc::value::createObject("Bindings");
             for (std::uint32_t i = 0; i < bound.size(); ++i) {
                 const auto member = bound.getObjectMemberAt(i);
                 const std::string name(member.name);
                 const auto other = name == a   ? bound[b].getInt64()
                                    : name == b ? bound[a].getInt64()
                                                : member.value.getInt64();
                 bindings.addMember(name, other);
             }
             doc.setMember("bindings", bindings);
         }),
         GraphDocumentRefusal::IdentityEnvelopeInvalid},
        // A v7 document labelled as format 6, and as v6.
        {"v7 at format 6", replace_once(saved, "\"format_version\": 7", "\"format_version\": 6"),
         GraphDocumentRefusal::IdentityEnvelopeInvalid},
        {"v7 records under the v6 schema",
         replace_all(replace_once(saved, "\"format_version\": 7", "\"format_version\": 5"),
                     "pulp.graph.saved-identity.v7", "pulp.graph.saved-identity.v6"),
         GraphDocumentRefusal::IdentityEnvelopeInvalid},
        {"future format", replace_once(saved, "\"format_version\": 7", "\"format_version\": 8"),
         GraphDocumentRefusal::UnsupportedFutureVersion},
    };
    for (const auto& item : planted) {
        INFO(item.name);
        const auto inspection = GraphSerializer::inspect_document(item.document);
        INFO(inspection.detail);
        CHECK(inspection.refusal == item.refusal);
        CHECK(diff_graph_documents(saved, item.document).code ==
              GraphDocumentDiffCode::AfterRefused);
        // Refused before the caller's graph is touched.
        SignalGraph target;
        const auto kept = target.add_gain_node("Kept");
        const auto refused = GraphSerializer::from_saved_document(target, item.document);
        CHECK_FALSE(refused.ok);
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(kept) != nullptr);
    }
}

TEST_CASE("v7 restore refuses a lost edge", "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    // An edge into a port the destination does not have cannot be restored.
    const auto lost = replace_once(saved, "\"dest_port\": 0", "\"dest_port\": 9");
    REQUIRE(GraphSerializer::inspect_document(lost).ok());
    SignalGraph target;
    const auto refused = GraphSerializer::from_saved_document(target, lost);
    INFO(refused.error);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.starts_with("connection_not_restored:"));
    CHECK(target.nodes().empty());
    // Control: the unmodified document restores every edge.
    SignalGraph control;
    REQUIRE(GraphSerializer::from_saved_document(control, saved).ok);
    CHECK(control.connections().size() == 3);
}

TEST_CASE("v6 migration is deterministic and never guesses continuity",
          "[host][graph][identity][v7][migration]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto legacy = GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph);
    const auto inspection = GraphSerializer::inspect_document(legacy);
    REQUIRE(inspection.ok());
    REQUIRE(inspection.document.profile() == "pulp.graph.saved-identity.v6");

    const auto first = GraphSerializer::migrate_saved_document(legacy);
    INFO(first.detail);
    REQUIRE(first.ok());
    CHECK(first.migrated);
    const auto again = GraphSerializer::migrate_saved_document(legacy);
    REQUIRE(again.ok());
    CHECK(again.document == first.document);
    CHECK(again.identity_map == first.identity_map);

    // The map names every v6 identity once, each to a distinct v7 ID.
    const auto v6_ids = GraphSerializerTestAccess::legacy_v6_identities(fixture.graph);
    REQUIRE(first.identity_map.size() == v6_ids.size());
    std::set<std::string> new_ids;
    std::map<std::string, std::string> map(first.identity_map.begin(), first.identity_map.end());
    for (const auto& [node, v6] : v6_ids) {
        REQUIRE(map.contains(v6));
        CHECK(map.at(v6).rfind("nid-", 0) == 0);
        CHECK(new_ids.insert(map.at(v6)).second);
    }
    const auto migrated = GraphSerializer::inspect_document(first.document);
    INFO(migrated.detail);
    REQUIRE(migrated.ok());
    CHECK(migrated.document.profile() == "pulp.graph.saved-identity.v7");

    // Loading the v6 document assigns exactly the migrated IDs.
    SignalGraph loaded;
    const auto restored = GraphSerializer::from_saved_document(loaded, legacy);
    INFO(restored.error);
    REQUIRE(restored.ok);
    for (const auto& [v6, live] : restored.identity_node_ids)
        CHECK(loaded.node(live)->authored_id.str() == map.at(v6));
    CHECK(diff_graph_documents(first.document, GraphSerializer::to_saved_document(loaded)).equal());

    // A v7 document migrates to itself.
    const auto current = GraphSerializer::migrate_saved_document(first.document);
    REQUIRE(current.ok());
    CHECK_FALSE(current.migrated);
    CHECK(current.document == first.document);
    CHECK(current.identity_map.empty());
    // A document with no envelope has nothing to migrate.
    CHECK_FALSE(
        GraphSerializer::migrate_saved_document(GraphSerializer::to_json(fixture.graph)).ok());

    // Two independently edited copies of one legacy document share no ID,
    // even for the nodes neither edit touched.
    REQUIRE(fixture.graph.set_node_gain(fixture.b, 0.5f));
    const auto edited = GraphSerializer::migrate_saved_document(
        GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph));
    REQUIRE(edited.ok());
    for (const auto& [v6, id] : edited.identity_map)
        CHECK_FALSE(new_ids.contains(id));
    const auto cross = diff_graph_documents(first.document, edited.document);
    REQUIRE(cross.ok());
    CHECK(cross.nodes_removed.size() == 4);
    CHECK(cross.nodes_added.size() == 4);

    // A v6 and a v7 document are not compared with each other.
    const auto mixed = diff_graph_documents(legacy, first.document);
    CHECK(mixed.code == GraphDocumentDiffCode::AfterRefused);
    CHECK(mixed.refusal == GraphDocumentRefusal::UnsupportedDocumentProfile);
}

TEST_CASE("legacy v6 and plain documents keep loading", "[host][graph][identity][v7][migration]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto plain = GraphSerializer::to_json(fixture.graph);
    // The plain writer is unchanged: no format 7 and no authored IDs.
    CHECK(plain.find("authored_id") == std::string::npos);
    CHECK(plain.find("\"format_version\": 7") == std::string::npos);

    // A plain load assigns deterministic IDs from the document.
    SignalGraph first;
    SignalGraph second;
    REQUIRE(GraphSerializer::from_json(first, plain).ok);
    REQUIRE(GraphSerializer::from_json(second, plain).ok);
    CHECK(GraphSerializer::to_saved_document(first) == GraphSerializer::to_saved_document(second));

    // An authored_id outside a v7 document is refused, not honoured.
    const auto legacy = GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph);
    const auto smuggled =
        replace_once(legacy, "\"name\": \"Input\"", "\"name\": \"Input\", \"authored_id\": \"x\"");
    CHECK(GraphSerializer::inspect_document(smuggled).refusal ==
          GraphDocumentRefusal::IdentityEnvelopeInvalid);
    SignalGraph target;
    const auto loaded = GraphSerializer::from_json(target, smuggled);
    CHECK_FALSE(loaded.ok);
    CHECK(loaded.error.find("requires format_version 7") != std::string::npos);
    // Control: without it the legacy document loads.
    CHECK(GraphSerializer::from_saved_document(target, legacy).ok);
}

TEST_CASE("an ordinary load refuses a format 7 document whose envelope is incomplete",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    const auto edited = [&](auto&& edit) {
        auto copy = choc::json::parse(saved);
        edit(copy);
        return choc::json::toString(copy, true);
    };
    const auto without = [&](std::string_view member) {
        return edited([&](choc::value::Value& doc) {
            auto rebuilt = choc::value::createObject("");
            for (std::uint32_t i = 0; i < doc.size(); ++i) {
                const auto item = doc.getObjectMemberAt(i);
                if (std::string_view(item.name) != member)
                    rebuilt.addMember(item.name, item.value);
            }
            doc = rebuilt;
        });
    };
    struct Case {
        std::string name;
        std::string document;
    };
    const std::vector<Case> cases = {
        {"no logical_identities", without("logical_identities")},
        {"no bindings", without("bindings")},
        {"no logical_identities or bindings", edited([&](choc::value::Value& doc) {
             auto rebuilt = choc::value::createObject("");
             for (std::uint32_t i = 0; i < doc.size(); ++i) {
                 const auto item = doc.getObjectMemberAt(i);
                 const std::string_view name(item.name);
                 if (name != "logical_identities" && name != "bindings")
                     rebuilt.addMember(item.name, item.value);
             }
             doc = rebuilt;
         })},
        {"v6 identity profile under a v7 schema", edited([](choc::value::Value& doc) {
             choc::value::Value identities(doc["logical_identities"]);
             identities.setMember("profile", "pulp.graph.saved-identity.v6");
             doc.setMember("logical_identities", identities);
         })},
    };

    const auto path = std::filesystem::temp_directory_path() / "pulp-graph-v7-envelope.pulpgraph";
    for (const auto& planted : cases) {
        INFO(planted.name);
        // Control on the instrument: the inspector refuses each document.
        CHECK_FALSE(GraphSerializer::inspect_document(planted.document).ok());

        SignalGraph target;
        const auto keep = target.add_gain_node("Keep");
        const auto from_json = GraphSerializer::from_json(target, planted.document);
        INFO(from_json.error);
        CHECK_FALSE(from_json.ok);
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(keep) != nullptr);

        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << planted.document;
        }
        const auto from_file = GraphSerializer::from_file(target, path);
        INFO(from_file.error);
        CHECK_FALSE(from_file.ok);
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(keep) != nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // Control: the complete document loads through both readers.
    SignalGraph control;
    REQUIRE(GraphSerializer::from_json(control, saved).ok);
    CHECK(control.nodes().size() == 4);
}

TEST_CASE("a failure planning authored IDs refuses the load and leaves the graph",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);

    SignalGraph target;
    const auto keep = target.add_gain_node("Keep");
    GraphSerializerTestAccess::fail_next_authored_id_plan();
    const auto refused = GraphSerializer::from_json(target, saved);
    INFO(refused.error);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.starts_with("authored_id planning failed:"));
    CHECK(target.nodes().size() == 1);
    CHECK(target.node(keep) != nullptr);

    // Control: without the injected failure the same load keeps every ID.
    const auto loaded = GraphSerializer::from_json(target, saved);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    CHECK(id_of(target, "Input") == id_of(fixture.graph, "Input"));
}

TEST_CASE("a failure parsing the original document refuses the load and leaves the graph",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const std::vector<std::pair<std::string, std::string>> documents = {
        {"v7", GraphSerializer::to_saved_document(fixture.graph)},
        {"legacy v6", GraphSerializerTestAccess::legacy_v6_saved_document(fixture.graph)},
    };
    for (const auto& [label, document] : documents) {
        INFO(label);
        REQUIRE_FALSE(document.empty());
        SignalGraph target;
        const auto keep = target.add_gain_node("Keep");
        // The original parse is the first one; every later parse succeeds,
        // so only a refusal at this parse keeps fresh IDs out of the graph.
        GraphSerializerTestAccess::fail_next_document_parse();
        const auto refused = GraphSerializer::from_json(target, document);
        INFO(refused.error);
        CHECK_FALSE(refused.ok);
        CHECK(refused.error.starts_with("JSON parse failed:"));
        CHECK(refused.persisted_node_ids.empty());
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(keep) != nullptr);

        // Control: without the injected failure the same load succeeds.
        const auto loaded = GraphSerializer::from_json(target, document);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
        CHECK(target.nodes().size() == 4);
    }
    SignalGraph v7_target;
    REQUIRE(GraphSerializer::from_json(v7_target, documents[0].second).ok);
    CHECK(id_of(v7_target, "Input") == id_of(fixture.graph, "Input"));
}

TEST_CASE("an ordinary load refuses a v7 document relabelled as an older format",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);
    // format_version 6 and no authored_id on any record, with the v7
    // envelope and bindings kept, optionally declaring v7 only through the
    // logical_identities profile.
    const auto downgraded = [&](bool keep_schema) {
        auto doc = choc::json::parse(saved);
        doc.setMember("format_version", static_cast<std::int64_t>(6));
        auto nodes = choc::value::createEmptyArray();
        for (std::uint32_t i = 0; i < doc["nodes"].size(); ++i) {
            const auto record = doc["nodes"][i];
            auto rebuilt = choc::value::createObject("");
            for (std::uint32_t m = 0; m < record.size(); ++m) {
                const auto item = record.getObjectMemberAt(m);
                if (std::string_view(item.name) != "authored_id")
                    rebuilt.addMember(item.name, item.value);
            }
            nodes.addArrayElement(rebuilt);
        }
        doc.setMember("nodes", nodes);
        if (!keep_schema) {
            auto rebuilt = choc::value::createObject("");
            for (std::uint32_t i = 0; i < doc.size(); ++i) {
                const auto item = doc.getObjectMemberAt(i);
                if (std::string_view(item.name) != "identity_schema")
                    rebuilt.addMember(item.name, item.value);
            }
            doc = rebuilt;
        }
        return choc::json::toString(doc, true);
    };
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"v7 identity_schema at format 6", downgraded(true)},
        {"v7 profile only at format 6", downgraded(false)},
    };

    const auto path = std::filesystem::temp_directory_path() / "pulp-graph-v7-downgraded.pulpgraph";
    for (const auto& [label, document] : cases) {
        INFO(label);
        REQUIRE(document.find("authored_id") == std::string::npos);
        REQUIRE(document.find("\"bindings\"") != std::string::npos);
        // Control on the instrument: the inspector refuses each document.
        CHECK_FALSE(GraphSerializer::inspect_document(document).ok());

        SignalGraph target;
        const auto keep = target.add_gain_node("Keep");
        const auto from_json = GraphSerializer::from_json(target, document);
        INFO(from_json.error);
        CHECK_FALSE(from_json.ok);
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(keep) != nullptr);

        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << document;
        }
        const auto from_file = GraphSerializer::from_file(target, path);
        INFO(from_file.error);
        CHECK_FALSE(from_file.ok);
        CHECK(target.nodes().size() == 1);
        CHECK(target.node(keep) != nullptr);
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // Control: the same document at format 7 with its authored IDs loads and
    // keeps them.
    SignalGraph control;
    REQUIRE(GraphSerializer::from_json(control, saved).ok);
    CHECK(id_of(control, "Input") == id_of(fixture.graph, "Input"));
}

TEST_CASE("a failure after a saved document restored leaves no graph and no identity map",
          "[host][graph][identity][v7]") {
    TwinGraph fixture;
    build_twins(fixture);
    const auto saved = GraphSerializer::to_saved_document(fixture.graph);

    SignalGraph target;
    REQUIRE(target.add_gain_node("Keep") != 0);
    GraphSerializerTestAccess::fail_next_identity_record();
    const auto refused = GraphSerializer::from_saved_document(target, saved);
    INFO(refused.error);
    CHECK_FALSE(refused.ok);
    CHECK(refused.error.starts_with("identity_envelope_invalid:"));
    CHECK(target.nodes().empty());
    CHECK(refused.identity_node_ids.empty());
    CHECK(refused.persisted_node_ids.empty());
    CHECK(refused.editor_layout.empty());

    // Control: without the injected failure the same load records every ID.
    const auto loaded = GraphSerializer::from_saved_document(target, saved);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    CHECK(loaded.identity_node_ids.size() == 4);
}
