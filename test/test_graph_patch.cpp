#include "../examples/graph-patch-consumer/hello.hpp"
#include <algorithm>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/spectral_graph.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>
#include <vector>

using namespace pulp::host;
using Code = GraphPatchDiagnostic::Code;
namespace {
std::string hello() {
    std::ifstream f(PULP_PATCH_HELLO);
    REQUIRE(f.good());
    return {std::istreambuf_iterator<char>(f), {}};
}
std::string replaced(std::string value, const std::string& from, const std::string& to) {
    const auto i = value.find(from);
    REQUIRE(i != value.npos);
    value.replace(i, from.size(), to);
    return value;
}
} // namespace

TEST_CASE("Patch catalog projects runtime ports and parameters", "[host][patch]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    const auto json = choc::json::parse(c.to_json());
    REQUIRE(json["schema"].getString() == "pulp.graph-patch-catalog.v1");
    bool found = false;
    for (const auto row : json["types"])
        if (row["type_id"].getString() == "com.example.PatchGain") {
            found = true;
            REQUIRE(row["inputs"].size() == 2);
            REQUIRE(row["inputs"][1]["name"].getString() == "midi");
            REQUIRE(row["parameters"][0]["name"].getString() == "level");
            REQUIRE(row["parameters"][0]["max"].getInt64() == 2);
        }
    REQUIRE(found);
}
TEST_CASE("Named MIDI ports preserve audio buses and true routing", "[host][patch][named-midi]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto g = c.make_graph();
    const auto m = g->add_midi_input_node("m");
    const auto v = c.instantiate(*g, "com.example.PatchGain", 1, "v");
    const auto out = g->add_midi_output_node("out");
    REQUIRE(g->node(v)->num_input_ports == 1);
    REQUIRE(g->node(v)->num_output_ports == 1);
    const auto source = g->node(m)->out("audio");
    const auto sink = g->node(v)->in("midi");
    REQUIRE(g->connect(source, sink));
    REQUIRE(g->connections().back().midi);
    REQUIRE(g->connections().back().source_port == 0);
    REQUIRE(g->connect(source, sink).code == GraphResult::Code::Duplicate);
    REQUIRE(g->connect(g->node(v)->out("midi"), g->node(out)->in("audio")));
    REQUIRE(g->connect(g->node(v)->out("midi"), sink).code == GraphResult::Code::IllegalCycle);
    REQUIRE(g->connect(g->node(v)->out("audio"), sink).code == GraphResult::Code::KindMismatch);
    const auto saved = GraphSerializer::to_json(*g);
    auto restored = load_graph_patch_json(saved, c);
    REQUIRE(restored);
    REQUIRE(restored.graph->connections().size() == 2);
    for (const auto& edge : restored.graph->connections())
        REQUIRE(edge.midi);
    auto foreign = c.make_graph();
    const auto foreign_m = foreign->add_midi_input_node("m");
    REQUIRE(g->connect(foreign->node(foreign_m)->out("audio"), sink).code ==
            GraphResult::Code::UnknownPort);
    g->remove_node(v);
    REQUIRE(g->connect(source, sink).code == GraphResult::Code::UnknownNode);
}
TEST_CASE("Port alias midi names a node's single MIDI port", "[host][patch][named-midi]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto g = c.make_graph();
    const auto m = g->add_midi_input_node("m");
    const auto v = c.instantiate(*g, "com.example.PatchGain", 1, "v");
    const auto out = g->add_midi_output_node("out");
    const auto in = g->add_input_node(2, "in");

    // The MIDI boundary's real port keeps its name; the alias resolves to it.
    const auto alias_source = g->node(m)->out("midi");
    REQUIRE(alias_source.name == "audio");
    REQUIRE(alias_source.kind == PortKind::Midi);
    REQUIRE(g->connect(alias_source, g->node(v)->in("midi")));
    REQUIRE(g->connections().back().midi);
    REQUIRE(g->connect(g->node(m)->out("audio"), g->node(v)->in("midi")).code ==
            GraphResult::Code::Duplicate);
    REQUIRE(g->connect(g->node(v)->out("midi"), g->node(out)->in("midi")));

    // Refusal: a node without a MIDI port has no alias.
    REQUIRE(g->node(in)->out("midi").kind == PortKind::Audio);
    REQUIRE(g->connect(g->node(in)->out("midi"), g->node(v)->in("audio")).code ==
            GraphResult::Code::UnknownPort);

    // Refusal: two MIDI ports make the alias ambiguous, so it resolves to neither.
    CustomNodeType two;
    two.type_id = "pulp.test.two-midi";
    two.num_input_ports = 2;
    two.num_output_ports = 1;
    two.input_ports = {{"notes", PortKind::Midi}, {"clock", PortKind::Midi}};
    two.output_ports = {{"audio", PortKind::Audio}};
    two.process = [](pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                     int) {};
    REQUIRE(g->register_custom_node_type(two));
    const auto both = g->add_custom_node("pulp.test.two-midi");
    REQUIRE(both != 0);
    REQUIRE(g->node(both)->in("notes").kind == PortKind::Midi);
    REQUIRE(g->node(both)->in("midi").kind == PortKind::Audio);
    REQUIRE(g->connect(g->node(m)->out("midi"), g->node(both)->in("midi")).code ==
            GraphResult::Code::KindMismatch);
}
TEST_CASE("Notation accepts the midi port alias and keeps the canonical spelling",
          "[host][patch][named-midi]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    const auto canonical = parse_graph_patch(hello(), c);
    REQUIRE(canonical);
    const auto aliased = parse_graph_patch(
        replaced(hello(), "midi.audio >> voice.midi", "midi.midi >> voice.midi"), c);
    INFO(aliased.diagnostic.message());
    REQUIRE(aliased);
    REQUIRE(aliased.graph->connections().size() == canonical.graph->connections().size());
    REQUIRE(aliased.graph->connections()[0].midi);
    const auto formatted = format_graph_patch(*aliased.graph, c);
    REQUIRE(formatted);
    REQUIRE(formatted.text == format_graph_patch(*canonical.graph, c).text);
    REQUIRE(formatted.text.find("midi.audio >> voice.midi") != std::string::npos);

    // Refusal: an audio input boundary has no MIDI port, so the alias is unknown.
    const auto refused = parse_graph_patch(
        replaced(hello(), "midi.audio >> voice.midi", "input.midi >> voice.midi"), c);
    REQUIRE_FALSE(refused);
    REQUIRE_FALSE(refused.graph);
    REQUIRE(refused.diagnostic.code == Code::UnknownPort);
    REQUIRE(refused.diagnostic.rule == "unknown port 'midi'");
    REQUIRE(refused.diagnostic.suggested_fix == "use one of: audio, out1");
}
TEST_CASE("Notation graph serialization roundtrip is deterministic", "[host][patch][roundtrip]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto first = parse_graph_patch(hello(), c);
    INFO(first.diagnostic.message());
    REQUIRE(first);
    auto formatted = format_graph_patch(*first.graph, c);
    INFO(formatted.diagnostic.message());
    REQUIRE(formatted);
    auto saved = GraphSerializer::to_json(*first.graph);
    auto loaded = load_graph_patch_json(saved, c);
    INFO(loaded.diagnostic.message());
    REQUIRE(loaded);
    auto again = format_graph_patch(*loaded.graph, c);
    INFO(again.diagnostic.message());
    REQUIRE(again);
    REQUIRE(formatted.text == again.text);
    auto parsed = parse_graph_patch(again.text, c);
    REQUIRE(parsed);
    REQUIRE(format_graph_patch(*parsed.graph, c).text == again.text);
    REQUIRE(first.graph->connections().size() == 4);
    REQUIRE(first.graph->connections()[0].midi);
}
TEST_CASE("Notation refuses wrong ports kinds cycles and parameter ranges",
          "[host][patch][controls]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    struct Bad {
        const char* before;
        const char* after;
        Code code;
        const char* fix;
    };
    for (const auto& bad :
         {Bad{"input.audio >> voice.audio", "input.typo >> voice.audio", Code::UnknownPort,
              "use one of"},
          Bad{"midi.audio >> voice.midi", "midi.audio >> voice.audio", Code::KindMismatch,
              "same kind"},
          Bad{"voice.level = 0.75", "voice.level = 3", Code::ParameterOutOfRange, "[0, 2]"},
          Bad{"voice.level = 0.75", "voice.audio >> voice.audio", Code::IllegalCycle,
              "feedback delay"}}) {
        DYNAMIC_SECTION(graph_patch_code_name(bad.code)) {
            const auto result = parse_graph_patch(replaced(hello(), bad.before, bad.after), c);
            INFO(result.diagnostic.message());
            REQUIRE_FALSE(result);
            REQUIRE_FALSE(result.graph);
            REQUIRE(result.diagnostic.code == bad.code);
            REQUIRE(result.diagnostic.line > 0);
            REQUIRE(result.diagnostic.message().find(bad.fix) != std::string::npos);
        }
    }
}
TEST_CASE("Notation explains built-in gain automation and chain port inference",
          "[host][patch][controls]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    const auto gain_route = parse_graph_patch("# flow 1\n"
                                              "input : pulp/audio_in\n"
                                              "g : pulp/gain\n"
                                              "input.audio ~> g.gain @ 0.5\n",
                                              c);
    REQUIRE_FALSE(gain_route);
    REQUIRE(gain_route.diagnostic.code == Code::UnknownPort);
    REQUIRE(gain_route.diagnostic.rule ==
            "built-in pulp/gain exposes 'gain' as a node setting, not an automatable parameter");
    REQUIRE(gain_route.diagnostic.suggested_fix ==
            "set it with g.gain = <value>, or automate a registered Processor parameter");

    const auto chain = parse_graph_patch("# flow 1\n"
                                         "input : pulp/audio_in\n"
                                         "voice : \"com.example.PatchGain\"\n"
                                         "out : pulp/audio_out\n"
                                         "input.audio >> voice.audio >> out.audio\n",
                                         c);
    REQUIRE_FALSE(chain);
    REQUIRE(chain.diagnostic.rule ==
            "ambiguous port: node 'voice' has 2 output ports and a chain hop infers a port only "
            "when the node has exactly one output");
    REQUIRE(chain.diagnostic.suggested_fix ==
            "write the port explicitly, e.g. voice.audio >> next.audio; use one of: audio, midi");
}
TEST_CASE("Strict JSON never certifies connections the legacy loader drops",
          "[host][patch][strict]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "in");
    const auto gain = g.add_gain_node("gain");
    REQUIRE(g.connect(g.node(in)->out("audio"), g.node(gain)->in("audio")));
    const auto saved = GraphSerializer::to_json(g);
    REQUIRE(load_graph_patch_json(saved));
    for (const auto& field : {"source_node", "source_port"}) {
        auto root = choc::json::parse(saved);
        auto damaged_edge = choc::value::Value(root["connections"][0]);
        damaged_edge.setMember(field, 999);
        auto damaged_edges = choc::value::createEmptyArray();
        damaged_edges.addArrayElement(damaged_edge);
        root.setMember("connections", damaged_edges);
        const auto damaged = choc::json::toString(root);
        SignalGraph tolerant;
        REQUIRE(GraphSerializer::from_json(tolerant, damaged).ok);
        REQUIRE(tolerant.connections().empty());
        const auto strict = load_graph_patch_json(damaged);
        REQUIRE_FALSE(strict);
        REQUIRE(strict.diagnostic.code ==
                (std::string(field) == "source_node" ? Code::UnknownNode : Code::UnknownPort));
        REQUIRE_FALSE(strict.diagnostic.suggested_fix.empty());
    }
    auto root = choc::json::parse(saved);
    auto back = root["connections"][0];
    auto edge = choc::value::Value(back);
    edge.setMember("source_node", int64_t(gain));
    edge.setMember("dest_node", int64_t(gain));
    auto damaged_edges = choc::value::Value(root["connections"]);
    damaged_edges.addArrayElement(edge);
    root.setMember("connections", damaged_edges);
    REQUIRE(load_graph_patch_json(choc::json::toString(root)).diagnostic.code ==
            Code::IllegalCycle);
}
TEST_CASE("Notation token grammar handles chains fanout quotes and finite exponents",
          "[host][patch][grammar]") {
    GraphPatchCatalog catalog;
    CustomNodeType pipe;
    pipe.type_id = "test.pipe";
    pipe.num_input_ports = 1;
    pipe.num_output_ports = 1;
    pipe.process = [](auto&, const auto&, int) {};
    REQUIRE(catalog.register_custom(pipe));
    const std::string text =
        "# flow 1\na : pulp/audio_in\nb : \"test.pipe\"\nc : pulp/audio_out\ng : "
        "pulp/gain\na.\"audio\" >> b.'audio' >> c.audio, c.in1\ng.gain = 1e-05\n";
    auto result = parse_graph_patch(text, catalog);
    INFO(result.diagnostic.message());
    REQUIRE(result);
    REQUIRE(result.graph->connections().size() == 3);
    REQUIRE(format_graph_patch(*result.graph, catalog));
    REQUIRE_FALSE(parse_graph_patch(replaced(text, "# flow 1", "# flow 2")));
    REQUIRE_FALSE(parse_graph_patch(replaced(text, "1e-05", "nan")));
}
TEST_CASE("Offline notation hello has independently calculated audio", "[host][patch][numeric]") {
    for (const int block : {16, 64, 127}) {
        GraphPatchCatalog c;
        register_patch_gain(c);
        auto parsed = parse_graph_patch(hello(), c);
        REQUIRE(parsed);
        OfflineSignalGraphHost host(*parsed.graph);
        REQUIRE(host.prepare({.sample_rate = 48000,
                              .block_frames = block,
                              .input_channels = 2,
                              .output_channels = 2}));
        pulp::audio::Buffer<float> input(2, 511);
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (std::size_t i = 0; i < 511; ++i)
                input.channel(ch)[i] = 0.5f;
        const auto rendered =
            host.render({.frame_count = 511, .input = std::as_const(input).view()});
        REQUIRE(rendered.ok);
        // Input .5 maps through depth .5 over [0,2] to level .5; .5 * .5 = .25.
        for (const float sample : rendered.audio.channel(0))
            REQUIRE(sample == 0.25f);
        for (const float sample : rendered.audio.channel(1))
            REQUIRE(sample == 0.0f);
    }
}

TEST_CASE("Named Processor audio and MIDI can share a node pair", "[host][patch][named-midi]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto g = c.make_graph();
    const auto a = c.instantiate(*g, "com.example.PatchGain", 1, "a");
    const auto b = c.instantiate(*g, "com.example.PatchGain", 1, "b");
    REQUIRE(g->connect(g->node(a)->out("audio"), g->node(b)->in("audio")));
    REQUIRE(g->connect(g->node(a)->out("midi"), g->node(b)->in("midi")));
    REQUIRE(g->connections().size() == 2);
    REQUIRE_FALSE(g->connections()[0].midi);
    REQUIRE(g->connections()[1].midi);
}
TEST_CASE("MIDI notation edge changes the independently calculated render",
          "[host][patch][numeric][named-midi]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto patch = parse_graph_patch(hello(), c);
    REQUIRE(patch);
    OfflineSignalGraphHost host(*patch.graph);
    REQUIRE(host.prepare({.block_frames = 64}));
    NodeId input_node = 0;
    for (const auto& n : patch.graph->nodes())
        if (n.name == "midi")
            input_node = n.id;
    pulp::midi::MidiBuffer midi;
    REQUIRE(midi.add(pulp::midi::MidiEvent::note_on(0, 60, 100)));
    REQUIRE(patch.graph->inject_midi(input_node, midi));
    pulp::audio::Buffer<float> input(2, 128);
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (auto& sample : input.channel(ch))
            sample = 0.5f;
    const auto audio = host.render({.frame_count = 128, .input = std::as_const(input).view()});
    REQUIRE(audio.ok);
    for (std::size_t i = 0; i < 128; ++i)
        REQUIRE(audio.audio.channel(0)[i] == (i < 64 ? 0.375f : 0.25f));
}
TEST_CASE("Opaque and unsupported state contracts are explicit refusals",
          "[host][patch][strict][opaque-strict]") {
    GraphPatchCatalog catalog;
    CustomNodeType type;
    type.type_id = "test.opaque";
    type.num_output_ports = 1;
    type.create = [] { return new int(1); };
    type.destroy = [](void* p) { delete static_cast<int*>(p); };
    type.process_instance = [](void*, auto&, const auto&, int) {};
    type.save_state = [](void*) { return std::vector<uint8_t>{1}; };
    type.load_state = [](void*, const auto&) { return false; };
    REQUIRE(catalog.register_custom(type));
    auto patch = parse_graph_patch("# flow 1\nx : \"test.opaque\"\n", catalog);
    REQUIRE_FALSE(patch);
    REQUIRE(patch.diagnostic.code == Code::UnsupportedContract);
    REQUIRE(patch.diagnostic.message().find("opaque") != std::string::npos);
    auto g = catalog.make_graph();
    const auto opaque = g->add_custom_node(type.type_id, 1, "x");
    REQUIRE(opaque);
    REQUIRE(g->set_custom_node_state(opaque, {42}));
    REQUIRE_FALSE(format_graph_patch(*g, catalog));
    const auto serialized = GraphSerializer::to_json(*g);
    SignalGraph legacy;
    REQUIRE(legacy.register_custom_node_type(type));
    REQUIRE(GraphSerializer::from_json(legacy, serialized).ok);
    REQUIRE(legacy.nodes().front().custom_state_pending);
    REQUIRE(legacy.custom_node_state(legacy.nodes().front().id) == g->custom_node_state(opaque));
    const auto strict = load_graph_patch_json(serialized, catalog);
    REQUIRE_FALSE(strict);
    REQUIRE(strict.diagnostic.code == Code::UnsupportedContract);
    REQUIRE(strict.diagnostic.message().find("deferred") != std::string::npos);
    SignalGraph feedback;
    auto a = feedback.add_gain_node("a");
    auto b = feedback.add_gain_node("b");
    REQUIRE(feedback.connect_feedback(a, 0, b, 0));
    REQUIRE(format_graph_patch(feedback).diagnostic.code == Code::UnsupportedContract);
}

TEST_CASE("Formatting refuses unrepresentable node scalars and shapes", "[host][patch][strict]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    auto g = c.make_graph();
    const auto id = c.instantiate(*g, "com.example.PatchGain", 1, "voice");
    REQUIRE(id != 0);
    g->set_node_gain(id, 0.5f);
    REQUIRE(format_graph_patch(*g, c).diagnostic.code == Code::UnsupportedContract);
    // A mono boundary is spelled by flow 2; a layout outside [1, 16] is not.
    SignalGraph boundary;
    boundary.add_input_node(1, "in");
    const auto mono = format_graph_patch(boundary);
    INFO(mono.diagnostic.message());
    REQUIRE(mono.text == "# flow 2\nin : pulp/audio_in (channels 1)\n");
    SignalGraph wide;
    wide.add_input_node(17, "in");
    REQUIRE(format_graph_patch(wide).diagnostic.code == Code::UnsupportedChannelLayout);
    REQUIRE(parse_graph_patch("# flow 1\nx :\n").diagnostic.code == Code::SyntaxError);
    SignalGraph gain;
    gain.add_gain_node("gain");
    auto json = choc::json::parse(GraphSerializer::to_json(gain));
    auto node = choc::value::Value(json["nodes"][0]);
    node.setMember("gain", "not-a-number");
    auto nodes = choc::value::createEmptyArray();
    nodes.addArrayElement(node);
    json.setMember("nodes", nodes);
    REQUIRE_FALSE(load_graph_patch_json(choc::json::toString(json)));
}

TEST_CASE("Strict JSON validation still refuses spectral nodes and frame edges",
          "[host][patch][strict][spectral-notation]") {
    SignalGraph gains;
    const auto a = gains.add_gain_node("a");
    const auto b = gains.add_gain_node("b");
    REQUIRE(gains.connect(a, 0, b, 0));
    const auto ordinary = GraphSerializer::to_json(gains);
    REQUIRE(load_graph_patch_json(ordinary));

    auto typed = choc::json::parse(ordinary);
    auto node = choc::value::Value(typed["nodes"][0]);
    node.setMember("type", "spectral_analysis");
    auto nodes = choc::value::createEmptyArray();
    nodes.addArrayElement(node);
    nodes.addArrayElement(typed["nodes"][1]);
    typed.setMember("nodes", nodes);
    const auto typed_result = load_graph_patch_json(choc::json::toString(typed));
    REQUIRE_FALSE(typed_result);
    REQUIRE(typed_result.diagnostic.code == Code::UnsupportedContract);

    auto edged = choc::json::parse(ordinary);
    auto edge = choc::value::Value(edged["connections"][0]);
    edge.setMember("spectral", true);
    auto edges = choc::value::createEmptyArray();
    edges.addArrayElement(edge);
    edged.setMember("connections", edges);
    const auto edge_result = load_graph_patch_json(choc::json::toString(edged));
    REQUIRE_FALSE(edge_result);
    REQUIRE(edge_result.diagnostic.code == Code::UnsupportedContract);
}

TEST_CASE("Patch catalog lists spectral constructors with their notation contract",
          "[host][patch][spectral-notation]") {
    const auto json = choc::json::parse(GraphPatchCatalog{}.to_json());
    const struct {
        const char* key;
        const char* constructor;
        const char* input;
        const char* input_kind;
    } expected[] = {
        {"pulp/spectral_analysis", "SignalGraph::add_spectral_analysis", "audio", "audio"},
        {"pulp/spectral_stage", "SignalGraph::add_spectral_stage", "frames_in", "spectral"},
        {"pulp/spectral_synthesis", "SignalGraph::add_spectral_synthesis", "frames", "spectral"},
        {"pulp/spectral_asset_morph", "SignalGraph::add_spectral_asset_morph_v1", "morph",
         "audio"}};
    for (const auto& e : expected) {
        INFO(e.key);
        bool found = false;
        for (const auto row : json["types"]) {
            if (row["type_id"].getString() != e.key)
                continue;
            REQUIRE_FALSE(found);
            found = true;
            const bool asset = std::string_view(e.key) == "pulp/spectral_asset_morph";
            REQUIRE(row["notation_state"].getString() ==
                    std::string(asset ? "unsupported_typed_api_only" : "flow_2_arguments"));
            if (!asset) {
                const bool stage = std::string_view(e.key) == "pulp/spectral_stage";
                REQUIRE(row["flow_2_arguments"].size() == (stage ? 5u : 4u));
                REQUIRE(row["flow_2_arguments"][3].getString() == "rate");
            }
            REQUIRE(row["constructor"].getString() == e.constructor);
            REQUIRE(row["probe_identity"]["fft_size"].getInt64() == 256);
            REQUIRE(row["probe_identity"]["hop"].getInt64() == 64);
            REQUIRE(row["probe_identity"]["channels"].getInt64() == 1);
            REQUIRE(row["inputs"].size() == 1);
            REQUIRE(row["inputs"][0]["name"].getString() == e.input);
            REQUIRE(row["inputs"][0]["kind"].getString() == e.input_kind);
            REQUIRE(row["outputs"].size() == 1);
            const bool synthesis = std::string_view(e.key) == "pulp/spectral_synthesis";
            REQUIRE(row["outputs"][0]["kind"].getString() ==
                    std::string(synthesis ? "audio" : "spectral"));
        }
        REQUIRE(found);

        // Flow 1 does not construct the listed key; its refusal names the
        // owning constructor instead of registration.
        const auto parsed = parse_graph_patch(std::string("# flow 1\nx : \"") + e.key + "\"\n");
        REQUIRE_FALSE(parsed);
        REQUIRE(parsed.diagnostic.code == Code::UnsupportedContract);
        REQUIRE(parsed.diagnostic.line == 2);
        REQUIRE(parsed.diagnostic.node == "x");
        REQUIRE(parsed.diagnostic.suggested_fix.find(e.constructor) != std::string::npos);
    }
}

TEST_CASE("Notation admits exactly max_graph_notation_bytes and refuses one more",
          "[host][patch][negative-control]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    STATIC_REQUIRE(max_graph_notation_bytes == 1024u * 1024u);
    auto text = hello();
    REQUIRE(text.size() < max_graph_notation_bytes);
    // Trailing blank lines keep the document valid at any length.
    text.append(max_graph_notation_bytes - text.size(), '\n');
    const auto at_limit = parse_graph_patch(text, c);
    CHECK(at_limit.diagnostic.code == Code::Ok);
    text.push_back('\n');
    const auto over = parse_graph_patch(text, c);
    CHECK(over.diagnostic.code == Code::ResourceLimit);
    CHECK_FALSE(over.graph);
}

namespace {
// Mono analysis -> identity stage -> synthesis between explicit mono
// boundaries, written in canonical order (declarations by name, then edges).
constexpr const char* kFlow2Spectral =
    "# flow 2\n"
    "analysis : pulp/spectral_analysis (fft 256, hop 63, channels 1, rate 48000)\n"
    "in : pulp/audio_in (channels 1)\n"
    "out : pulp/audio_out (channels 1)\n"
    "pass : pulp/spectral_stage (fft 256, hop 63, channels 1, rate 48000, kind identity)\n"
    "synthesis : pulp/spectral_synthesis (fft 256, hop 63, channels 1, rate 48000)\n"
    "analysis.frames >> pass.frames_in\n"
    "in.audio >> analysis.audio\n"
    "pass.frames_out >> synthesis.frames\n"
    "synthesis.audio >> out.audio\n";

const GraphNode* named(const SignalGraph& graph, const std::string& name) {
    for (const auto& n : graph.nodes())
        if (n.name == name)
            return graph.node(n.id);
    return nullptr;
}
GraphPatchDiagnostic refused(const std::string& text) {
    const auto result = parse_graph_patch(text);
    INFO(text);
    INFO(result.diagnostic.message());
    REQUIRE_FALSE(result);
    REQUIRE_FALSE(result.graph);
    return result.diagnostic;
}
} // namespace

TEST_CASE("Flow 2 spells spectral identities and round-trips them exactly",
          "[host][patch][spectral-notation][flow-2]") {
    const auto parsed = parse_graph_patch(kFlow2Spectral);
    INFO(parsed.diagnostic.message());
    REQUIRE(parsed);
    const auto& graph = *parsed.graph;
    REQUIRE(graph.nodes().size() == 5);
    REQUIRE(graph.connections().size() == 4);
    for (const char* name : {"analysis", "pass", "synthesis"}) {
        INFO(name);
        const auto* node = named(graph, name);
        REQUIRE(node);
        REQUIRE(node->spectral_identity);
        REQUIRE(node->spectral_identity->fft_size() == 256);
        REQUIRE(node->spectral_identity->hop() == 63);
        REQUIRE(node->spectral_identity->channels() == 1);
        REQUIRE(node->spectral_identity->sample_rate_bits() ==
                std::bit_cast<std::uint64_t>(48000.0));
    }
    REQUIRE(named(graph, "pass")->type == NodeType::SpectralStage);
    REQUIRE(named(graph, "pass")->spectral_stage->kind() ==
            QualifiedSpectralStage::Kind::IdentityV1);
    REQUIRE(named(graph, "in")->num_output_ports == 1);
    REQUIRE(named(graph, "out")->num_input_ports == 1);
    std::size_t frame_edges = 0;
    for (const auto& c : graph.connections())
        frame_edges += c.spectral ? 1 : 0;
    REQUIRE(frame_edges == 2);

    // parse -> format -> parse: the canonical text is the input and a second
    // pass reproduces it, so no identity bit or edge is lost.
    const auto formatted = format_graph_patch(graph);
    INFO(formatted.diagnostic.message());
    REQUIRE(formatted);
    REQUIRE(formatted.text == kFlow2Spectral);
    const auto again = parse_graph_patch(formatted.text);
    REQUIRE(again);
    REQUIRE(format_graph_patch(*again.graph).text == formatted.text);

    // A non-integral rate keeps its exact bits through the shortest spelling.
    const std::string fractional =
        replaced(replaced(replaced(kFlow2Spectral, "rate 48000", "rate 44100.000000000007"),
                          "rate 48000", "rate 44100.000000000007"),
                 "rate 48000", "rate 44100.000000000007");
    const auto odd = parse_graph_patch(fractional);
    INFO(odd.diagnostic.message());
    REQUIRE(odd);
    const auto odd_text = format_graph_patch(*odd.graph);
    REQUIRE(odd_text);
    const auto odd_again = parse_graph_patch(odd_text.text);
    REQUIRE(odd_again);
    REQUIRE(named(*odd_again.graph, "analysis")->spectral_identity->sample_rate_bits() ==
            std::bit_cast<std::uint64_t>(44100.000000000007));

    // Argument order and spacing are free; the formatter canonicalizes them.
    const auto reordered =
        parse_graph_patch(replaced(kFlow2Spectral, "(fft 256, hop 63, channels 1, rate 48000)\nin",
                                   "(  rate 4.8e4 ,channels 1, hop 63,fft 256 )\nin"));
    INFO(reordered.diagnostic.message());
    REQUIRE(reordered);
    REQUIRE(format_graph_patch(*reordered.graph).text == kFlow2Spectral);
}

TEST_CASE("Flow 2 spectral notation renders the identity oracle and prepares only at its rate",
          "[host][patch][spectral-notation][flow-2][numeric]") {
    {
        auto mismatched = parse_graph_patch(kFlow2Spectral);
        REQUIRE(mismatched);
        REQUIRE_FALSE(mismatched.graph->prepare(44100, 64));
        REQUIRE(mismatched.graph->spectral_error() == SpectralError::IdentityMismatch);
    }
    auto parsed = parse_graph_patch(kFlow2Spectral);
    REQUIRE(parsed);
    auto& graph = *parsed.graph;
    REQUIRE(graph.prepare(48000, 4096));
    const int latency = graph.latency_samples();
    REQUIRE(latency > 0);
    constexpr int total = 4096;
    std::vector<float> input(total), output(total);
    for (int t = 0; t < 3000; ++t)
        input[t] = static_cast<float>(0.25 * std::sin(t * 0.0731));
    input[0] = 1;
    for (int pos = 0; pos < total;) {
        const int count = std::min(total - pos, 1 + pos * 13 % 512);
        const float* ip[] = {input.data() + pos};
        float* op[] = {output.data() + pos};
        pulp::audio::BufferView<const float> iv(ip, 1, count);
        pulp::audio::BufferView<float> ov(op, 1, count);
        graph.process(ov, iv, count);
        pos += count;
    }
    // An identity stage reconstructs the input delayed by the reported latency.
    double error = 0, energy = 0;
    for (int t = 0; t + latency < total; ++t) {
        const double delta = output[t + latency] - input[t];
        error += delta * delta;
        energy += double(input[t]) * input[t];
    }
    REQUIRE(energy > 1);
    REQUIRE(error / energy < 1e-10);
    for (int t = 0; t < latency; ++t)
        REQUIRE(output[t] == 0);
}

TEST_CASE("Flow 1 documents keep their exact text and refuse flow 2 arguments",
          "[host][patch][spectral-notation][flow-1-compat]") {
    GraphPatchCatalog c;
    register_patch_gain(c);
    const auto flow1 = parse_graph_patch(hello(), c);
    REQUIRE(flow1);
    const auto canonical = format_graph_patch(*flow1.graph, c);
    REQUIRE(canonical);
    REQUIRE(canonical.text == "# flow 1\n"
                              "input : pulp/audio_in\n"
                              "midi : pulp/midi_in\n"
                              "out : pulp/audio_out\n"
                              "voice : \"com.example.PatchGain\"\n"
                              "input.audio >> voice.audio\n"
                              "input.audio ~> voice.level @ 0.5\n"
                              "midi.audio >> voice.midi\n"
                              "voice.audio >> out.audio\n"
                              "voice.level = 0.75\n");
    // The same declarations under # flow 2 build the same graph, which still
    // formats as flow 1 because it needs no constructor argument.
    const auto promoted = parse_graph_patch(replaced(hello(), "# flow 1", "# flow 2"), c);
    INFO(promoted.diagnostic.message());
    REQUIRE(promoted);
    REQUIRE(format_graph_patch(*promoted.graph, c).text == canonical.text);
    const auto stereo =
        parse_graph_patch(replaced(replaced(hello(), "# flow 1", "# flow 2"),
                                   "input : pulp/audio_in", "input : pulp/audio_in (channels 2)"),
                          c);
    REQUIRE(stereo);
    REQUIRE(format_graph_patch(*stereo.graph, c).text == canonical.text);

    // Flow 1 reads a parenthesized list as part of the type key, as before.
    const auto args = refused("# flow 1\nin : pulp/audio_in (channels 1)\n");
    REQUIRE(args.code == Code::SyntaxError);
    REQUIRE(args.rule == "invalid type key");
    const auto spectral =
        refused("# flow 1\nx : pulp/spectral_analysis (fft 256, hop 64, channels 1, rate 48000)\n");
    REQUIRE(spectral.code == Code::SyntaxError);
    REQUIRE(refused("# flow 3\n").code == Code::SyntaxError);
}

TEST_CASE("Flow 2 refuses a missing rate, mismatched rates and unsupported layouts",
          "[host][patch][spectral-notation][flow-2][negative-control]") {
    const std::string head = "# flow 2\n";
    const std::string analysis =
        "a : pulp/spectral_analysis (fft 256, hop 64, channels 1, rate 48000)\n";
    // Control: the complete declaration is admitted.
    REQUIRE(parse_graph_patch(head + analysis));

    SECTION("missing sample rate") {
        const auto d = refused(head + "a : pulp/spectral_analysis (fft 256, hop 64, channels 1)\n");
        REQUIRE(d.code == Code::SyntaxError);
        REQUIRE(d.line == 2);
        REQUIRE(d.node == "a");
        REQUIRE(d.rule.find("sample rate") != std::string::npos);
        const auto bare = refused(head + "a : pulp/spectral_synthesis\n");
        REQUIRE(bare.code == Code::SyntaxError);
        REQUIRE(bare.suggested_fix.find("rate R") != std::string::npos);
        for (const char* bad : {"rate 0", "rate -48000", "rate inf", "rate nan", "rate 48kHz"}) {
            INFO(bad);
            const auto r = refused(head +
                                   "a : pulp/spectral_analysis (fft 256, hop 64, channels "
                                   "1, " +
                                   bad + ")\n");
            REQUIRE((r.code == Code::ParameterOutOfRange || r.code == Code::SyntaxError));
        }
    }
    SECTION("mismatched sample rate") {
        const auto d = refused(head + analysis +
                               "s : pulp/spectral_synthesis (fft 256, hop 64, channels 1, rate "
                               "44100)\n");
        REQUIRE(d.code == Code::SampleRateMismatch);
        REQUIRE(d.line == 3);
        REQUIRE(d.node == "s");
        REQUIRE(d.rule.find("44100") != std::string::npos);
        REQUIRE(d.rule.find("48000") != std::string::npos);
        REQUIRE(graph_patch_code_name(d.code) == std::string("SampleRateMismatch"));
        // Control: the same pair at one rate is admitted.
        REQUIRE(parse_graph_patch(
            head + analysis +
            "s : pulp/spectral_synthesis (fft 256, hop 64, channels 1, rate 48000)\n"));
        // The formatter refuses a typed graph whose nodes disagree.
        SignalGraph typed;
        REQUIRE(typed.add_spectral_analysis(*SpectralIdentity::make(256, 64, 1, 48000).value, "a"));
        REQUIRE(
            typed.add_spectral_synthesis(*SpectralIdentity::make(256, 64, 1, 44100).value, "s"));
        REQUIRE(format_graph_patch(typed).diagnostic.code == Code::SampleRateMismatch);
    }
    SECTION("unsupported channel layout") {
        for (const char* text :
             {"in : pulp/audio_in (channels 0)\n", "out : pulp/audio_out (channels 17)\n",
              "a : pulp/spectral_analysis (fft 256, hop 64, channels 17, rate "
              "48000)\n"}) {
            const auto d = refused(head + text);
            REQUIRE(d.code == Code::UnsupportedChannelLayout);
            REQUIRE(graph_patch_code_name(d.code) == std::string("UnsupportedChannelLayout"));
        }
        // Control: the domain's edges are admitted.
        REQUIRE(parse_graph_patch(head + "in : pulp/audio_in (channels 1)\n"
                                         "out : pulp/audio_out (channels 16)\n"));
    }
    SECTION("argument grammar") {
        REQUIRE(refused(head + "g : pulp/gain (channels 1)\n").code == Code::SyntaxError);
        REQUIRE(refused(head + "in : pulp/audio_in (channels 1, channels 2)\n").code ==
                Code::Duplicate);
        REQUIRE(refused(head + "in : pulp/audio_in (lanes 1)\n").code == Code::SyntaxError);
        REQUIRE(refused(head + "in : pulp/audio_in ()\n").code == Code::SyntaxError);
        REQUIRE(refused(head + "in : pulp/audio_in ((channels 1))\n").code == Code::SyntaxError);
        REQUIRE(refused(head + "a : pulp/spectral_analysis (fft 300, hop 64, channels 1, rate "
                               "48000)\n")
                    .code == Code::ParameterOutOfRange);
        REQUIRE(refused(head + "p : pulp/spectral_stage (fft 256, hop 64, channels 1, rate "
                               "48000, kind other)\n")
                    .code == Code::SyntaxError);
        REQUIRE(refused(head + "a : pulp/spectral_analysis @ 2 (fft 256, hop 64, channels 1, "
                               "rate 48000)\n")
                    .code == Code::UnsupportedContract);
    }
}

TEST_CASE("Flow 2 keeps masks and asset payloads in JSON with precise refusals",
          "[host][patch][spectral-notation][flow-2][negative-control]") {
    const auto identity = *SpectralIdentity::make(256, 64, 2, 48000).value;
    auto build = [&](bool mask) {
        auto graph = std::make_unique<SignalGraph>();
        const auto analysis = *graph->add_spectral_analysis(identity, "analysis").value;
        std::vector<float> gains(std::size_t(2) * identity.bins(), 0.5f);
        auto stage = mask ? QualifiedSpectralStage::real_mask_v1(identity, gains)
                          : QualifiedSpectralStage::identity_v1(identity);
        REQUIRE(stage);
        const auto added =
            *graph->add_spectral_stage(identity, std::move(*stage.value), "stage").value;
        REQUIRE(graph->connect(analysis.frames, added.frames_in));
        return graph;
    };
    // Control: the identity stage of the same graph is spelled.
    const auto identity_text = format_graph_patch(*build(false));
    INFO(identity_text.diagnostic.message());
    REQUIRE(identity_text);
    REQUIRE(identity_text.text.find("kind identity") != std::string::npos);
    REQUIRE(identity_text.text.find("channels 2") != std::string::npos);

    const auto masked = format_graph_patch(*build(true));
    REQUIRE_FALSE(masked);
    REQUIRE(masked.text.empty());
    REQUIRE(masked.diagnostic.code == Code::UnsupportedContract);
    REQUIRE(masked.diagnostic.node == "stage");
    REQUIRE(masked.diagnostic.rule.find("258 gains (2 channels x 129 bins)") != std::string::npos);
    REQUIRE(masked.diagnostic.suggested_fix.find("JSON") != std::string::npos);
    const auto parsed_mask = refused(
        "# flow 2\ns : pulp/spectral_stage (fft 256, hop 64, channels 1, rate 48000, kind mask)\n");
    REQUIRE(parsed_mask.code == Code::UnsupportedContract);
    REQUIRE(parsed_mask.suggested_fix.find("JSON") != std::string::npos);

    // Codec asset payloads: refused by the parser and the formatter.
    const auto asset = refused("# flow 2\nm : pulp/spectral_asset_morph\n");
    REQUIRE(asset.code == Code::UnsupportedContract);
    REQUIRE(asset.rule.find("asset") != std::string::npos);
    auto stream = pulp::signal::make_spectral_stream({}, 4);
    REQUIRE(stream);
    auto encoded = pulp::signal::encode_spectral_stream(*stream.value);
    REQUIRE(encoded);
    const auto bytes = encoded.value->view();
    SignalGraph morph;
    REQUIRE(morph.add_spectral_asset_morph_v1(bytes, bytes, 1, "morph"));
    const auto morph_text = format_graph_patch(morph);
    REQUIRE(morph_text.diagnostic.code == Code::UnsupportedContract);
    REQUIRE(morph_text.diagnostic.rule.find("asset payloads") != std::string::npos);
}
