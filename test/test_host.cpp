#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>

using namespace pulp::host;

// ── Scanner tests ───────────────────────────────────────────────────────

TEST_CASE("PluginScanner default paths", "[host][scanner]") {
    SECTION("VST3 paths are non-empty on all platforms") {
        auto paths = PluginScanner::default_paths(PluginFormat::VST3);
#ifdef __APPLE__
        REQUIRE(paths.size() >= 2); // user + system
#elif defined(_WIN32)
        REQUIRE(paths.size() >= 1);
#elif defined(__linux__)
        REQUIRE(paths.size() >= 2);
#endif
    }

    SECTION("CLAP paths") {
        auto paths = PluginScanner::default_paths(PluginFormat::CLAP);
        REQUIRE_FALSE(paths.empty());
    }
}

TEST_CASE("PluginScanner bundle detection", "[host][scanner]") {
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.vst3", PluginFormat::VST3));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.clap", PluginFormat::CLAP));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.component", PluginFormat::AudioUnit));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.lv2", PluginFormat::LV2));
    REQUIRE_FALSE(PluginScanner::is_plugin_bundle("MyPlugin.dll", PluginFormat::VST3));
}

TEST_CASE("PluginScanner scan runs without crash", "[host][scanner]") {
    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_lv2 = false; // LV2 not typical in test environments
    auto plugins = scanner.scan(opts);
    // May find real plugins on the system — just verify no crash
    REQUIRE(plugins.size() >= 0);
}

// ── PluginSlot tests ────────────────────────────────────────────────────

TEST_CASE("PluginSlot load returns nullptr for stub", "[host][slot]") {
    PluginInfo info;
    info.name = "TestPlugin";
    info.path = "/nonexistent/path.vst3";
    info.format = PluginFormat::VST3;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot == nullptr); // Stub always returns nullptr
}

// ── SignalGraph tests ───────────────────────────────────────────────────

TEST_CASE("SignalGraph add and remove nodes", "[host][graph]") {
    SignalGraph graph;

    auto input = graph.add_input_node(2, "Input");
    auto output = graph.add_output_node(2, "Output");

    REQUIRE(graph.nodes().size() == 2);
    REQUIRE(graph.node(input) != nullptr);
    REQUIRE(graph.node(input)->name == "Input");
    REQUIRE(graph.node(output)->type == NodeType::AudioOutput);

    REQUIRE(graph.remove_node(input));
    REQUIRE(graph.nodes().size() == 1);
    REQUIRE(graph.node(input) == nullptr);
}

TEST_CASE("SignalGraph connections", "[host][graph]") {
    SignalGraph graph;
    auto a = graph.add_input_node(2);
    auto b = graph.add_gain_node("Gain");
    auto c = graph.add_output_node(2);

    REQUIRE(graph.connect(a, 0, b, 0));
    REQUIRE(graph.connect(b, 0, c, 0));
    REQUIRE(graph.connections().size() == 2);

    // Duplicate connection should fail
    REQUIRE_FALSE(graph.connect(a, 0, b, 0));

    // Disconnect
    REQUIRE(graph.disconnect(a, 0, b, 0));
    REQUIRE(graph.connections().size() == 1);
}

TEST_CASE("SignalGraph cycle detection", "[host][graph]") {
    SignalGraph graph;
    auto a = graph.add_input_node(2);
    auto b = graph.add_gain_node();
    auto c = graph.add_gain_node();

    graph.connect(a, 0, b, 0);
    graph.connect(b, 0, c, 0);

    // c→a would create a cycle
    REQUIRE(graph.would_create_cycle(c, a));
    REQUIRE_FALSE(graph.connect(c, 0, a, 0));

    // a→c is fine (already implied by existing path, but direct is ok)
    REQUIRE_FALSE(graph.would_create_cycle(a, c));
}

TEST_CASE("SignalGraph topological sort", "[host][graph]") {
    SignalGraph graph;
    auto a = graph.add_input_node(2, "A");
    auto b = graph.add_gain_node("B");
    auto c = graph.add_output_node(2, "C");

    graph.connect(a, 0, b, 0);
    graph.connect(b, 0, c, 0);

    auto order = graph.processing_order();
    REQUIRE(order.size() == 3);
    // A must come before B, B before C
    auto pos_a = std::find(order.begin(), order.end(), a) - order.begin();
    auto pos_b = std::find(order.begin(), order.end(), b) - order.begin();
    auto pos_c = std::find(order.begin(), order.end(), c) - order.begin();
    REQUIRE(pos_a < pos_b);
    REQUIRE(pos_b < pos_c);
}

TEST_CASE("SignalGraph clear", "[host][graph]") {
    SignalGraph graph;
    graph.add_input_node(2);
    graph.add_output_node(2);
    graph.clear();
    REQUIRE(graph.nodes().empty());
    REQUIRE(graph.connections().empty());
}

TEST_CASE("SignalGraph MIDI nodes", "[host][graph]") {
    SignalGraph graph;
    auto midi_in = graph.add_midi_input_node("Keys");
    auto midi_out = graph.add_midi_output_node("Out");

    REQUIRE(graph.node(midi_in)->type == NodeType::MidiInput);
    REQUIRE(graph.node(midi_out)->type == NodeType::MidiOutput);
    REQUIRE(graph.connect(midi_in, 0, midi_out, 0));
}

TEST_CASE("SignalGraph routes input → gain → output", "[host][graph][routing]") {
    // Validates the Phase 2 graph execution path: per-node scratch buffers,
    // inbound connection summing, gain application, and output accumulation.
    SignalGraph graph;
    auto in  = graph.add_input_node(2, "in");
    auto gain = graph.add_gain_node("gain");
    auto out = graph.add_output_node(2, "out");

    REQUIRE(graph.connect(in,   0, gain, 0));
    REQUIRE(graph.connect(in,   1, gain, 1));
    REQUIRE(graph.connect(gain, 0, out,  0));
    REQUIRE(graph.connect(gain, 1, out,  1));

    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(graph.set_node_gain(gain, 0.5f));

    std::vector<float> in_l(64, 0.8f), in_r(64, 0.4f);
    std::vector<float> out_l(64, 0.0f), out_r(64, 0.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 64);
    pulp::audio::BufferView<float>       out_view(out_ptrs, 2, 64);

    graph.process(out_view, in_view, 64);

    // Expected: output = input * 0.5 across both channels, every sample.
    for (int i = 0; i < 64; ++i) {
        REQUIRE(std::abs(out_l[i] - 0.4f) < 1e-6f);
        REQUIRE(std::abs(out_r[i] - 0.2f) < 1e-6f);
    }
    graph.release();
}

TEST_CASE("SignalGraph disconnected output stays silent", "[host][graph][routing]") {
    // If no node connects to the AudioOutput, process() must leave the
    // output silent regardless of input content.
    SignalGraph graph;
    graph.add_input_node(2, "in");
    graph.add_output_node(2, "out");
    REQUIRE(graph.prepare(48000.0, 32));

    std::vector<float> in_l(32, 1.0f), in_r(32, 1.0f);
    std::vector<float> out_l(32, 99.0f), out_r(32, 99.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 2, 32);
    pulp::audio::BufferView<float>       out_view(out_ptrs, 2, 32);

    graph.process(out_view, in_view, 32);

    for (int i = 0; i < 32; ++i) {
        REQUIRE(out_l[i] == 0.0f);
        REQUIRE(out_r[i] == 0.0f);
    }
    graph.release();
}

// ── Real CLAP loader (integration test, skipped when no test plugin built) ──

#include <filesystem>
#include <vector>

#ifdef PULP_TEST_CLAP_PATH
TEST_CASE("PluginSlot loads and processes a real CLAP plugin", "[host][clap][integration]") {
    namespace fs = std::filesystem;
    const std::string path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(path)) {
        WARN("CLAP test plugin not built at " << path << " — skipping");
        return;
    }

    PluginInfo info;
    info.name   = "PulpGain";
    info.path   = path;
    info.format = PluginFormat::CLAP;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());
    REQUIRE(slot->prepare(48000.0, 256));

    std::vector<float> in_l(256, 0.5f), in_r(256, 0.5f);
    std::vector<float> out_l(256, 0.0f), out_r(256, 0.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};

    pulp::audio::BufferView<const float> in(in_ptrs, 2, 256);
    pulp::audio::BufferView<float>       out(out_ptrs, 2, 256);
    pulp::midi::MidiBuffer mi, mo;

    slot->process(out, in, mi, mo, 256);

    // PulpGain is a unity-ish gain plugin; output should not be all zeros
    // after processing a non-zero input.
    float sum = 0.0f;
    for (int i = 0; i < 256; ++i) sum += std::abs(out_l[i]) + std::abs(out_r[i]);
    REQUIRE(sum > 0.0f);

    slot->release();
}
#endif
