// Fixed-count voice-template declarations: expansion, the v7 document
// record, restore through the public saved-document path, v6 compatibility,
// planted refusals against a positive control, diff and edit history.

#include "../core/host/src/graph_serializer_test_access.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/format/processor.hpp>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_subgraph.hpp>
#include <pulp/host/subgraph_persistence.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace pulp::host;

namespace {

constexpr std::array<float, 3> kVoiceGains{0.5f, 0.25f, 0.125f};
// Ones through three parallel voices of distinct power-of-two gains: every
// voice contributes a distinct bit, so a lost or swapped voice changes it.
constexpr float kBankSum = 0.875f;

// One voice: a single Gain member with exposed audio ports and a gain
// parameter.
SubgraphTemplate voice_template() {
    SubgraphTemplate t;
    t.identity = "test.voice";
    t.nodes = {{"amp", NodeType::Gain, 1, 1.0f}};
    t.exposed_ports = {{"input", "amp", 0, false, PortKind::Audio},
                       {"output", "amp", 0, true, PortKind::Audio}};
    t.parameters = {{"gain", "amp", 1.0f, 0.0f, 2.0f}};
    return t;
}

// The same identity with a different member structure (two members).
SubgraphTemplate reshaped_voice_template() {
    auto t = voice_template();
    t.nodes.push_back({"trim", NodeType::Gain, 1, 1.0f});
    t.edges = {{"amp", 0, "trim", 0}};
    t.exposed_ports[1].node = "trim";
    return t;
}

struct VoiceRig {
    SignalGraph graph;
    NodeId input = 0;
    NodeId output = 0;
    SubgraphVoiceBank bank;
    std::string document;
};

// input -> three parallel voices (gains 0.5, 0.25, 0.125) -> output.
void build_rig(VoiceRig& rig) {
    rig.input = rig.graph.add_input_node(1);
    rig.output = rig.graph.add_output_node(1);
    auto expansion = instantiate_subgraph_voices(rig.graph, voice_template(), "pad", 3);
    INFO(expansion.result.message());
    REQUIRE(expansion);
    rig.bank = std::move(expansion.bank);
    for (std::uint32_t k = 0; k < kVoiceGains.size(); ++k) {
        auto& voice = rig.bank.voices[k];
        auto state = voice.save_state();
        state.parameters.at("gain") = kVoiceGains[k];
        REQUIRE(restore_subgraph_state(rig.graph, voice, state));
        REQUIRE(rig.graph.connect(rig.input, 0, voice.ports.at("input").node, 0));
        REQUIRE(rig.graph.connect(voice.ports.at("output").node, 0, rig.output, 0));
    }
    rig.document = to_saved_subgraph_document(rig.graph, {}, std::span(&rig.bank, 1), {});
    REQUIRE_FALSE(rig.document.empty());
}

float render_ones(SignalGraph& graph) {
    REQUIRE(graph.prepare(48000.0, 8));
    std::array<float, 8> ones{}, out{};
    ones.fill(1.0f);
    const float* ip[] = {ones.data()};
    float* op[] = {out.data()};
    pulp::audio::BufferView<const float> input(ip, 1, ones.size());
    pulp::audio::BufferView<float> output(op, 1, out.size());
    graph.process(output, input, static_cast<int>(out.size()));
    return out[4];
}

std::vector<SubgraphTemplate> voice_templates() {
    return {voice_template()};
}

// The document with its voice_declarations rewritten by `edit`.
std::string edit_declarations(const std::string& document,
                              const std::function<void(choc::value::Value&)>& edit) {
    auto root = choc::json::parse(document);
    choc::value::Value declarations(root["voice_declarations"]);
    edit(declarations);
    root.setMember("voice_declarations", declarations);
    return choc::json::toString(root, true);
}

// The document with its subgraph records rewritten by `edit`.
std::string edit_records(const std::string& document,
                         const std::function<void(choc::value::Value&)>& edit) {
    auto root = choc::json::parse(document);
    choc::value::Value records(root["subgraphs"]);
    edit(records);
    root.setMember("subgraphs", records);
    return choc::json::toString(root, true);
}

choc::value::Value declaration_at(const choc::value::Value& declarations, std::uint32_t i) {
    return choc::value::Value(declarations[i]);
}

// A refused restore into a graph that already holds a node leaves that
// graph exactly as it was and returns nothing.
void require_refused(const std::string& document, std::string_view rule,
                     const std::vector<SubgraphTemplate>& templates = voice_templates()) {
    SignalGraph graph;
    const auto existing = graph.add_gain_node("existing");
    REQUIRE(existing != 0);
    const auto loaded = from_saved_subgraph_document(graph, document, templates);
    INFO(loaded.load.error);
    CHECK_FALSE(loaded.ok());
    CHECK(loaded.load.error.find(rule) != std::string::npos);
    CHECK(loaded.subgraphs.empty());
    CHECK(loaded.voice_banks.empty());
    CHECK(loaded.load.identity_node_ids.empty());
    REQUIRE(graph.nodes().size() == 1);
    CHECK(graph.nodes().front().id == existing);
}

class IndexedProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor value;
        value.name = "Indexed";
        value.manufacturer = "Pulp Test";
        value.bundle_id = "com.pulp.test.indexed-voice";
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

// A Processor voice whose factory refuses instance index `refused`, so the
// expansion of that voice fails after earlier voices were built.
SubgraphTemplate processor_voice_template(std::uint32_t refused) {
    SubgraphTemplate t;
    t.identity = "test.processor-voice";
    SubgraphNodeSpec node;
    node.key = "processor";
    node.type = NodeType::Plugin;
    node.processor_type_id = "test.indexed-voice";
    node.processor_type_version = 1;
    node.processor_descriptor_version = "1.0.0";
    node.processor_factory =
        [refused](std::uint32_t index) -> std::unique_ptr<pulp::format::Processor> {
        if (index == refused)
            return nullptr;
        return std::make_unique<IndexedProcessor>();
    };
    t.nodes.push_back(std::move(node));
    return t;
}

} // namespace

TEST_CASE("voice declaration expands a fixed count of voices with derived identities",
          "[subgraph][voices]") {
    SignalGraph graph;
    const auto existing = graph.add_gain_node("existing");
    auto expansion = instantiate_subgraph_voices(graph, voice_template(), "pad", 4);
    INFO(expansion.result.message());
    REQUIRE(expansion);
    CHECK(expansion.bank.declaration.identity == "pad");
    CHECK(expansion.bank.declaration.template_identity == "test.voice");
    CHECK(expansion.bank.declaration.voice_count == 4);
    REQUIRE(expansion.bank.voices.size() == 4);
    for (std::uint32_t k = 0; k < 4; ++k) {
        const auto& voice = expansion.bank.voices[k];
        CHECK(voice.instance_identity == "pad.voice." + std::to_string(k));
        CHECK(voice.instance_identity == subgraph_voice_identity("pad", k));
        CHECK(voice.instance_index == k);
        CHECK(voice.template_identity == "test.voice");
        REQUIRE(voice.nodes.size() == 1);
        CHECK(graph.node(voice.nodes[0])->name == voice.instance_identity + ".amp");
    }
    CHECK(graph.nodes().size() == 5);

    // Count, identity and template refusals happen before the graph changes.
    const auto refused = [&](std::string identity, std::uint32_t count,
                             const SubgraphTemplate& definition = voice_template()) {
        SignalGraph target;
        const auto keep = target.add_gain_node("keep");
        const auto result = instantiate_subgraph_voices(target, definition, identity, count);
        INFO(result.result.message());
        CHECK_FALSE(result);
        CHECK(result.result.rule.starts_with("subgraph boundary:"));
        CHECK(result.bank.voices.empty());
        REQUIRE(target.nodes().size() == 1);
        CHECK(target.nodes().front().id == keep);
    };
    refused("pad", 0);
    refused("pad", kMaxSubgraphVoices + 1);
    refused("", 2);
    refused(std::string("p\nad"), 2);
    refused(std::string(129, 'p'), 2);
    SubgraphTemplate anonymous = voice_template();
    anonymous.identity.clear();
    refused("pad", 2, anonymous);
    // A later voice that fails removes the voices already built.
    refused("pad", 3, processor_voice_template(2));
    SignalGraph retry;
    const auto keep_after_failure = retry.add_gain_node("keep");
    const auto failed = instantiate_subgraph_voices(retry, processor_voice_template(2), "pad", 3);
    REQUIRE_FALSE(failed);
    CHECK(retry.add_gain_node("next") == keep_after_failure + 1);

    // Positive controls at both bounds and for the Processor voice.
    SignalGraph bounds;
    CHECK(instantiate_subgraph_voices(bounds, voice_template(), "one", 1));
    CHECK(instantiate_subgraph_voices(bounds, voice_template(), "max", kMaxSubgraphVoices));
    CHECK(instantiate_subgraph_voices(bounds, processor_voice_template(99), "proc", 3));
    CHECK(graph.node(existing) != nullptr);
}

TEST_CASE("voice bank round-trips through the v7 document with an audio render across voices",
          "[subgraph][voices][persistence]") {
    VoiceRig rig;
    build_rig(rig);
    CHECK(render_ones(rig.graph) == kBankSum);

    const auto inspection = GraphSerializer::inspect_document(rig.document);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    CHECK(inspection.document.profile() == "pulp.graph.saved-identity.v7");
    const auto root = choc::json::parse(rig.document);
    REQUIRE(root.hasObjectMember("voice_declarations"));
    const auto& declaration = root["voice_declarations"][0];
    CHECK(declaration["identity"].getString() == "pad");
    CHECK(declaration["template_identity"].getString() == "test.voice");
    CHECK(declaration["voice_count"].getInt64() == 3);
    CHECK(declaration["voices"][2].getString() == "pad.voice.2");
    CHECK(root["subgraphs"].size() == 3);
    // Only the declaration and authored values persist: nothing names a
    // runtime voice, allocator, token, epoch, pedal or tail.
    for (const auto* runtime : {"allocator", "token", "epoch", "pedal", "tail", "sounding"})
        CHECK(rig.document.find(runtime) == std::string::npos);

    SignalGraph restored;
    const auto loaded = from_saved_subgraph_document(restored, rig.document, voice_templates());
    INFO(loaded.load.error);
    REQUIRE(loaded.ok());
    CHECK(loaded.subgraphs.empty());
    REQUIRE(loaded.voice_banks.size() == 1);
    const auto& bank = loaded.voice_banks[0];
    CHECK(bank.declaration.identity == "pad");
    CHECK(bank.declaration.template_identity == "test.voice");
    CHECK(bank.declaration.voice_count == 3);
    REQUIRE(bank.voices.size() == 3);
    for (std::uint32_t k = 0; k < 3; ++k) {
        const auto& voice = bank.voices[k];
        CHECK(voice.instance_identity == subgraph_voice_identity("pad", k));
        CHECK(voice.instance_index == k);
        CHECK(restored.node(voice.parameter_nodes.at("gain"))->gain == kVoiceGains[k]);
        CHECK(restored.node(voice.nodes[0])->authored_id.str() ==
              rig.graph.node(rig.bank.voices[k].nodes[0])->authored_id.str());
    }
    // Every voice renders: the fixed oracle sums one distinct bit per voice.
    CHECK(render_ones(restored) == kBankSum);
    CHECK(to_saved_subgraph_document(restored, loaded.subgraphs, loaded.voice_banks, {}) ==
          rig.document);

    // Restored handles are live: editing one voice changes only its bit.
    auto voice = bank.voices[1];
    auto state = voice.save_state();
    state.parameters.at("gain") = 0.0f;
    REQUIRE(restore_subgraph_state(restored, voice, state));
    CHECK(render_ones(restored) == kBankSum - kVoiceGains[1]);

    // A plain instance and a bank in one document restore side by side.
    auto plain = instantiate_subgraph(rig.graph, voice_template(), "solo", 9);
    REQUIRE(plain);
    const auto mixed = to_saved_subgraph_document(rig.graph, std::span(&plain.instance, 1),
                                                  std::span(&rig.bank, 1), {});
    REQUIRE_FALSE(mixed.empty());
    SignalGraph both;
    const auto mixed_load = from_saved_subgraph_document(both, mixed, voice_templates());
    INFO(mixed_load.load.error);
    REQUIRE(mixed_load.ok());
    REQUIRE(mixed_load.subgraphs.size() == 1);
    CHECK(mixed_load.subgraphs[0].instance_identity == "solo");
    REQUIRE(mixed_load.voice_banks.size() == 1);
    CHECK(mixed_load.voice_banks[0].voices.size() == 3);
}

TEST_CASE("documents without voice declarations load unchanged and v6 cannot carry one",
          "[subgraph][voices][persistence]") {
    VoiceRig rig;
    build_rig(rig);

    // A v6 document of the same flattened graph loads as before.
    const auto legacy = GraphSerializerTestAccess::legacy_v6_saved_document(rig.graph);
    const auto inspection = GraphSerializer::inspect_document(legacy);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    REQUIRE(inspection.document.profile() == "pulp.graph.saved-identity.v6");
    {
        SignalGraph graph;
        const auto loaded = from_saved_subgraph_document(graph, legacy, voice_templates());
        INFO(loaded.load.error);
        REQUIRE(loaded.ok());
        CHECK(loaded.voice_banks.empty());
        CHECK(loaded.subgraphs.empty());
        CHECK(graph.nodes().size() == rig.graph.nodes().size());
        CHECK(render_ones(graph) == kBankSum);
        SignalGraph plain;
        CHECK(GraphSerializer::from_saved_document(plain, legacy).ok);
        CHECK(plain.nodes().size() == rig.graph.nodes().size());
    }

    // The same section smuggled into the v6 envelope is refused.
    auto root = choc::json::parse(legacy);
    root.addMember("voice_declarations",
                   choc::value::Value(choc::json::parse(rig.document)["voice_declarations"]));
    const auto smuggled = choc::json::toString(root, true);
    CHECK(GraphSerializer::inspect_document(smuggled).refusal ==
          GraphDocumentRefusal::SubgraphRecordInvalid);
    require_refused(smuggled, "subgraph_record_invalid");
    SignalGraph plain;
    CHECK_FALSE(GraphSerializer::from_saved_document(plain, smuggled).ok);

    // A v7 document written without banks has no declaration section.
    const auto v7 = to_saved_subgraph_document(rig.graph, {});
    // The original three-argument form (an explicit empty layout) still resolves
    // to the plain overload rather than becoming ambiguous with the voice-bank one.
    CHECK(to_saved_subgraph_document(rig.graph, {}, {}) == v7);
    REQUIRE_FALSE(v7.empty());
    CHECK_FALSE(choc::json::parse(v7).hasObjectMember("voice_declarations"));
}

TEST_CASE("planted voice declaration refusals leave the graph untouched",
          "[subgraph][voices][persistence]") {
    VoiceRig rig;
    build_rig(rig);

    SECTION("positive control: the unedited document through the same edit path loads") {
        const auto same = edit_declarations(rig.document, [](choc::value::Value&) {});
        SignalGraph graph;
        graph.add_gain_node("existing");
        const auto loaded = from_saved_subgraph_document(graph, same, voice_templates());
        INFO(loaded.load.error);
        REQUIRE(loaded.ok());
        CHECK(loaded.voice_banks.size() == 1);
        CHECK(render_ones(graph) == kBankSum);
    }
    SECTION("count 0") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            declaration.setMember("voice_count", std::int64_t{0});
            declaration.setMember("voices", choc::value::createEmptyArray());
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        CHECK(GraphSerializer::inspect_document(document).refusal ==
              GraphDocumentRefusal::SubgraphRecordInvalid);
        require_refused(document, "voice_count must be an integer in 1..16");
    }
    SECTION("count over the bound") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            declaration.setMember("voice_count", std::int64_t{kMaxSubgraphVoices + 1});
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        require_refused(document, "voice_count must be an integer in 1..16");
    }
    SECTION("count that disagrees with the listed voices") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            declaration.setMember("voice_count", std::int64_t{2});
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        require_refused(document, "voices must list exactly voice_count identities");
    }
    SECTION("a voice identity claimed twice") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            auto voices = choc::value::createEmptyArray();
            for (const auto* id : {"pad.voice.0", "pad.voice.0", "pad.voice.2"})
                voices.addArrayElement(std::string(id));
            declaration.setMember("voices", voices);
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        CHECK(GraphSerializer::inspect_document(document).refusal ==
              GraphDocumentRefusal::SubgraphBindingDuplicate);
        require_refused(document, "subgraph_binding_duplicate");
    }
    SECTION("two declarations with one identity") {
        const auto document = edit_declarations(
            rig.document, [](choc::value::Value& d) { d.addArrayElement(declaration_at(d, 0)); });
        CHECK(GraphSerializer::inspect_document(document).refusal ==
              GraphDocumentRefusal::SubgraphBindingDuplicate);
        require_refused(document, "duplicate voice declaration identity");
    }
    SECTION("a voice identity that is not the derived one") {
        // The record is renamed with it, so the binding is consistent and
        // only the derivation is wrong.
        auto document = edit_records(rig.document, [](choc::value::Value& r) {
            choc::value::Value record(r[0]);
            record.setMember("instance_identity", std::string("pad.voice.7"));
            auto copy = choc::value::createEmptyArray();
            copy.addArrayElement(record);
            for (std::uint32_t i = 1; i < r.size(); ++i)
                copy.addArrayElement(choc::value::Value(r[i]));
            r = copy;
        });
        document = edit_declarations(document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            auto voices = choc::value::createEmptyArray();
            for (const auto* id : {"pad.voice.7", "pad.voice.1", "pad.voice.2"})
                voices.addArrayElement(std::string(id));
            declaration.setMember("voices", voices);
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        require_refused(document, "not the derived identity pad.voice.0");
    }
    SECTION("a declared voice without a record") {
        const auto document = edit_records(rig.document, [](choc::value::Value& r) {
            auto copy = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < r.size(); ++i)
                if (r[i]["instance_identity"].getString() != "pad.voice.1")
                    copy.addArrayElement(choc::value::Value(r[i]));
            r = copy;
        });
        CHECK(GraphSerializer::inspect_document(document).refusal ==
              GraphDocumentRefusal::SubgraphBindingDangling);
        require_refused(document, "subgraph_binding_dangling");
    }
    SECTION("a declaration whose template differs from its voice records") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            declaration.setMember("template_identity", std::string("test.other"));
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        require_refused(document, "record must use template test.other");
    }
    SECTION("an unknown declaration member") {
        const auto document = edit_declarations(rig.document, [](choc::value::Value& d) {
            auto declaration = declaration_at(d, 0);
            declaration.addMember("allocator", std::int64_t{3});
            d = choc::value::createEmptyArray();
            d.addArrayElement(declaration);
        });
        require_refused(document, "requires exactly identity, template_identity");
    }
    SECTION("missing template") {
        require_refused(rig.document, "subgraph_template_unknown", {});
    }
    SECTION("member-key mismatch: the template's members differ from the records") {
        require_refused(rig.document, "subgraph_instance_mismatch", {reshaped_voice_template()});
    }
    SECTION("member-key mismatch: one voice record lost a member") {
        auto with_extra = reshaped_voice_template();
        with_extra.identity = "test.voice";
        // Records written from the two-member template, restored against it
        // after one voice's second member was dropped.
        SignalGraph graph;
        auto expansion = instantiate_subgraph_voices(graph, with_extra, "pad", 2);
        REQUIRE(expansion);
        const auto document = to_saved_subgraph_document(graph, {}, std::span(&expansion.bank, 1), {});
        REQUIRE_FALSE(document.empty());
        const auto shortened = edit_records(document, [](choc::value::Value& r) {
            auto copy = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < r.size(); ++i) {
                choc::value::Value record(r[i]);
                if (record["instance_identity"].getString() == "pad.voice.1") {
                    auto members = choc::value::createEmptyArray();
                    members.addArrayElement(choc::value::Value(record["members"][0]));
                    record.setMember("members", members);
                }
                copy.addArrayElement(record);
            }
            r = copy;
        });
        require_refused(shortened, "member count does not match", {with_extra});
        SignalGraph control;
        CHECK(from_saved_subgraph_document(control, document, std::vector{with_extra}).ok());
    }
}

TEST_CASE("voice bank writer refuses banks a restore would refuse", "[subgraph][voices]") {
    VoiceRig rig;
    build_rig(rig);
    const auto write = [&](const std::vector<SubgraphVoiceBank>& banks,
                           const std::vector<SubgraphInstance>& plain = {}) {
        return to_saved_subgraph_document(rig.graph, plain, banks, {});
    };
    CHECK_FALSE(write({rig.bank}).empty()); // positive control

    auto short_bank = rig.bank;
    short_bank.voices.pop_back();
    CHECK(write({short_bank}).empty());

    auto zero = rig.bank;
    zero.declaration.voice_count = 0;
    zero.voices.clear();
    CHECK(write({zero}).empty());

    auto swapped = rig.bank;
    std::swap(swapped.voices[0], swapped.voices[1]);
    CHECK(write({swapped}).empty());

    auto foreign_template = rig.bank;
    foreign_template.declaration.template_identity = "test.other";
    CHECK(write({foreign_template}).empty());

    auto bad_identity = rig.bank;
    bad_identity.declaration.identity = "p\tad";
    CHECK(write({bad_identity}).empty());

    CHECK(write({rig.bank, rig.bank}).empty());
    // A voice also passed as a plain instance collides by identity and node.
    CHECK(write({rig.bank}, {rig.bank.voices[0]}).empty());
}

TEST_CASE("voice banks diff and undo by authored identity", "[subgraph][voices][history]") {
    VoiceRig rig;
    build_rig(rig);

    // Reload and re-save renumbers every NodeId; the diff is still equal.
    SignalGraph restored;
    const auto loaded = from_saved_subgraph_document(restored, rig.document, voice_templates());
    REQUIRE(loaded.ok());
    const auto resaved = to_saved_subgraph_document(restored, {}, loaded.voice_banks, {});
    const auto same = diff_graph_documents(rig.document, resaved);
    INFO(same.detail);
    REQUIRE(same.ok());
    CHECK(same.equal());

    // A per-voice value edit is a changed node, named by authored ID.
    auto voice = loaded.voice_banks[0].voices[2];
    auto state = voice.save_state();
    state.parameters.at("gain") = 1.5f;
    REQUIRE(restore_subgraph_state(restored, voice, state));
    const auto edited =
        diff_graph_documents(resaved, to_saved_subgraph_document(restored, {}, loaded.voice_banks, {}));
    REQUIRE(edited.ok());
    REQUIRE(edited.nodes_changed.size() == 1);
    CHECK(edited.nodes_changed[0].identity == restored.node(voice.nodes[0])->authored_id.str());
    CHECK(edited.nodes_changed[0].identity ==
          rig.graph.node(rig.bank.voices[2].nodes[0])->authored_id.str());

    // A second bank is new nodes by authored ID plus changed declaration
    // and record sections.
    const auto checkpoint = GraphEditHistory::checkpoint(rig.graph);
    GraphEditHistory history;
    auto lead = instantiate_subgraph_voices(rig.graph, voice_template(), "lead", 2);
    REQUIRE(lead);
    REQUIRE(history.commit(checkpoint, rig.graph, "add lead voices").ok());
    const std::array banks{rig.bank, lead.bank};
    const auto grown = to_saved_subgraph_document(rig.graph, {}, banks, {});
    REQUIRE_FALSE(grown.empty());
    const auto diff = diff_graph_documents(rig.document, grown);
    REQUIRE(diff.ok());
    CHECK_FALSE(diff.equal());
    std::vector<std::string> lead_ids;
    for (const auto& voice : lead.bank.voices)
        lead_ids.push_back(rig.graph.node(voice.nodes[0])->authored_id.str());
    auto added = diff.nodes_added;
    std::sort(added.begin(), added.end());
    std::sort(lead_ids.begin(), lead_ids.end());
    CHECK(added == lead_ids);
    CHECK(std::find(diff.sections_changed.begin(), diff.sections_changed.end(),
                    "voice_declarations") != diff.sections_changed.end());
    CHECK(std::find(diff.sections_changed.begin(), diff.sections_changed.end(), "subgraphs") !=
          diff.sections_changed.end());

    // Undo removes every lead voice member; redo brings each back by ID.
    const auto before_undo = rig.graph.nodes().size();
    const auto undone = history.undo(rig.graph);
    INFO(undone.detail);
    REQUIRE(undone.ok());
    CHECK(rig.graph.nodes().size() == before_undo - 2);
    for (const auto& id : lead_ids)
        CHECK_FALSE(undone.node_ids.contains(id));
    const auto redone = history.redo(rig.graph);
    INFO(redone.detail);
    REQUIRE(redone.ok());
    for (const auto& id : lead_ids)
        CHECK(redone.node_ids.contains(id));
}
