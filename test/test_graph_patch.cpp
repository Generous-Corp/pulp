#include "../examples/graph-patch-consumer/hello.hpp"
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <cmath>
#include <fstream>
#include <iterator>
#include <pulp/host/graph_patch.hpp>
#include <pulp/host/offline_signal_graph_host.hpp>
#include <pulp/host/spectral_graph.hpp>

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
    SignalGraph boundary;
    boundary.add_input_node(1, "in");
    REQUIRE(format_graph_patch(boundary).diagnostic.code == Code::UnsupportedContract);
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

TEST_CASE("Spectral nodes and frame edges are explicit notation refusals",
          "[host][patch][strict][spectral-notation]") {
    const auto identity = *SpectralIdentity::make(256, 64, 1, 48000).value;
    SignalGraph spectral;
    const auto analysis = *spectral.add_spectral_analysis(identity, "analysis").value;
    const auto synthesis = *spectral.add_spectral_synthesis(identity, "synthesis").value;
    REQUIRE(spectral.connect(analysis.frames, synthesis.frames));
    const auto formatted = format_graph_patch(spectral);
    REQUIRE_FALSE(formatted);
    REQUIRE(formatted.diagnostic.code == Code::UnsupportedContract);
    REQUIRE(formatted.diagnostic.rule.find("spectral") != std::string::npos);

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
