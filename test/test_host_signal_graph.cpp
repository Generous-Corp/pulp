// SignalGraph tests live separately from test_host.cpp so the host test surface
// stays reviewable. Self-contained: uses
// SignalGraph from pulp/host/signal_graph.hpp (in the shared host includes) and
// carries its own interleaved helper namespaces.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "harness/rt_allocation_probe.hpp"
#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/ump_buffer.hpp>
#if defined(__unix__) || defined(__APPLE__)
#include "native_components/rt_test_scope.hpp"
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;

TEST_CASE("SignalGraph accepts the audio workgroup capability handoff",
          "[host][signal-graph][workgroup]") {
    SignalGraph graph;
    auto* client = dynamic_cast<pulp::format::AudioWorkgroupClient*>(&graph);
    REQUIRE(client != nullptr);

    auto* handle = reinterpret_cast<void*>(std::uintptr_t{0x6060});
    client->set_audio_workgroup(handle);
    REQUIRE(graph.configured_audio_workgroup() == handle);
    client->set_audio_workgroup(nullptr);
    REQUIRE(graph.configured_audio_workgroup() == nullptr);
}

namespace {
PluginInfo make_plugin_info(std::string name,
                            int num_inputs = 0,
                            int num_outputs = 0,
                            std::string category = "Fx") {
    PluginInfo info{};
    info.name = std::move(name);
    info.format = PluginFormat::CLAP;
    info.num_inputs = num_inputs;
    info.num_outputs = num_outputs;
    info.category = std::move(category);
    return info;
}
} // namespace

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

TEST_CASE("SignalGraph registers and processes custom nodes",
          "[host][graph][node-abi]") {
    SignalGraph graph;
    CustomNodeType empty_type;
    empty_type.version = 1;
    empty_type.num_input_ports = 1;
    empty_type.num_output_ports = 1;
    empty_type.default_name = "Bad";
    REQUIRE_FALSE(graph.register_custom_node_type(empty_type));

    CustomNodeType zero_version_type;
    zero_version_type.type_id = "pulp.test.bad";
    zero_version_type.version = 0;
    zero_version_type.num_input_ports = 1;
    zero_version_type.num_output_ports = 1;
    zero_version_type.default_name = "Bad";
    REQUIRE_FALSE(graph.register_custom_node_type(zero_version_type));
    REQUIRE_FALSE(graph.add_custom_node("pulp.test.custom"));

    CustomNodeType custom_type;
    custom_type.type_id = "pulp.test.custom";
    custom_type.version = 3;
    custom_type.num_input_ports = 1;
    custom_type.num_output_ports = 1;
    custom_type.default_name = "Custom";
    custom_type.process = [](pulp::audio::BufferView<float>& output,
                             const pulp::audio::BufferView<const float>& input,
                             int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i] * 2.0f;
        }
    };
    REQUIRE(graph.register_custom_node_type(std::move(custom_type)));
    auto input = graph.add_input_node(1, "Input");
    auto custom = graph.add_custom_node("pulp.test.custom", "Custom A");
    auto output = graph.add_output_node(1, "Output");
    REQUIRE(custom != 0);

    const auto* node = graph.node(custom);
    REQUIRE(node != nullptr);
    REQUIRE(node->type == NodeType::Custom);
    REQUIRE(node->custom_type_id == "pulp.test.custom");
    REQUIRE(node->custom_type_version == 3);
    REQUIRE(node->num_input_ports == 1);
    REQUIRE(node->num_output_ports == 1);

    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));

    float in_samples[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_samples[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float* in_ptrs[1] = {in_samples};
    float* out_ptrs[1] = {out_samples};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);
    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_samples[i] == in_samples[i] * 2.0f);
}

namespace {
// A stateful custom node for lifecycle tests: its opaque instance
// holds a `level` multiplier; process_instance scales the input by it;
// save/load_state serialize the level as 4 little-endian bytes.
struct StatefulLevel {
    float level = 1.0f;
};
pulp::host::CustomNodeType make_level_type() {
    pulp::host::CustomNodeType t;
    t.type_id = "pulp.test.level";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Level";
    t.create = []() -> void* { return new StatefulLevel(); };
    t.destroy = [](void* p) { delete static_cast<StatefulLevel*>(p); };
    t.reset = [](void* p) { static_cast<StatefulLevel*>(p)->level = 1.0f; };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                            const pulp::audio::BufferView<const float>& in,
                            int n) {
        const float lvl = static_cast<StatefulLevel*>(p)->level;
        for (int i = 0; i < n; ++i) out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * lvl;
    };
    t.save_state = [](void* p) {
        const float lvl = static_cast<StatefulLevel*>(p)->level;
        std::vector<uint8_t> b(sizeof(float));
        std::memcpy(b.data(), &lvl, sizeof(float));
        return b;
    };
    t.load_state = [](void* p, const std::vector<uint8_t>& b) {
        if (b.size() != sizeof(float)) return false;
        std::memcpy(&static_cast<StatefulLevel*>(p)->level, b.data(), sizeof(float));
        return true;
    };
    return t;
}
std::vector<uint8_t> level_bytes(float lvl) {
    std::vector<uint8_t> b(sizeof(float));
    std::memcpy(b.data(), &lvl, sizeof(float));
    return b;
}
}  // namespace

TEST_CASE("SignalGraph stateful custom node: lifecycle + state round-trip",
          "[host][graph][node-abi]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_level_type()));

    auto input = graph.add_input_node(1, "Input");
    auto node = graph.add_custom_node("pulp.test.level", "Level A");
    auto output = graph.add_output_node(1, "Output");
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output, 0));

    // Load opaque state (level = 3.0) before prepare; applied on prepare().
    REQUIRE(graph.set_custom_node_state(node, level_bytes(3.0f)));
    REQUIRE(graph.prepare(48000.0, 4));

    float in_s[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_s[4] = {-1, -1, -1, -1};
    const float* in_p[1] = {in_s};
    float* out_p[1] = {out_s};
    pulp::audio::BufferView<const float> in_view(in_p, 1, 4);
    pulp::audio::BufferView<float> out_view(out_p, 1, 4);
    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_s[i] == in_s[i] * 3.0f);

    // The live instance's state reflects the loaded level.
    REQUIRE(graph.custom_node_state(node) == level_bytes(3.0f));

    // A node with no instance (not a custom node) yields empty state.
    REQUIRE(graph.custom_node_state(input).empty());
    REQUIRE_FALSE(graph.set_custom_node_state(input, level_bytes(1.0f)));

    // release() drives the stateful instance's release callback; the instance
    // (and its state) survive for a subsequent re-prepare.
    graph.release();
    REQUIRE(graph.custom_node_state(node) == level_bytes(3.0f));
    REQUIRE(graph.prepare(48000.0, 4));
    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_s[i] == in_s[i] * 3.0f);
}

TEST_CASE("SignalGraph generated custom state changes require re-prepare",
          "[host][graph][generated][rt-safety]") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_level_type()));

    auto input = graph.add_input_node(1, "Input");
    auto node = graph.add_custom_node("pulp.test.level", "Level A");
    auto output = graph.add_output_node(1, "Output");
    REQUIRE(node != 0);
    REQUIRE(graph.connect(input, 0, node, 0));
    REQUIRE(graph.connect(node, 0, output, 0));
    REQUIRE(graph.set_custom_node_state(node, level_bytes(3.0f)));
    REQUIRE(graph.prepare(48000.0, 4));

    float in_s[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_s[4] = {-1, -1, -1, -1};
    const float* in_p[1] = {in_s};
    float* out_p[1] = {out_s};
    pulp::audio::BufferView<const float> in_view(in_p, 1, 4);
    pulp::audio::BufferView<float> out_view(out_p, 1, 4);

    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_s[i] == in_s[i] * 3.0f);

    REQUIRE(graph.set_custom_node_state(node, level_bytes(2.0f)));
    std::fill(std::begin(out_s), std::end(out_s), -1.0f);
    graph.process(out_view, in_view, 4);
    for (float sample : out_s) REQUIRE(sample == 0.0f);

    REQUIRE(graph.prepare(48000.0, 4));
    std::fill(std::begin(out_s), std::end(out_s), -1.0f);
    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_s[i] == in_s[i] * 2.0f);
}

TEST_CASE("SignalGraph custom node registry keeps versions distinct",
          "[host][graph][node-abi]") {
    SignalGraph graph;

    CustomNodeType v1_type;
    v1_type.type_id = "pulp.test.versioned";
    v1_type.version = 1;
    v1_type.num_input_ports = 1;
    v1_type.num_output_ports = 1;
    v1_type.default_name = "Version 1";
    REQUIRE(graph.register_custom_node_type(v1_type));

    CustomNodeType v2_type;
    v2_type.type_id = "pulp.test.versioned";
    v2_type.version = 2;
    v2_type.num_input_ports = 2;
    v2_type.num_output_ports = 2;
    v2_type.default_name = "Version 2";
    REQUIRE(graph.register_custom_node_type(v2_type));

    const auto* v1 = graph.custom_node_type("pulp.test.versioned", 1);
    const auto* v2 = graph.custom_node_type("pulp.test.versioned", 2);
    const auto* latest = graph.custom_node_type("pulp.test.versioned");
    REQUIRE(v1 != nullptr);
    REQUIRE(v2 != nullptr);
    REQUIRE(latest != nullptr);
    REQUIRE(v1->version == 1);
    REQUIRE(v2->version == 2);
    REQUIRE(latest->version == 2);

    auto node_v1 = graph.add_custom_node("pulp.test.versioned", 1, "Old Shape");
    auto node_v2 = graph.add_custom_node("pulp.test.versioned", 2, "New Shape");
    REQUIRE(node_v1 != 0);
    REQUIRE(node_v2 != 0);
    REQUIRE(graph.node(node_v1)->custom_type_version == 1);
    REQUIRE(graph.node(node_v1)->num_input_ports == 1);
    REQUIRE(graph.node(node_v2)->custom_type_version == 2);
    REQUIRE(graph.node(node_v2)->num_input_ports == 2);
}

TEST_CASE("SignalGraph custom node processors require matching shapes",
          "[host][graph][node-abi]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "Input");
    auto custom = graph.add_unresolved_custom_node(
        "pulp.test.shape-guard", 1, 1, 1, "Shape Guard");
    auto output = graph.add_output_node(1, "Output");
    REQUIRE(custom != 0);
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));

    bool callback_called = false;
    CustomNodeType mismatched_type;
    mismatched_type.type_id = "pulp.test.shape-guard";
    mismatched_type.version = 1;
    mismatched_type.num_input_ports = 2;
    mismatched_type.num_output_ports = 1;
    mismatched_type.default_name = "Shape Guard";
    mismatched_type.process =
        [&callback_called](pulp::audio::BufferView<float>& output,
                           const pulp::audio::BufferView<const float>&,
                           int num_samples) {
            callback_called = true;
            for (int i = 0; i < num_samples; ++i) {
                output.channel_ptr(0)[i] = 99.0f;
            }
        };
    REQUIRE(graph.register_custom_node_type(std::move(mismatched_type)));
    REQUIRE(graph.prepare(48000.0, 4));

    float in_samples[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_samples[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float* in_ptrs[1] = {in_samples};
    float* out_ptrs[1] = {out_samples};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);
    graph.process(out_view, in_view, 4);

    REQUIRE_FALSE(callback_called);
    for (int i = 0; i < 4; ++i) REQUIRE(out_samples[i] == in_samples[i]);
}

TEST_CASE("SignalGraph custom node registrations invalidate matching snapshots",
          "[host][graph][node-abi]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "Input");
    auto custom = graph.add_unresolved_custom_node(
        "pulp.test.live-registration", 1, 1, 1, "Live Registration");
    auto output = graph.add_output_node(1, "Output");
    REQUIRE(custom != 0);
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));

    float in_samples[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_samples[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float* in_ptrs[1] = {in_samples};
    float* out_ptrs[1] = {out_samples};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_samples[i] == in_samples[i]);

    CustomNodeType registered_type;
    registered_type.type_id = "pulp.test.live-registration";
    registered_type.version = 1;
    registered_type.num_input_ports = 1;
    registered_type.num_output_ports = 1;
    registered_type.default_name = "Live Registration";
    registered_type.process = [](pulp::audio::BufferView<float>& output,
                                 const pulp::audio::BufferView<const float>& input,
                                 int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i] * 3.0f;
        }
    };
    REQUIRE(graph.register_custom_node_type(std::move(registered_type)));

    std::fill(out_samples, out_samples + 4, -1.0f);
    graph.process(out_view, in_view, 4);
    for (float sample : out_samples) REQUIRE(sample == 0.0f);

    REQUIRE(graph.prepare(48000.0, 4));
    std::fill(out_samples, out_samples + 4, -1.0f);
    graph.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_samples[i] == in_samples[i] * 3.0f);
}

TEST_CASE("SignalGraph remove_node prunes edges and invalidates live graph",
          "[host][graph]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "Input");
    auto gain = graph.add_gain_node("Gain");
    auto output = graph.add_output_node(1, "Output");

    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.connections().size() == 2);
    REQUIRE(graph.prepare(48000.0, 8));

    std::vector<float> input_samples(8, 0.5f);
    std::vector<float> output_samples(8, -1.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 8);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 8);

    graph.process(out_view, in_view, 8);
    for (float sample : output_samples) REQUIRE(sample == 0.5f);

    REQUIRE(graph.remove_node(gain));
    REQUIRE_FALSE(graph.remove_node(gain));
    REQUIRE(graph.node(gain) == nullptr);
    REQUIRE(graph.connections().empty());

    std::fill(output_samples.begin(), output_samples.end(), -1.0f);
    graph.process(out_view, in_view, 8);
    for (float sample : output_samples) REQUIRE(sample == 0.0f);
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

TEST_CASE("SignalGraph rejects missing-node and duplicate edge variants",
          "[host][graph][issue-493]") {
    SignalGraph graph;
    auto input = graph.add_input_node(2);
    auto gain = graph.add_gain_node("Gain");
    auto output = graph.add_output_node(2);
    auto midi_in = graph.add_midi_input_node("MIDI In");
    auto midi_out = graph.add_midi_output_node("MIDI Out");

    REQUIRE_FALSE(graph.connect(999, 0, gain, 0));
    REQUIRE_FALSE(graph.connect(input, 0, 999, 0));
    REQUIRE_FALSE(graph.disconnect(input, 0, gain, 0));

    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE_FALSE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.disconnect(input, 0, gain, 0));
    REQUIRE_FALSE(graph.disconnect(input, 0, gain, 0));

    REQUIRE_FALSE(graph.connect_feedback(999, 0, output, 0));
    REQUIRE_FALSE(graph.connect_feedback(gain, 0, 999, 0));
    REQUIRE(graph.connect_feedback(gain, 0, output, 0));
    REQUIRE_FALSE(graph.connect_feedback(gain, 0, output, 0));

    REQUIRE_FALSE(graph.connect_midi(999, midi_out));
    REQUIRE_FALSE(graph.connect_midi(midi_in, 999));
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE_FALSE(graph.connect_midi(midi_in, midi_out));
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

TEST_CASE("SignalGraph routes input -> gain -> output", "[host][graph][routing]") {
    // Validates the graph execution path: per-node scratch buffers,
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

TEST_CASE("SignalGraph live gain updates and release silence output",
          "[host][graph][routing][issue-493]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto out = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    std::vector<float> input(16, 0.8f);
    std::vector<float> output(16, -1.0f);
    const float* in_ptrs[1] = {input.data()};
    float* out_ptrs[1] = {output.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 16);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 16);

    graph.process(out_view, in_view, 16);
    for (float sample : output) REQUIRE_THAT(sample, WithinAbs(0.8f, 1e-6f));

    REQUIRE(graph.set_node_gain(gain, 0.25f));
    REQUIRE(graph.node_gain(gain) == 0.25f);
    std::fill(output.begin(), output.end(), -1.0f);
    graph.process(out_view, in_view, 16);
    for (float sample : output) REQUIRE_THAT(sample, WithinAbs(0.2f, 1e-6f));

    graph.release();
    std::fill(output.begin(), output.end(), -1.0f);
    graph.process(out_view, in_view, 16);
    for (float sample : output) REQUIRE(sample == 0.0f);
    REQUIRE(graph.latency_samples() == 0);
    REQUIRE(graph.node_latency_samples(gain) == 0);
}

TEST_CASE("SignalGraph live gain Race is atomic while processing",
          "[host][graph][race][tsan]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto out = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 32));

    std::vector<float> input(32, 1.0f);
    std::vector<float> output(32, 0.0f);
    const float* in_ptrs[1] = {input.data()};
    float* out_ptrs[1] = {output.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 32);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 32);

    std::atomic<bool> stop{false};
    std::atomic<bool> updates_active{false};
    std::atomic<bool> saw_invalid_output{false};
    std::atomic<int> processed_blocks{0};
    std::atomic<int> blocks_during_updates{0};
    std::thread audio_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.process(out_view, in_view, 32);
            processed_blocks.fetch_add(1, std::memory_order_relaxed);
            if (updates_active.load(std::memory_order_acquire)) {
                blocks_during_updates.fetch_add(1, std::memory_order_relaxed);
            }
            for (float sample : output) {
                if (!std::isfinite(sample) || sample < -1.0e-6f
                    || sample > 1.0f + 1.0e-6f) {
                    saw_invalid_output.store(true, std::memory_order_relaxed);
                    break;
                }
            }
        }
    });

    bool all_gain_updates_succeeded = true;
    constexpr int kMinGainUpdates = 10000;
    constexpr int kMinBlocksDuringUpdates = 4;
    constexpr int kMaxGainUpdates = 1000000;
    int update_count = 0;
    updates_active.store(true, std::memory_order_release);
    while ((update_count < kMinGainUpdates
            || blocks_during_updates.load(std::memory_order_relaxed)
                < kMinBlocksDuringUpdates)
           && update_count < kMaxGainUpdates) {
        const int i = update_count++;
        const float g = static_cast<float>(i % 17) / 16.0f;
        all_gain_updates_succeeded =
            graph.set_node_gain(gain, g) && all_gain_updates_succeeded;
        if ((i % 64) == 0) std::this_thread::yield();
    }
    updates_active.store(false, std::memory_order_release);

    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(all_gain_updates_succeeded);
    REQUIRE(processed_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(blocks_during_updates.load(std::memory_order_relaxed)
            >= kMinBlocksDuringUpdates);
    REQUIRE_FALSE(saw_invalid_output.load(std::memory_order_relaxed));
    graph.release();
}

namespace {
class ReleaseOrderingPlugin final : public PluginSlot {
public:
    ReleaseOrderingPlugin(std::atomic<bool>& release_thread_started,
                          std::atomic<bool>& process_entered,
                          std::atomic<bool>& in_process,
                          std::atomic<bool>& release_called,
                          std::atomic<bool>& release_during_process)
        : release_thread_started_(release_thread_started),
          process_entered_(process_entered),
          in_process_(in_process),
          release_called_(release_called),
          release_during_process_(release_during_process),
          info_(make_plugin_info("ReleaseOrdering", 1, 1)) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {
        if (in_process_.load(std::memory_order_acquire)) {
            release_during_process_.store(true, std::memory_order_release);
        }
        release_called_.store(true, std::memory_order_release);
    }
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        in_process_.store(true, std::memory_order_release);
        process_entered_.store(true, std::memory_order_release);
        while (!release_thread_started_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Give the release thread a bounded window to demonstrate whether it
        // (incorrectly) calls release() while process() is still in flight.
        // Bound the window by wall-clock, not a fixed yield count: on a
        // saturated CI host 100000 yields can each context-switch and the loop
        // can outlast the ctest timeout, aborting the test spuriously. ~50ms is
        // ample for the release thread to run graph.release() and expose an
        // ordering bug if one exists (release_called_ breaks the loop early).
        const auto window_end =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
        while (std::chrono::steady_clock::now() < window_end
               && !release_called_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const size_t channels = std::min(out.num_channels(), in.num_channels());
        for (size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            const float* src = c < channels ? in.channel_ptr(c) : nullptr;
            if (src) std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(n));
            else std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
        }
        in_process_.store(false, std::memory_order_release);
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    std::atomic<bool>& release_thread_started_;
    std::atomic<bool>& process_entered_;
    std::atomic<bool>& in_process_;
    std::atomic<bool>& release_called_;
    std::atomic<bool>& release_during_process_;
    PluginInfo info_;
};

class LifetimeTrackedPlugin final : public PluginSlot {
public:
    explicit LifetimeTrackedPlugin(std::atomic<int>& destroyed)
        : destroyed_(destroyed), info_(make_plugin_info("LifetimeTracked", 1, 1)) {}

    ~LifetimeTrackedPlugin() override {
        destroyed_.fetch_add(1, std::memory_order_release);
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const size_t channels = std::min(out.num_channels(), in.num_channels());
        for (size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            const float* src = c < channels ? in.channel_ptr(c) : nullptr;
            if (src) std::memcpy(dst, src, sizeof(float) * static_cast<size_t>(n));
            else std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    std::atomic<int>& destroyed_;
    PluginInfo info_;
};
} // namespace

TEST_CASE("SignalGraph release waits for in-flight snapshot process",
          "[host][graph][race][tsan]") {
    std::atomic<bool> release_thread_started{false};
    std::atomic<bool> process_entered{false};
    std::atomic<bool> in_process{false};
    std::atomic<bool> release_called{false};
    std::atomic<bool> release_during_process{false};

    SignalGraph graph;
    auto input = graph.add_input_node(1, "input");
    auto plugin = graph.add_plugin_node(
        std::make_unique<ReleaseOrderingPlugin>(release_thread_started,
                                                process_entered,
                                                in_process,
                                                release_called,
                                                release_during_process),
        1,
        1,
        "plugin");
    auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 32));

    std::vector<float> input_samples(32, 1.0f);
    std::vector<float> output_samples(32, 0.0f);
    const float* input_ptrs[1] = {input_samples.data()};
    float* output_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> input_view(input_ptrs, 1, 32);
    pulp::audio::BufferView<float> output_view(output_ptrs, 1, 32);

    std::thread audio_thread([&] {
        graph.process(output_view, input_view, 32);
    });
    while (!process_entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }

    SECTION("while snapshot is still live") {}
    SECTION("after topology invalidates the live snapshot") {
        REQUIRE(graph.disconnect(plugin, 0, output, 0));
    }

    std::thread release_thread([&] {
        release_thread_started.store(true, std::memory_order_release);
        graph.release();
    });

    audio_thread.join();
    release_thread.join();

    REQUIRE(release_called.load(std::memory_order_acquire));
    REQUIRE_FALSE(release_during_process.load(std::memory_order_acquire));
}

TEST_CASE("SignalGraph control-thread snapshot readers pin against retirement",
          "[host][graph][race][rt-safety][tsan]") {
    // The control-thread snapshot readers (inject_midi / extract_midi /
    // node_latency_samples / set_node_gain) load live_raw_ and dereference the
    // CompiledGraph (cg->runtime / cg->shapes / per-runtime gain atomic). They
    // run on a different thread than prepare()/release(), so they must pin the
    // snapshot via the same reader-count handshake the audio path uses;
    // otherwise a concurrent prepare()/release()/invalidate can retire+free the
    // snapshot mid-dereference (use-after-free). The reader thread below touches
    // ONLY the frozen snapshot (never the control-owned nodes_/connections_
    // vectors), so the only cross-thread sharing under test is the snapshot
    // lifetime — exactly what ProcessReadGuard protects. The retirer churns
    // snapshots via prepare()/release()/topology edits. Definitive proof is
    // under ThreadSanitizer: without the guards TSan flags a UAF on the retired
    // CompiledGraph; with them it is clean.
    //
    // Note: set_node_gain ALSO writes the control-owned GraphNode::gain scalar
    // and scans nodes_, which is a separate control-thread-vs-prepare ordering
    // concern (a real host serializes structural edits against prepare) and is
    // out of scope for the snapshot-lifetime fix. To keep this test focused on
    // the snapshot UAF, set_node_gain's snapshot store is exercised separately
    // below rather than raced against prepare()'s nodes_ read.
    SignalGraph graph;
    auto in   = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto out  = graph.add_output_node(1, "out");
    auto mi   = graph.add_midi_input_node("keys");
    auto mo   = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect(in, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    REQUIRE(graph.connect_midi(mi, mo));
    REQUIRE(graph.prepare(48000.0, 64));

    // set_node_gain's snapshot half (the per-runtime gain atomic store) is
    // pinned by ProcessReadGuard; cover it directly so all four methods are
    // represented even though it is not raced against prepare() below.
    REQUIRE(graph.set_node_gain(gain, 0.5f));

    pulp::midi::MidiBuffer in_events;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 0;
    in_events.add(note);

    std::atomic<bool> stop{false};
    std::atomic<bool> saw_crash{false};
    std::atomic<long> reader_iterations{0};
    std::atomic<long> retire_iterations{0};

    // Reader thread: continuously dereference the live snapshot through the
    // three pure-snapshot readers. None must crash or read freed memory while
    // the retirer churns snapshots underneath them.
    std::thread reader_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.inject_midi(mi, in_events);
            pulp::midi::MidiBuffer arrived;
            graph.extract_midi(mo, arrived);
            const int latency = graph.node_latency_samples(gain);
            if (latency < 0) saw_crash.store(true, std::memory_order_relaxed);
            reader_iterations.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Retirer thread: force snapshot retirement + free. prepare() retires the
    // previous snapshot; a topology edit invalidates+retires the live one;
    // release() retires and waits. Each prune frees the retired CompiledGraph,
    // which would dangle the reader's loaded pointer absent the guard.
    std::thread retirer_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.prepare(48000.0, 64);
            graph.disconnect(gain, 0, out, 0);
            graph.connect(gain, 0, out, 0);
            graph.release();
            graph.prepare(48000.0, 64);
            retire_iterations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    reader_thread.join();
    retirer_thread.join();

    REQUIRE_FALSE(saw_crash.load(std::memory_order_relaxed));
    REQUIRE(reader_iterations.load(std::memory_order_relaxed) > 0);
    REQUIRE(retire_iterations.load(std::memory_order_relaxed) > 0);
}

TEST_CASE("SignalGraph control-thread node-field edits serialize against prepare",
          "[host][graph][race][rt-safety][tsan]") {
    // Companion to the snapshot-lifetime test above, which deliberately left the
    // control-thread GraphNode-field race out of scope (see its note). The race
    // closed here is between two CONTROL threads — never the audio thread:
    //   * a UI thread mutating control-owned source-of-truth state —
    //     set_node_gain() writes the plain GraphNode::gain scalar and scans nodes_
    //     via node(); add_gain_node()/add_input_node()/add_plugin_node()/
    //     remove_node()/connect()/disconnect() push/erase nodes_/connections_;
    //   * a host thread calling prepare() -> compile_(), which iterates nodes_ /
    //     connections_ and reads GraphNode::gain (signal_graph.cpp, `rt.gain =
    //     ... n.gain`) plus validate_generated_graph()'s nodes_/connections_ scan.
    // These touch the plain nodes_/connections_ vectors + GraphNode plain fields,
    // which the ProcessReadGuard snapshot pin does NOT protect (that pins the
    // compiled snapshot, not the source GraphNode). Without graph_mutation_mutex_
    // this is a data race; with it the accesses are serialized. The audio render
    // path is intentionally absent: it reads only the immutable CompiledGraph
    // snapshot, never nodes_/GraphNode fields, so it takes no lock here.
    //
    // Thread A churns a spread of mutators — set_node_gain (GraphNode::gain),
    // add_gain_node/add_input_node/add_plugin_node + remove_node (nodes_
    // push/erase), and a connect/disconnect toggle (connections_) — so the race
    // coverage is not limited to add_gain_node/remove_node. Proof is under
    // ThreadSanitizer: reverting the locks reports write/read data races on the
    // nodes_/connections_ vector storage and GraphNode::gain; with the locks the
    // run is clean.
    SignalGraph graph;
    auto in   = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto out  = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(in, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 32));

    std::atomic<bool> stop{false};
    std::atomic<bool> all_gain_ok{true};
    std::atomic<int> destroyed{0};  // for the churned plugin nodes
    std::atomic<long> mutate_iterations{0};
    std::atomic<long> prepare_iterations{0};

    // Thread A — control-thread topology + node-field mutator. Each iteration
    // exercises a different previously-unlocked surface so the test catches the
    // broad mutator gap, not just add_gain_node/remove_node:
    //   * set_node_gain  -> GraphNode::gain write + node() scan
    //   * add_input_node / add_gain_node / add_plugin_node(unique_ptr) ->
    //     nodes_.push_back on three distinct add paths
    //   * remove_node    -> nodes_/connections_ erase
    //   * connect/disconnect -> connections_ push/erase + node() scan
    // The scratch nodes are added then removed so the graph size stays bounded
    // while nodes_ is mutated under compile_()'s iteration.
    std::thread mutate_thread([&] {
        long i = 0;
        while (!stop.load(std::memory_order_acquire)) {
            const float g = static_cast<float>(i % 17) / 16.0f;
            if (!graph.set_node_gain(gain, g)) {
                all_gain_ok.store(false, std::memory_order_relaxed);
            }
            // Rotate across the previously-unlocked add_* paths.
            NodeId scratch = 0;
            switch (i % 3) {
            case 0: scratch = graph.add_gain_node("scratch"); break;
            case 1: scratch = graph.add_input_node(1, "scratch-in"); break;
            case 2:
                scratch = graph.add_plugin_node(
                    std::make_unique<LifetimeTrackedPlugin>(destroyed),
                    1, 1, "scratch-plugin");
                break;
            }
            // connections_ churn: briefly wire the scratch node, then tear it down.
            graph.connect(scratch, 0, out, 0);
            graph.disconnect(scratch, 0, out, 0);
            graph.remove_node(scratch);
            mutate_iterations.fetch_add(1, std::memory_order_relaxed);
            ++i;
            if ((i % 64) == 0) std::this_thread::yield();
        }
    });

    // Thread B — host prepare()/compile_() loop. compile_() reads GraphNode::gain
    // and iterates nodes_; this is the read side of the race.
    std::thread prepare_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.prepare(48000.0, 32);
            prepare_iterations.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    mutate_thread.join();
    prepare_thread.join();

    REQUIRE(all_gain_ok.load(std::memory_order_relaxed));
    REQUIRE(mutate_iterations.load(std::memory_order_relaxed) > 0);
    REQUIRE(prepare_iterations.load(std::memory_order_relaxed) > 0);
    graph.release();
}

TEST_CASE("SignalGraph retired snapshots do not own removed plugins",
          "[host][graph][lifetime]") {
    std::atomic<int> destroyed{0};

    {
        SignalGraph graph;
        auto plugin = graph.add_plugin_node(
            std::make_unique<LifetimeTrackedPlugin>(destroyed),
            1,
            1,
            "plugin");
        REQUIRE(plugin != 0);
        REQUIRE(graph.prepare(48000.0, 32));

        REQUIRE(graph.remove_node(plugin));
        REQUIRE(destroyed.load(std::memory_order_acquire) == 1);
    }

    REQUIRE(destroyed.load(std::memory_order_acquire) == 1);
}

TEST_CASE("SignalGraph query helpers handle non-plugin and cleared nodes",
          "[host][graph][issue-493]") {
    SignalGraph graph;
    auto input = graph.add_input_node(2);
    auto gain = graph.add_gain_node("Gain");
    auto output = graph.add_output_node(2);

    REQUIRE_FALSE(graph.prepare(48000.0, 0));
    REQUIRE_FALSE(graph.prepare(48000.0, -16));
    REQUIRE_FALSE(graph.set_node_gain(999, 0.5f));
    REQUIRE(graph.node_gain(999) == 1.0f);
    REQUIRE_FALSE(graph.set_node_parameter(gain, 7, 0.25f));
    REQUIRE(graph.get_node_parameter(gain, 7) == 0.0f);
    REQUIRE_FALSE(graph.set_node_parameter(999, 7, 0.25f));
    REQUIRE(graph.get_node_parameter(999, 7) == 0.0f);
    REQUIRE(graph.node_latency_samples(999) == 0);

    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));
    REQUIRE(graph.node_latency_samples(gain) == 0);

    graph.clear();
    REQUIRE(graph.nodes().empty());
    REQUIRE(graph.connections().empty());
    REQUIRE(graph.latency_samples() == 0);
    REQUIRE(graph.node_latency_samples(gain) == 0);
    REQUIRE(graph.node_gain(gain) == 1.0f);
}

TEST_CASE("SignalGraph prepare rejects graphs beyond configured limits",
          "[host][graph][limits]") {
    SECTION("node count") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_nodes = 2;
        graph.set_limits(limits);

        auto input = graph.add_input_node(1, "in");
        auto gain = graph.add_gain_node("gain");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));

        REQUIRE_FALSE(graph.prepare(48000.0, 16));
        REQUIRE(graph.latency_samples() == 0);
    }

    SECTION("connection count") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_connections = 1;
        graph.set_limits(limits);

        auto input = graph.add_input_node(1, "in");
        auto gain = graph.add_gain_node("gain");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));

        REQUIRE_FALSE(graph.prepare(48000.0, 16));
        REQUIRE(graph.latency_samples() == 0);
    }

    SECTION("port count") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_ports = 3;
        graph.set_limits(limits);

        auto input = graph.add_input_node(2, "in");
        auto output = graph.add_output_node(2, "out");
        REQUIRE(graph.connect(input, 0, output, 0));
        REQUIRE(graph.connect(input, 1, output, 1));

        REQUIRE_FALSE(graph.prepare(48000.0, 16));
        REQUIRE(graph.latency_samples() == 0);
    }

    SECTION("block size") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_block_size = 8;
        graph.set_limits(limits);

        auto input = graph.add_input_node(1, "in");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, output, 0));

        REQUIRE_FALSE(graph.prepare(48000.0, 16));
        REQUIRE(graph.prepare(48000.0, 8));
    }
}

TEST_CASE("SignalGraph generated graph validation reports limit reasons",
          "[host][graph][generated][limits]") {
    SECTION("valid graph") {
        SignalGraph graph;
        auto input = graph.add_input_node(1, "in");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, output, 0));

        const auto validation = graph.validate_generated_graph(16);
        REQUIRE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::None);
    }

    SECTION("invalid block size") {
        SignalGraph graph;
        const auto validation = graph.validate_generated_graph(0);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::InvalidBlockSize);
        REQUIRE(validation.actual == 0);
        REQUIRE(validation.limit == 1);
    }

    SECTION("block size budget") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_block_size = 8;
        graph.set_limits(limits);

        const auto validation = graph.validate_generated_graph(16);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::MaxBlockSizeExceeded);
        REQUIRE(validation.actual == 16);
        REQUIRE(validation.limit == 8);
    }

    SECTION("node budget") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_nodes = 2;
        graph.set_limits(limits);

        auto input = graph.add_input_node(1, "in");
        auto gain = graph.add_gain_node("gain");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));

        const auto validation = graph.validate_generated_graph(16);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::NodeLimitExceeded);
        REQUIRE(validation.actual == 3);
        REQUIRE(validation.limit == 2);
    }

    SECTION("connection budget") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_connections = 1;
        graph.set_limits(limits);

        auto input = graph.add_input_node(1, "in");
        auto gain = graph.add_gain_node("gain");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));

        const auto validation = graph.validate_generated_graph(16);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::ConnectionLimitExceeded);
        REQUIRE(validation.actual == 2);
        REQUIRE(validation.limit == 1);
    }

    SECTION("port budget") {
        SignalGraph graph;
        auto limits = graph.limits();
        limits.max_ports = 3;
        graph.set_limits(limits);

        auto input = graph.add_input_node(2, "in");
        auto output = graph.add_output_node(2, "out");
        REQUIRE(graph.connect(input, 0, output, 0));
        REQUIRE(graph.connect(input, 1, output, 1));

        const auto validation = graph.validate_generated_graph(16);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::PortLimitExceeded);
        REQUIRE(validation.actual == 4);
        REQUIRE(validation.limit == 3);
    }

    SECTION("estimated work budget") {
        SignalGraph graph;
        auto input = graph.add_input_node(1, "in");
        auto gain = graph.add_gain_node("gain");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));

        const std::size_t estimated =
            graph.estimate_generated_graph_work_units(16);
        REQUIRE(estimated == 160);

        auto limits = graph.limits();
        limits.max_estimated_work_units = estimated;
        graph.set_limits(limits);
        REQUIRE(graph.validate_generated_graph(16).accepted);

        limits.max_estimated_work_units = estimated - 1;
        graph.set_limits(limits);
        const auto validation = graph.validate_generated_graph(16);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::EstimatedWorkExceeded);
        REQUIRE(validation.actual == estimated);
        REQUIRE(validation.limit == estimated - 1);
        REQUIRE_FALSE(graph.prepare(48000.0, 16));
    }

    SECTION("preflight does not clear the prepared snapshot") {
        SignalGraph graph;
        auto input = graph.add_input_node(1, "in");
        auto output = graph.add_output_node(1, "out");
        REQUIRE(graph.connect(input, 0, output, 0));
        REQUIRE(graph.prepare(48000.0, 8));
        const auto prepared = graph.prepared_stats();
        REQUIRE(prepared.node_count == 2);
        REQUIRE(prepared.max_block_size == 8);

        const auto validation = graph.validate_generated_graph(0);
        REQUIRE_FALSE(validation.accepted);
        REQUIRE(validation.reason
                == SignalGraph::GeneratedGraphValidationRejectReason::InvalidBlockSize);
        REQUIRE(graph.prepared_stats().node_count == 2);

        REQUIRE_FALSE(graph.prepare(48000.0, 0));
        REQUIRE(graph.prepared_stats().node_count == 0);
    }
}

TEST_CASE("SignalGraph prepares and routes a large graph at configured limits",
          "[host][graph][limits][scale]") {
    SignalGraph graph;
    constexpr int kGainNodes = 128;
    constexpr int kBlock = 16;

    auto limits = graph.limits();
    limits.max_nodes = static_cast<size_t>(kGainNodes + 2);
    limits.max_connections = static_cast<size_t>(kGainNodes + 1);
    limits.max_ports = static_cast<size_t>(kGainNodes * 4 + 2);
    limits.max_block_size = kBlock;
    graph.set_limits(limits);

    auto input = graph.add_input_node(1, "in");
    NodeId previous = input;
    for (int i = 0; i < kGainNodes; ++i) {
        auto gain = graph.add_gain_node("gain");
        REQUIRE(graph.connect(previous, 0, gain, 0));
        previous = gain;
    }
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(previous, 0, output, 0));

    REQUIRE(graph.nodes().size() == limits.max_nodes);
    REQUIRE(graph.connections().size() == limits.max_connections);
    REQUIRE(graph.prepare(48000.0, kBlock));

    std::vector<float> input_samples(kBlock, 0.0f);
    std::vector<float> output_samples(kBlock, -1.0f);
    for (int i = 0; i < kBlock; ++i) {
        input_samples[static_cast<size_t>(i)] = static_cast<float>(i) / 16.0f;
    }
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, kBlock);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, kBlock);

    graph.process(out_view, in_view, kBlock);

    for (int i = 0; i < kBlock; ++i) {
        REQUIRE_THAT(output_samples[static_cast<size_t>(i)],
                     WithinAbs(input_samples[static_cast<size_t>(i)], 1e-6f));
    }

#if defined(__unix__) || defined(__APPLE__)
    std::fill(output_samples.begin(), output_samples.end(), -1.0f);
    {
        pulp::native_components::test::RtNoAllocScope no_alloc;
        graph.process(out_view, in_view, kBlock);
    }

    for (int i = 0; i < kBlock; ++i) {
        REQUIRE_THAT(output_samples[static_cast<size_t>(i)],
                     WithinAbs(input_samples[static_cast<size_t>(i)], 1e-6f));
    }
#endif
}

TEST_CASE("SignalGraph exposes prepared runtime stats",
          "[host][graph][stats]") {
    SignalGraph graph;
    REQUIRE(graph.prepared_stats().node_count == 0);

    auto input = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));

    constexpr int kBlock = 16;
    REQUIRE(graph.prepare(48000.0, kBlock));

    const auto stats = graph.prepared_stats();
    REQUIRE(stats.node_count == 3);
    REQUIRE(stats.ordered_node_count == 3);
    REQUIRE(stats.connection_count == 2);
    REQUIRE(stats.total_ports == 6);
    REQUIRE(stats.max_block_size == kBlock);
    REQUIRE(stats.node_audio_buffer_bytes == 384);
    REQUIRE(stats.automation_buffer_bytes == 0);
    REQUIRE(stats.delay_buffer_bytes == 0);
    REQUIRE(stats.total_prepared_buffer_bytes == stats.node_audio_buffer_bytes);

    graph.add_gain_node("later");
    const auto invalidated = graph.prepared_stats();
    REQUIRE(invalidated.node_count == 0);
    REQUIRE(invalidated.total_prepared_buffer_bytes == 0);
}

TEST_CASE("SignalGraph evaluates optional runtime budget from prepared stats",
          "[host][graph][budget-policy]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    pulp::runtime::RuntimeBudgetFrame exact(256);
    auto report = graph.evaluate_optional_runtime_budget(
        exact, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.prepared);
    REQUIRE(report.estimated_cost == 256);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Run);
    REQUIRE(report.should_run_optional_work());
    REQUIRE(report.frame_stats.run_count == 1);
    REQUIRE(report.frame_stats.remaining_budget == 0);

    pulp::runtime::RuntimeBudgetFrame tight(255);
    report = graph.evaluate_optional_runtime_budget(
        tight, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Bypass);
    REQUIRE_FALSE(report.should_run_optional_work());
    REQUIRE(report.frame_stats.bypass_count == 1);
    REQUIRE(report.frame_stats.remaining_budget == 255);

    pulp::runtime::RuntimeBudgetPolicy policy;
    policy.shed_background_on_overload = true;
    pulp::runtime::RuntimeBudgetFrame overloaded(1024, policy, true);
    report = graph.evaluate_optional_runtime_budget(
        overloaded, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Shed);
    REQUIRE(report.frame_stats.shed_count == 1);
}

TEST_CASE("SignalGraph optional runtime budget reports unprepared graphs",
          "[host][graph][budget-policy]") {
    SignalGraph graph;
    pulp::runtime::RuntimeBudgetFrame frame(0);

    const auto report = graph.evaluate_optional_runtime_budget(
        frame, pulp::runtime::RuntimeWorkLane::Background);

    REQUIRE_FALSE(report.prepared);
    REQUIRE(report.estimated_cost == 0);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Run);
    REQUIRE(report.frame_stats.run_count == 1);
}

TEST_CASE("SignalGraph optional runtime budget has deterministic large-graph cost",
          "[host][graph][budget-policy][scale]") {
    SignalGraph graph;
    constexpr int kGainNodes = 128;
    constexpr int kBlock = 16;

    auto input = graph.add_input_node(1, "in");
    NodeId previous = input;
    for (int i = 0; i < kGainNodes; ++i) {
        auto gain = graph.add_gain_node("gain");
        REQUIRE(graph.connect(previous, 0, gain, 0));
        previous = gain;
    }
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(previous, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, kBlock));

    const auto stats = graph.prepared_stats();
    REQUIRE(stats.node_count == static_cast<std::size_t>(kGainNodes + 2));
    REQUIRE(stats.connection_count == static_cast<std::size_t>(kGainNodes + 1));
    REQUIRE(stats.total_ports == static_cast<std::size_t>(kGainNodes * 4 + 2));
    REQUIRE(stats.max_block_size == kBlock);

    const auto expected_cost =
        static_cast<std::uint64_t>(stats.node_count) * 16u
        + static_cast<std::uint64_t>(stats.connection_count) * 8u
        + static_cast<std::uint64_t>(stats.total_ports)
              * static_cast<std::uint64_t>(stats.max_block_size)
        + static_cast<std::uint64_t>(
              stats.total_prepared_buffer_bytes / sizeof(float));

    pulp::runtime::RuntimeBudgetFrame exact(expected_cost);
    auto report = graph.evaluate_optional_runtime_budget(
        exact, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.estimated_cost == expected_cost);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Run);
    REQUIRE(report.frame_stats.remaining_budget == 0);

    pulp::runtime::RuntimeBudgetFrame tight(expected_cost - 1);
    report = graph.evaluate_optional_runtime_budget(
        tight, pulp::runtime::RuntimeWorkLane::Background);
    REQUIRE(report.estimated_cost == expected_cost);
    REQUIRE(report.decision.action == pulp::runtime::RuntimeBudgetAction::Bypass);
}

TEST_CASE("SignalGraph clears prepared runtime stats after failed prepare",
          "[host][graph][stats][limits]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));
    REQUIRE(graph.prepared_stats().node_count == 2);

    auto limits = graph.limits();
    limits.max_nodes = 1;
    graph.set_limits(limits);
    REQUIRE_FALSE(graph.prepare(48000.0, 16));

    const auto stats = graph.prepared_stats();
    REQUIRE(stats.node_count == 0);
    REQUIRE(stats.connection_count == 0);
    REQUIRE(stats.total_prepared_buffer_bytes == 0);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SignalGraph optional runtime budget path allocates zero times",
          "[host][graph][budget-policy][rt-safety]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    pulp::runtime::RuntimeBudgetFrame frame(1024);
    {
        pulp::native_components::test::RtNoAllocScope no_alloc;
        const auto report = graph.evaluate_optional_runtime_budget(
            frame, pulp::runtime::RuntimeWorkLane::Opportunistic);
        REQUIRE(report.prepared);
        REQUIRE(report.should_run_optional_work());
    }
}

TEST_CASE("SignalGraph prepared runtime stats path allocates zero times",
          "[host][graph][stats][rt-safety]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    {
        pulp::native_components::test::RtNoAllocScope no_alloc;
        const auto stats = graph.prepared_stats();
        REQUIRE(stats.node_count == 2);
        REQUIRE(stats.total_prepared_buffer_bytes > 0);
    }
}
#endif

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

TEST_CASE("SignalGraph rejects audio connections with invalid ports",
          "[host][graph][routing]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto output = graph.add_output_node(1, "out");

    REQUIRE_FALSE(graph.connect(input, 99, gain, 0));
    REQUIRE_FALSE(graph.connect(input, 0, gain, 99));
    REQUIRE_FALSE(graph.connect(gain, 42, output, 0));
    REQUIRE(graph.connections().empty());
    REQUIRE(graph.prepare(48000.0, 8));

    std::vector<float> input_samples(8, 1.0f);
    std::vector<float> output_samples(8, -1.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 8);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 8);

    graph.process(out_view, in_view, 8);

    for (float sample : output_samples) {
        REQUIRE(sample == 0.0f);
    }
    graph.release();
}

// ── Mock plugin for PDC tests ───────────────────────────────────────────

namespace {
class MockLatencyPlugin final : public PluginSlot {
public:
    MockLatencyPlugin(int latency, int num_ch)
        : latency_(latency), num_ch_(num_ch) {
        info_.name = "MockLatency";
        info_.num_inputs = num_ch;
        info_.num_outputs = num_ch;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& /*pe*/,
                 int n) override {
        // Actually delay the audio by the reported latency samples so PDC
        // behavior can be measured end-to-end: the plugin's internal delay
        // line and the host's reported-latency view agree.
        if (rings_.size() != (size_t)num_ch_) {
            rings_.assign((size_t)num_ch_,
                          std::vector<float>((size_t)std::max(1, latency_ + 1), 0.f));
            wp_ = 0;
        }
        for (int c = 0; c < num_ch_ && (size_t)c < out.num_channels(); ++c) {
            const float* s = (size_t)c < in.num_channels() ? in.channel_ptr((size_t)c) : nullptr;
            float* d = out.channel_ptr((size_t)c);
            if (latency_ <= 0) {
                if (s) std::memcpy(d, s, sizeof(float) * (size_t)n);
                else std::memset(d, 0, sizeof(float) * (size_t)n);
                continue;
            }
            const int ring_size = (int)rings_[(size_t)c].size();
            int wp = wp_;
            int rp = wp - latency_;
            if (rp < 0) rp += ring_size;
            for (int i = 0; i < n; ++i) {
                rings_[(size_t)c][(size_t)wp] = s ? s[i] : 0.f;
                d[i] = rings_[(size_t)c][(size_t)rp];
                if (++wp == ring_size) wp = 0;
                if (++rp == ring_size) rp = 0;
            }
        }
        // Advance write pointer by one block (shared across channels).
        wp_ = (wp_ + n) % (int)std::max<size_t>(1, rings_.empty() ? 1 : rings_[0].size());
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return latency_; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_;
    int latency_ = 0;
    int num_ch_ = 2;
    std::vector<std::vector<float>> rings_;
    int wp_ = 0;
};

class PrepareFailPlugin final : public PluginSlot {
public:
    PrepareFailPlugin() {
        info_.name = "PrepareFail";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return false; }
    void release() override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int) override {}
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    PluginInfo info_;
};
} // namespace

TEST_CASE("SignalGraph prepare failure leaves process output silent",
          "[host][graph]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto plugin = graph.add_plugin_node(std::make_unique<PrepareFailPlugin>(),
                                        1, 1, "fail");
    auto output = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE_FALSE(graph.prepare(48000.0, 16));
    REQUIRE(graph.latency_samples() == 0);
    REQUIRE(graph.node_latency_samples(plugin) == 0);

    std::vector<float> input_samples(16, 1.0f);
    std::vector<float> output_samples(16, -1.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 16);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 16);

    graph.process(out_view, in_view, 16);
    for (float sample : output_samples) {
        REQUIRE(sample == 0.0f);
    }
}

TEST_CASE("SignalGraph process silences oversized blocks",
          "[host][graph]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples(8, 1.0f);
    std::vector<float> output_samples(8, -1.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 8);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 8);

    graph.process(out_view, in_view, 8);

    for (float sample : output_samples) {
        REQUIRE(sample == 0.0f);
    }
    graph.release();
}

TEST_CASE("SignalGraph process ignores non-positive block sizes",
          "[host][graph]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples(4, 1.0f);
    std::vector<float> output_samples{3.0f, 4.0f, 5.0f, 6.0f};
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 0);
    REQUIRE(output_samples == std::vector<float>{3.0f, 4.0f, 5.0f, 6.0f});

    graph.process(out_view, in_view, -1);
    REQUIRE(output_samples == std::vector<float>{3.0f, 4.0f, 5.0f, 6.0f});

    graph.release();
}

TEST_CASE("SignalGraph clears stale audio input channels",
          "[host][graph][routing]") {
    SignalGraph graph;
    auto input = graph.add_input_node(2, "in");
    auto gain = graph.add_gain_node("gain");
    auto output = graph.add_output_node(2, "out");

    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(input, 1, gain, 1));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.connect(gain, 1, output, 1));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> first_l(4, 0.25f);
    std::vector<float> first_r(4, 0.75f);
    std::vector<float> out_l(4, -1.0f);
    std::vector<float> out_r(4, -1.0f);
    const float* first_ptrs[2] = {first_l.data(), first_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> first_view(first_ptrs, 2, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 2, 4);

    graph.process(out_view, first_view, 4);
    for (float sample : out_l) REQUIRE(sample == 0.25f);
    for (float sample : out_r) REQUIRE(sample == 0.75f);

    std::vector<float> second_l(4, 0.5f);
    const float* second_ptrs[1] = {second_l.data()};
    pulp::audio::BufferView<const float> second_view(second_ptrs, 1, 4);
    std::fill(out_l.begin(), out_l.end(), -1.0f);
    std::fill(out_r.begin(), out_r.end(), -1.0f);

    graph.process(out_view, second_view, 4);
    for (float sample : out_l) REQUIRE(sample == 0.5f);
    for (float sample : out_r) REQUIRE(sample == 0.0f);
}

TEST_CASE("SignalGraph placeholder plugin nodes preserve identity and clear extra outputs",
          "[host][graph][routing]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    PluginInfo missing = make_plugin_info("Missing CLAP", 1, 2);
    missing.manufacturer = "Pulp";
    missing.version = "1.2.3";
    missing.path = "/tmp/not-a-plugin.clap";
    missing.unique_id = "missing.plugin";
    auto plugin = graph.add_plugin_node(missing);
    auto output = graph.add_output_node(2, "out");

    const auto* plugin_node = graph.node(plugin);
    REQUIRE(plugin_node != nullptr);
    REQUIRE(plugin_node->type == NodeType::Plugin);
    REQUIRE_FALSE(plugin_node->plugin);
    REQUIRE(plugin_node->plugin_info.name == "Missing CLAP");
    REQUIRE(plugin_node->plugin_info.unique_id == "missing.plugin");
    REQUIRE(plugin_node->num_input_ports == 1);
    REQUIRE(plugin_node->num_output_ports == 2);

    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.connect(plugin, 1, output, 1));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples(4, 0.625f);
    std::vector<float> out_l(4, -1.0f);
    std::vector<float> out_r(4, -1.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 2, 4);

    graph.process(out_view, in_view, 4);

    for (float sample : out_l) REQUIRE(sample == 0.625f);
    for (float sample : out_r) REQUIRE(sample == 0.0f);
}

TEST_CASE("SignalGraph PDC aligns parallel branches", "[host][graph][pdc]") {
    // in → A(latency=32) → mix
    // in → B(latency=0)  → mix   (should be delayed by 32 so A and B align)
    SignalGraph graph;
    auto in = graph.add_input_node(1, "in");
    auto a  = graph.add_plugin_node(std::make_unique<MockLatencyPlugin>(32, 1), 1, 1, "A");
    auto b  = graph.add_plugin_node(std::make_unique<MockLatencyPlugin>(0,  1), 1, 1, "B");
    auto out = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in, 0, a, 0));
    REQUIRE(graph.connect(in, 0, b, 0));
    REQUIRE(graph.connect(a, 0, out, 0));
    REQUIRE(graph.connect(b, 0, out, 0));

    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(graph.latency_samples() == 32);
    REQUIRE(graph.node_latency_samples(out) == 32);

    // Drive an impulse at sample 0 and check the first non-zero sample in the
    // output is at index 32 (aligned) with amplitude 2.0 (both branches sum).
    std::vector<float> in_buf(64, 0.f);
    in_buf[0] = 1.0f;
    std::vector<float> out_buf(64, 999.f);
    const float* in_ptrs[1]  = {in_buf.data()};
    float*       out_ptrs[1] = {out_buf.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 64);

    graph.process(ov, iv, 64);

    // Samples 0..31 should be silent (pre-latency), sample 32 should carry
    // the impulse from both branches aligned (2.0).
    for (int i = 0; i < 32; ++i) REQUIRE(out_buf[i] == 0.0f);
    REQUIRE(out_buf[32] == 2.0f);
    for (int i = 33; i < 64; ++i) REQUIRE(out_buf[i] == 0.0f);
    graph.release();
}

TEST_CASE("SignalGraph serial plugin latencies accumulate", "[host][graph][pdc]") {
    SignalGraph graph;
    auto in = graph.add_input_node(1, "in");
    auto a  = graph.add_plugin_node(std::make_unique<MockLatencyPlugin>(10, 1), 1, 1, "A");
    auto b  = graph.add_plugin_node(std::make_unique<MockLatencyPlugin>(20, 1), 1, 1, "B");
    auto out = graph.add_output_node(1, "out");

    graph.connect(in, 0, a, 0);
    graph.connect(a,  0, b, 0);
    graph.connect(b,  0, out, 0);

    REQUIRE(graph.prepare(48000.0, 32));
    REQUIRE(graph.latency_samples() == 30);
}

// Plugin that sums its main input (ports 0,1) and sidechain (ports 2,3)
// into its output, so a test can observe whether the sidechain arrived.
namespace {
class SidechainSum final : public PluginSlot {
public:
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& /*pe*/,
                 int n) override {
        const size_t nc = out.num_channels();
        for (size_t c = 0; c < nc; ++c) {
            float* d = out.channel_ptr(c);
            const float* a = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            const float* b = (c + 2) < in.num_channels() ? in.channel_ptr(c + 2) : nullptr;
            for (int i = 0; i < n; ++i) d[i] = (a ? a[i] : 0.f) + (b ? b[i] : 0.f);
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_ = make_plugin_info("SC");
};
} // namespace

TEST_CASE("SignalGraph routes sidechain via named port connections",
          "[host][graph][sidechain]") {
    // Two input nodes feed a 4-input-port plugin: ports 0/1 are the main
    // stereo bus, ports 2/3 are the sidechain. The plugin sums them.
    SignalGraph graph;
    auto main_in = graph.add_input_node(2, "main");
    auto side_in = graph.add_input_node(2, "side");
    auto p = graph.add_plugin_node(std::make_unique<SidechainSum>(), 4, 2, "mix");
    auto out = graph.add_output_node(2, "out");

    REQUIRE(graph.connect(main_in, 0, p, 0));
    REQUIRE(graph.connect(main_in, 1, p, 1));
    REQUIRE(graph.connect(side_in, 0, p, 2));   // sidechain L
    REQUIRE(graph.connect(side_in, 1, p, 3));   // sidechain R
    REQUIRE(graph.connect(p, 0, out, 0));
    REQUIRE(graph.connect(p, 1, out, 1));

    REQUIRE(graph.prepare(48000.0, 16));

    // Drive the sole AudioInput (the process-level input view) into
    // main_in; there's currently no API for per-node independent input
    // injection, so both main and sidechain see the same 2-channel input
    // buffer. We distinguish them by writing different values into L/R and
    // wiring main.0 from L and side.0 from L — the graph already dispatches
    // input[c] into each AudioInput's port c, so both nodes mirror the
    // same buffer. We check that the plugin saw *two* copies summed: L+L.
    std::vector<float> in_l(16, 0.3f), in_r(16, 0.5f);
    std::vector<float> out_l(16, 0.f), out_r(16, 0.f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 2, 16);
    pulp::audio::BufferView<float>       ov(out_ptrs, 2, 16);

    graph.process(ov, iv, 16);

    // SidechainSum adds ports {0,2} into out[0] and ports {1,3} into
    // out[1]. Because main_in.0 and side_in.0 both pull from input[0],
    // both carry 0.3 — so out[0] = 0.3 + 0.3 = 0.6. Similarly out[1] =
    // 0.5 + 0.5 = 1.0.
    for (int i = 0; i < 16; ++i) {
        REQUIRE(std::abs(out_l[i] - 0.6f) < 1e-6f);
        REQUIRE(std::abs(out_r[i] - 1.0f) < 1e-6f);
    }
    graph.release();
}

TEST_CASE("SignalGraph connect_feedback allows cycles with one-block delay",
          "[host][graph][feedback]") {
    // in → g → out, with a feedback edge g.out[0] → g.in[0]: each block,
    // gain reads its own previous block's output summed with the fresh
    // input. Classic one-block delay feedback loop.
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto g   = graph.add_gain_node("g");
    auto out = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in, 0, g, 0));
    REQUIRE(graph.connect(g,  0, out, 0));

    // Adding g → g as a forward edge must fail (self-loop = cycle), but
    // connect_feedback should accept it.
    REQUIRE_FALSE(graph.connect(g, 0, g, 0));
    REQUIRE(graph.connect_feedback(g, 0, g, 0));
    REQUIRE(graph.connections().size() == 3);

    // Topological sort ignores feedback edges, so the order is still valid.
    REQUIRE(graph.processing_order().size() == 3);

    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE(graph.set_node_gain(g, 0.5f));

    std::vector<float> in_buf(8, 0.f);
    in_buf[0] = 1.0f;
    std::vector<float> out_buf(8, 999.f);
    const float* in_ptrs[1]  = {in_buf.data()};
    float*       out_ptrs[1] = {out_buf.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 8);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 8);

    // Block 0: feedback buffer is still zero. Gain input = impulse + 0.
    //   g.out[0] = 0.5 * 1 = 0.5; g.out[i>0] = 0.
    graph.process(ov, iv, 8);
    REQUIRE(out_buf[0] == 0.5f);
    for (int i = 1; i < 8; ++i) REQUIRE(out_buf[i] == 0.0f);

    // Block 1: input silent, but feedback_prev holds block 0's g output
    // (0.5, 0, 0, ...). Gain input = 0 + (0.5, 0, 0, ...). Out = 0.25.
    std::fill(in_buf.begin(), in_buf.end(), 0.0f);
    std::fill(out_buf.begin(), out_buf.end(), 999.f);
    graph.process(ov, iv, 8);
    REQUIRE(out_buf[0] == 0.25f);
    for (int i = 1; i < 8; ++i) REQUIRE(out_buf[i] == 0.0f);

    // Block 2: feedback_prev = block 1's g output = (0.25, 0, ...).
    std::fill(out_buf.begin(), out_buf.end(), 999.f);
    graph.process(ov, iv, 8);
    REQUIRE(out_buf[0] == 0.125f);
    for (int i = 1; i < 8; ++i) REQUIRE(out_buf[i] == 0.0f);

    graph.release();
}

// A plugin that forwards its MIDI input to its MIDI output unchanged and
// records the events it saw so the test can inspect them.
namespace {
class MidiForwarder final : public PluginSlot {
public:
    MidiForwarder() { last_seen_.attach_ump(&last_seen_ump_); }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::host::ParameterEventQueue& /*pe*/,
                 int n) override {
        last_seen_.clear();
        last_seen_.clear_sysex();
        last_seen_ump_.clear();
        for (const auto& ev : midi_in) {
            last_seen_.add(ev);
            midi_out.add(ev);
        }
        for (const auto& sx : midi_in.sysex()) {
            last_seen_.add_sysex_copy(sx.data.data(), sx.data.size(),
                                      sx.sample_offset, sx.timestamp);
            midi_out.add_sysex_copy(sx.data.data(), sx.data.size(),
                                    sx.sample_offset, sx.timestamp);
        }
        if (const auto* in_ump = midi_in.ump()) {
            auto* out_ump = midi_out.ump();
            for (const auto& ev : *in_ump) {
                last_seen_ump_.add(ev);
                if (out_ump) out_ump->add(ev);
            }
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    const pulp::midi::MidiBuffer& last_seen() const { return last_seen_; }
    const pulp::midi::UmpBuffer& last_seen_ump() const { return last_seen_ump_; }
private:
    PluginInfo info_ = make_plugin_info("MidiFwd", 0, 0, "MidiEffect");
    pulp::midi::MidiBuffer last_seen_;
    pulp::midi::UmpBuffer last_seen_ump_;
};

class MidiFlooder final : public PluginSlot {
public:
    explicit MidiFlooder(std::size_t event_count) : event_count_(event_count) {}
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const auto block = static_cast<std::size_t>(std::max(1, n));
        for (std::size_t i = 0; i < event_count_; ++i) {
            auto ev = pulp::midi::MidiEvent::note_on(
                0, static_cast<int>(i % 127), 100);
            ev.sample_offset = static_cast<int32_t>(i % block);
            midi_out.add(ev);
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(n));
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_ = make_plugin_info("MidiFlood", 0, 0, "MidiGenerator");
    std::size_t event_count_ = 0;
};
} // namespace

TEST_CASE("SignalGraph connect_midi routes events through the graph",
          "[host][graph][midi]") {
    // midi_in → plugin (forwards) → midi_out
    SignalGraph graph;
    auto mi  = graph.add_midi_input_node("keys");
    auto fwd = std::make_unique<MidiForwarder>();
    auto* fwd_ptr = fwd.get();
    auto p   = graph.add_plugin_node(std::move(fwd), 0, 0, "fwd");
    auto mo  = graph.add_midi_output_node("thru");

    REQUIRE(graph.connect_midi(mi, p));
    REQUIRE(graph.connect_midi(p, mo));
    REQUIRE(graph.connections().size() == 2);

    REQUIRE(graph.prepare(48000.0, 32));

    pulp::midi::MidiBuffer in_events;
    auto ev0 = pulp::midi::MidiEvent::note_on(0, 60, 100);
    ev0.sample_offset = 0;
    auto ev1 = pulp::midi::MidiEvent::note_off(0, 60, 0);
    ev1.sample_offset = 16;
    in_events.add(ev0);
    in_events.add(ev1);
    in_events.add_sysex({0xF0, 0x7D, 0x01, 0xF7}, 8, 1.25);
    pulp::midi::UmpBuffer in_ump;
    in_ump.add(pulp::midi::UmpPacket::note_on_2(0, 2, 67, 0x8000), 24);
    in_events.attach_ump(&in_ump);
    REQUIRE(graph.inject_midi(mi, in_events));

    // Dummy audio (graph has no audio path; process() still runs).
    float in_sample = 0.f, out_sample = 0.f;
    const float* in_ptrs[1]  = {&in_sample};
    float*       out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> iv(in_ptrs, 0, 32);
    pulp::audio::BufferView<float>       ov(out_ptrs, 0, 32);
    graph.process(ov, iv, 32);

    // The forwarder plugin saw both events.
    REQUIRE(fwd_ptr->last_seen().size() == 2);
    REQUIRE(fwd_ptr->last_seen()[0].sample_offset == 0);
    REQUIRE(fwd_ptr->last_seen()[1].sample_offset == 16);
    REQUIRE(fwd_ptr->last_seen().sysex_size() == 1);
    REQUIRE(fwd_ptr->last_seen().sysex()[0].data
            == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});
    REQUIRE(fwd_ptr->last_seen().sysex()[0].sample_offset == 8);
    REQUIRE(fwd_ptr->last_seen_ump().size() == 1);
    REQUIRE(fwd_ptr->last_seen_ump()[0].sample_offset == 24);
    REQUIRE(fwd_ptr->last_seen_ump()[0].packet.channel() == 2);

    // MidiOutput sink reports an incomplete copy when the caller has no UMP
    // sidecar storage for the otherwise available UMP packets.
    pulp::midi::MidiBuffer arrived_without_ump_storage;
    REQUIRE_FALSE(graph.extract_midi(mo, arrived_without_ump_storage));
    REQUIRE(arrived_without_ump_storage.size() == 2);
    REQUIRE(arrived_without_ump_storage.sysex_size() == 1);

    // MidiOutput sink received the forwarded events.
    pulp::midi::MidiBuffer arrived;
    pulp::midi::UmpBuffer arrived_ump;
    arrived.attach_ump(&arrived_ump);
    REQUIRE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.empty());
    REQUIRE(arrived.sysex_size() == 0);
    REQUIRE(arrived_ump.size() == 1);
    REQUIRE(arrived_ump[0].sample_offset == 24);
    REQUIRE(arrived_ump[0].packet.channel() == 2);

    arrived.clear();
    arrived.clear_sysex();
    arrived_ump.clear();
    graph.process(ov, iv, 32);
    REQUIRE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.empty());
    REQUIRE(arrived.sysex_size() == 0);
    REQUIRE(arrived_ump.empty());
    REQUIRE(fwd_ptr->last_seen().empty());
    REQUIRE(fwd_ptr->last_seen().sysex_size() == 0);
    REQUIRE(fwd_ptr->last_seen_ump().empty());

    graph.release();
}

TEST_CASE("SignalGraph preserves MPE-bearing UMP events through MIDI edges",
          "[host][graph][midi][mpe]") {
    SignalGraph graph;
    auto mi = graph.add_midi_input_node("mpe");
    auto fwd = std::make_unique<MidiForwarder>();
    auto* fwd_ptr = fwd.get();
    auto p = graph.add_plugin_node(std::move(fwd), 0, 0, "mpe-fwd");
    auto mo = graph.add_midi_output_node("mpe-out");

    REQUIRE(graph.connect_midi(mi, p));
    REQUIRE(graph.connect_midi(p, mo));
    REQUIRE(graph.prepare(48000.0, 64));

    pulp::midi::MidiBuffer mpe_events;
    pulp::midi::UmpBuffer mpe_ump;
    mpe_ump.add(pulp::midi::UmpPacket::note_on_2(0, 1, 60, 0x8000), 3);
    mpe_ump.add(pulp::midi::UmpPacket::per_note_pitch_bend(
                    0, 1, 60, 0xC0000000u),
                11);
    mpe_ump.add(pulp::midi::UmpPacket::registered_per_note_cc(
                    0, 1, 60, 74, 0xFFFFFFFFu),
                19);
    mpe_ump.add(pulp::midi::UmpPacket::per_note_management(
                    0,
                    1,
                    60,
                    pulp::midi::UmpPacket::kPerNoteDetachControllers),
                27);
    mpe_events.attach_ump(&mpe_ump);
    REQUIRE(graph.inject_midi(mi, mpe_events));

    float in_sample = 0.0f;
    float out_sample = 0.0f;
    const float* in_ptrs[1] = {&in_sample};
    float* out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> iv(in_ptrs, 0, 64);
    pulp::audio::BufferView<float> ov(out_ptrs, 0, 64);
    graph.process(ov, iv, 64);

    REQUIRE(fwd_ptr->last_seen_ump().size() == 4);
    REQUIRE(fwd_ptr->last_seen_ump()[0].sample_offset == 3);
    REQUIRE(fwd_ptr->last_seen_ump()[1].packet.status() == 0x61);
    REQUIRE(fwd_ptr->last_seen_ump()[2].packet.status() == 0x01);
    REQUIRE(fwd_ptr->last_seen_ump()[3].packet.status() == 0xF1);

    pulp::midi::MidiBuffer arrived;
    pulp::midi::UmpBuffer arrived_ump;
    arrived.attach_ump(&arrived_ump);
    REQUIRE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived_ump.size() == 4);
    REQUIRE(arrived_ump[0].sample_offset == 3);
    REQUIRE(arrived_ump[1].sample_offset == 11);
    REQUIRE(arrived_ump[2].sample_offset == 19);
    REQUIRE(arrived_ump[3].sample_offset == 27);

    pulp::midi::MpeVoiceTracker tracker;
    pulp::midi::MpeBuffer derived;
    int32_t current_sample_offset = 0;
    pulp::midi::bind_tracker_to_buffer(tracker, derived, current_sample_offset);
    for (const auto& ev : arrived_ump) {
        current_sample_offset = ev.sample_offset;
        REQUIRE(tracker.process(ev.packet));
    }

    REQUIRE(derived.size() == 3);
    REQUIRE(derived[0].sample_offset == 3);
    REQUIRE(derived[0].kind == pulp::midi::MpeExpressionEvent::Kind::NoteOn);
    REQUIRE(derived[1].sample_offset == 11);
    REQUIRE(derived[1].kind == pulp::midi::MpeExpressionEvent::Kind::PitchBend);
    REQUIRE_THAT(derived[1].state.pitch_bend_semitones,
                 WithinAbs(24.0f, 0.001f));
    REQUIRE(derived[2].sample_offset == 19);
    REQUIRE(derived[2].kind == pulp::midi::MpeExpressionEvent::Kind::Timbre);
    REQUIRE_THAT(derived[2].state.timbre, WithinAbs(1.0f, 0.001f));
    const auto* note = tracker.find(1, 60);
    REQUIRE(note != nullptr);
    REQUIRE(note->detached);

    graph.release();
}

TEST_CASE("SignalGraph MIDI sidecar drops are caller-visible",
          "[host][graph][midi]") {
    constexpr std::size_t event_capacity = ParameterEventQueue::kCapacity;
    constexpr std::size_t sysex_capacity = 128;
    constexpr std::size_t sysex_payload_capacity = 4096;

    SignalGraph graph;
    auto mi = graph.add_midi_input_node("keys");
    auto mo = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(mi, mo));
    REQUIRE(graph.prepare(48000.0, 32));

    float in_sample = 0.f, out_sample = 0.f;
    const float* in_ptrs[1]  = {&in_sample};
    float*       out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> iv(in_ptrs, 0, 32);
    pulp::audio::BufferView<float>       ov(out_ptrs, 0, 32);

    pulp::midi::MidiBuffer short_overflow;
    for (std::size_t i = 0; i <= event_capacity; ++i) {
        short_overflow.add(pulp::midi::MidiEvent::note_on(
            0, static_cast<int>(i % 127), 100));
    }
    REQUIRE_FALSE(graph.inject_midi(mi, short_overflow));
    graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer arrived;
    REQUIRE_FALSE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.size() == event_capacity);

    pulp::midi::MidiBuffer sysex_count_overflow;
    for (std::size_t i = 0; i <= sysex_capacity; ++i) {
        sysex_count_overflow.add_sysex(
            {0xF0, 0x7D, static_cast<uint8_t>(i & 0x7F), 0xF7},
            0,
            0.0);
    }
    REQUIRE_FALSE(graph.inject_midi(mi, sysex_count_overflow));
    graph.process(ov, iv, 32);
    arrived.clear();
    arrived.clear_sysex();
    REQUIRE_FALSE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.sysex_size() == sysex_capacity);

    pulp::midi::MidiBuffer oversized_sysex;
    oversized_sysex.add_sysex(std::vector<uint8_t>(
        sysex_payload_capacity + 1, uint8_t{0x7D}));
    REQUIRE_FALSE(graph.inject_midi(mi, oversized_sysex));
    graph.process(ov, iv, 32);
    arrived.clear();
    arrived.clear_sysex();
    REQUIRE_FALSE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.sysex_size() == 0);

    pulp::midi::MidiBuffer ump_overflow;
    pulp::midi::UmpBuffer in_ump;
    for (std::size_t i = 0; i <= event_capacity; ++i) {
        in_ump.add(pulp::midi::UmpPacket::note_on_2(
            0, 2, static_cast<uint8_t>(i % 127), 0x8000), 0);
    }
    ump_overflow.attach_ump(&in_ump);
    REQUIRE_FALSE(graph.inject_midi(mi, ump_overflow));
    graph.process(ov, iv, 32);
    arrived.clear();
    arrived.clear_sysex();
    pulp::midi::UmpBuffer arrived_ump;
    arrived.attach_ump(&arrived_ump);
    REQUIRE_FALSE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived_ump.size() == event_capacity);

    REQUIRE_FALSE(graph.inject_midi(mi, ump_overflow));
    graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer missing_ump_extract;
    REQUIRE_FALSE(graph.extract_midi(mo, missing_ump_extract));
    pulp::midi::MidiBuffer recovered_ump_extract;
    pulp::midi::UmpBuffer recovered_ump;
    recovered_ump_extract.attach_ump(&recovered_ump);
    REQUIRE_FALSE(graph.extract_midi(mo, recovered_ump_extract));
    REQUIRE(recovered_ump.size() == event_capacity);

    SignalGraph fan_in_graph;
    auto mi_a = fan_in_graph.add_midi_input_node("a");
    auto mi_b = fan_in_graph.add_midi_input_node("b");
    auto fan_in_out = fan_in_graph.add_midi_output_node("merged");
    REQUIRE(fan_in_graph.connect_midi(mi_a, fan_in_out));
    REQUIRE(fan_in_graph.connect_midi(mi_b, fan_in_out));
    REQUIRE(fan_in_graph.prepare(48000.0, 32));
    pulp::midi::MidiBuffer full_a;
    pulp::midi::MidiBuffer full_b;
    for (std::size_t i = 0; i < event_capacity; ++i) {
        full_a.add(pulp::midi::MidiEvent::note_on(
            0, static_cast<int>(i % 127), 100));
        full_b.add(pulp::midi::MidiEvent::note_off(
            0, static_cast<int>(i % 127), 0));
    }
    REQUIRE(fan_in_graph.inject_midi(mi_a, full_a));
    REQUIRE(fan_in_graph.inject_midi(mi_b, full_b));
    fan_in_graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer fan_in_arrived;
    REQUIRE_FALSE(fan_in_graph.extract_midi(fan_in_out, fan_in_arrived));
    REQUIRE(fan_in_arrived.size() == event_capacity);
    fan_in_graph.release();

    SignalGraph plugin_overflow_graph;
    auto flood = plugin_overflow_graph.add_plugin_node(
        std::make_unique<MidiFlooder>(event_capacity + 1), 0, 0, "flood");
    auto flood_out = plugin_overflow_graph.add_midi_output_node("flood-out");
    REQUIRE(plugin_overflow_graph.connect_midi(flood, flood_out));
    REQUIRE(plugin_overflow_graph.prepare(48000.0, 32));
    plugin_overflow_graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer flood_arrived;
    REQUIRE_FALSE(plugin_overflow_graph.extract_midi(flood_out, flood_arrived));
    REQUIRE(flood_arrived.size() == event_capacity);
    plugin_overflow_graph.release();

    SignalGraph plugin_chain_overflow_graph;
    auto chain_flood = plugin_chain_overflow_graph.add_plugin_node(
        std::make_unique<MidiFlooder>(event_capacity + 1), 0, 0, "chain-flood");
    auto chain_fwd = plugin_chain_overflow_graph.add_plugin_node(
        std::make_unique<MidiForwarder>(), 0, 0, "chain-fwd");
    auto chain_out = plugin_chain_overflow_graph.add_midi_output_node("chain-out");
    REQUIRE(plugin_chain_overflow_graph.connect_midi(chain_flood, chain_fwd));
    REQUIRE(plugin_chain_overflow_graph.connect_midi(chain_fwd, chain_out));
    REQUIRE(plugin_chain_overflow_graph.prepare(48000.0, 32));
    plugin_chain_overflow_graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer chain_arrived;
    REQUIRE_FALSE(plugin_chain_overflow_graph.extract_midi(chain_out, chain_arrived));
    REQUIRE(chain_arrived.size() == event_capacity);
    plugin_chain_overflow_graph.release();

    pulp::midi::MidiBuffer two_events;
    two_events.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    two_events.add(pulp::midi::MidiEvent::note_on(0, 61, 100));
    REQUIRE(graph.inject_midi(mi, two_events));
    graph.process(ov, iv, 32);
    pulp::midi::MidiBuffer limited_extract;
    limited_extract.reserve(1);
    limited_extract.set_realtime_capacity_limit(true);
    REQUIRE_FALSE(graph.extract_midi(mo, limited_extract));
    REQUIRE(limited_extract.size() == 1);
    CHECK(limited_extract[0].note() == 60);
    REQUIRE(limited_extract.dropped_event_count() == 1);

    pulp::midi::MidiBuffer resumed_extract;
    REQUIRE(graph.extract_midi(mo, resumed_extract));
    REQUIRE(resumed_extract.size() == 1);
    CHECK(resumed_extract[0].note() == 61);

    graph.release();
}

TEST_CASE("SignalGraph MIDI egress resumes partial sidecar copies",
          "[host][graph][midi][mailbox][egress]") {
    SignalGraph graph;
    const auto midi_in = graph.add_midi_input_node("keys");
    const auto midi_out = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));

    float sample = 0.0f;
    const float* input_ptrs[] = {&sample};
    float* output_ptrs[] = {&sample};
    pulp::audio::BufferView<const float> in(input_ptrs, 0, 32);
    pulp::audio::BufferView<float> out(output_ptrs, 0, 32);

    pulp::midi::MidiBuffer sysex_source;
    sysex_source.add_sysex({0xF0, 0x7D, 0x01, 0xF7});
    sysex_source.add_sysex({0xF0, 0x7D, 0x02, 0xF7});
    REQUIRE(graph.inject_midi(midi_in, sysex_source));
    graph.process(out, in, 32);

    pulp::midi::MidiBuffer limited_sysex;
    limited_sysex.reserve(0, 1, 4);
    limited_sysex.set_realtime_capacity_limit(true);
    REQUIRE_FALSE(graph.extract_midi(midi_out, limited_sysex));
    REQUIRE(limited_sysex.sysex_size() == 1);
    CHECK(limited_sysex.sysex()[0].data
          == std::vector<uint8_t>{0xF0, 0x7D, 0x01, 0xF7});

    pulp::midi::MidiBuffer resumed_sysex;
    REQUIRE(graph.extract_midi(midi_out, resumed_sysex));
    REQUIRE(resumed_sysex.sysex_size() == 1);
    CHECK(resumed_sysex.sysex()[0].data
          == std::vector<uint8_t>{0xF0, 0x7D, 0x02, 0xF7});

    pulp::midi::MidiBuffer ump_source;
    pulp::midi::UmpBuffer source_ump;
    source_ump.add(pulp::midi::UmpPacket::note_on_2(0, 2, 67, 0x8000));
    source_ump.add(pulp::midi::UmpPacket::note_on_2(0, 2, 68, 0x8000));
    ump_source.attach_ump(&source_ump);
    REQUIRE(graph.inject_midi(midi_in, ump_source));
    graph.process(out, in, 32);

    pulp::midi::MidiBuffer limited_ump_extract;
    pulp::midi::UmpBuffer limited_ump;
    const auto ump_capacity = limited_ump.capacity();
    for (std::size_t i = 0; i + 1 < ump_capacity; ++i) {
        REQUIRE(limited_ump.add(
            pulp::midi::UmpPacket::note_on_2(0, 1, 1, 0x4000)));
    }
    limited_ump.set_realtime_capacity_limit(true);
    limited_ump_extract.attach_ump(&limited_ump);
    REQUIRE_FALSE(graph.extract_midi(midi_out, limited_ump_extract));
    REQUIRE(limited_ump.size() == ump_capacity);
    CHECK(limited_ump[ump_capacity - 1].packet.note_number() == 67);

    pulp::midi::MidiBuffer resumed_ump_extract;
    pulp::midi::UmpBuffer resumed_ump;
    resumed_ump_extract.attach_ump(&resumed_ump);
    REQUIRE(graph.extract_midi(midi_out, resumed_ump_extract));
    REQUIRE(resumed_ump.size() == 1);
    CHECK(resumed_ump[0].packet.note_number() == 68);

    graph.release();
}

TEST_CASE("SignalGraph MIDI injection and extraction require a live node runtime",
          "[host][graph][midi]") {
    SignalGraph graph;
    auto midi_in = graph.add_midi_input_node("keys");
    auto midi_out = graph.add_midi_output_node("thru");

    pulp::midi::MidiBuffer events;
    events.add(pulp::midi::MidiEvent::note_on(0, 64, 100));

    pulp::midi::MidiBuffer out;
    REQUIRE_FALSE(graph.inject_midi(midi_in, events));
    REQUIRE_FALSE(graph.extract_midi(midi_out, out));
    REQUIRE(out.empty());

    REQUIRE(graph.prepare(48000.0, 16));
    REQUIRE_FALSE(graph.inject_midi(999, events));
    REQUIRE_FALSE(graph.extract_midi(999, out));
    REQUIRE(out.empty());

    REQUIRE(graph.inject_midi(midi_in, events));
    graph.release();

    REQUIRE_FALSE(graph.inject_midi(midi_in, events));
    REQUIRE_FALSE(graph.extract_midi(midi_out, out));
    REQUIRE(out.empty());
}

TEST_CASE("SignalGraph MIDI ingress and egress mailboxes allocate only at prepare",
          "[host][graph][midi][rt-safety]") {
    SignalGraph graph;
    auto midi_in = graph.add_midi_input_node("keys");
    auto midi_out = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));

    float in_sample = 0.f, out_sample = 0.f;
    const float* in_ptrs[1] = {&in_sample};
    float* out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> iv(in_ptrs, 0, 32);
    pulp::audio::BufferView<float> ov(out_ptrs, 0, 32);

    pulp::midi::MidiBuffer events;
    events.reserve(1);
    events.add(pulp::midi::MidiEvent::note_on(0, 64, 100));

    pulp::midi::MidiBuffer out;
    out.reserve(1);

    pulp::test::RtAllocationProbe probe;
    REQUIRE(graph.inject_midi(midi_in, events));
    graph.process(ov, iv, 32);
    REQUIRE(graph.extract_midi(midi_out, out));

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(probe.allocated_bytes() == 0);
    REQUIRE(out.size() == 1);
}

TEST_CASE("SignalGraph MIDI ingress is latest wins and consumed once",
          "[host][graph][midi][mailbox]") {
    SignalGraph graph;
    const auto midi_in = graph.add_midi_input_node("keys");
    const auto midi_out = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));

    pulp::midi::MidiBuffer first;
    first.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    pulp::midi::MidiBuffer latest;
    latest.add(pulp::midi::MidiEvent::note_on(0, 67, 100));
    REQUIRE(graph.inject_midi(midi_in, first));
    REQUIRE(graph.inject_midi(midi_in, latest));

    float in_sample = 0.f, out_sample = 0.f;
    const float* in_ptrs[1] = {&in_sample};
    float* out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> in(in_ptrs, 0, 32);
    pulp::audio::BufferView<float> out(out_ptrs, 0, 32);
    graph.process(out, in, 32);

    pulp::midi::MidiBuffer arrived;
    REQUIRE(graph.extract_midi(midi_out, arrived));
    REQUIRE(arrived.size() == 1);
    CHECK(arrived[0].note() == 67);

    graph.process(out, in, 32);
    arrived.clear();
    REQUIRE(graph.extract_midi(midi_out, arrived));
    CHECK(arrived.empty());
}

TEST_CASE("SignalGraph MIDI ingress survives a gap free swap",
          "[host][graph][midi][mailbox][live-swap]") {
    for (const int mode : {0, 1, 2}) {
        INFO("routing mode " << mode);
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "input");
        auto slot = std::make_unique<MidiForwarder>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(
            std::move(slot), 1, 1, "forwarder");
        const auto gain = graph.add_gain_node("inserted");
        const auto output = graph.add_output_node(1, "output");
        const auto midi_in = graph.add_midi_input_node("keys");
        REQUIRE(graph.connect(input, 0, plugin, 0));
        REQUIRE(graph.connect(plugin, 0, output, 0));
        REQUIRE(graph.connect_midi(midi_in, plugin));
        graph.set_canonical_executor_routing_enabled(mode != 0);
        graph.set_parallel_routing_enabled(mode == 2);
        graph.set_parallel_min_work_units(0);
        REQUIRE(graph.prepare(48000.0, 32));

        pulp::midi::MidiBuffer stale;
        stale.add(pulp::midi::MidiEvent::note_on(0, 126, 100));
        REQUIRE(graph.inject_midi(midi_in, stale));

        constexpr std::size_t event_capacity = ParameterEventQueue::kCapacity;
        pulp::midi::MidiBuffer overflow;
        for (std::size_t index = 0; index <= event_capacity; ++index) {
            auto event = pulp::midi::MidiEvent::note_on(
                0, static_cast<int>(index % 127), 100);
            event.sample_offset = static_cast<int32_t>(index % 32);
            overflow.add(event);
        }
        REQUIRE_FALSE(graph.inject_midi(midi_in, overflow));

        // Publish the replacement snapshot between injection and process(). The
        // stable MidiInput must carry the latest retained prefix exactly once.
        graph.begin_swap_edit();
        REQUIRE(graph.disconnect(plugin, 0, output, 0));
        REQUIRE(graph.connect(plugin, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));
        REQUIRE(graph.prepare_swap(48000.0, 32)
                == SignalGraph::SwapResult::Swapped);

        std::array<float, 32> source{};
        const float* input_ptrs[] = {source.data()};
        std::array<float, 32> rendered{};
        float* output_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(output_ptrs, 1, rendered.size());
        const auto stats_before = graph.routing_executor_stats();
        graph.process(out, in, 32);

        REQUIRE(probe->last_seen().size() == event_capacity);
        CHECK(probe->last_seen()[0].note() == 0);
        const auto stats_after = graph.routing_executor_stats();
        if (mode == 0) {
            CHECK(stats_after.blocks_processed == stats_before.blocks_processed);
        } else {
            CHECK(stats_after.blocks_processed > stats_before.blocks_processed);
        }
        if (mode == 2) {
            CHECK(stats_after.parallel_levels_dispatched
                  > stats_before.parallel_levels_dispatched);
        }
        CHECK(graph.routed_walk_fallbacks() == 0);

        graph.process(out, in, 32);
        CHECK(probe->last_seen().empty());
    }
}

TEST_CASE("SignalGraph preserves pending MIDI egress by rejecting a live swap",
          "[host][graph][midi][mailbox][live-swap][egress]") {
    for (const int mode : {0, 1, 2}) {
        INFO("routing mode " << mode);
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "input");
        const auto plugin = graph.add_plugin_node(
            std::make_unique<MidiForwarder>(), 1, 1, "forwarder");
        const auto output = graph.add_output_node(1, "output");
        const auto midi_in = graph.add_midi_input_node("keys");
        const auto midi_out = graph.add_midi_output_node("thru");
        REQUIRE(graph.connect(input, 0, plugin, 0));
        REQUIRE(graph.connect(plugin, 0, output, 0));
        REQUIRE(graph.connect_midi(midi_in, plugin));
        REQUIRE(graph.connect_midi(plugin, midi_out));
        graph.set_canonical_executor_routing_enabled(mode != 0);
        graph.set_parallel_routing_enabled(mode == 2);
        graph.set_parallel_min_work_units(0);
        REQUIRE(graph.prepare(48000.0, 32));

        pulp::midi::MidiBuffer events;
        events.add(pulp::midi::MidiEvent::note_off(0, 60, 0));
        REQUIRE(graph.inject_midi(midi_in, events));
        std::array<float, 32> source{};
        const float* input_ptrs[] = {source.data()};
        std::array<float, 32> rendered{};
        float* output_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(output_ptrs, 1, rendered.size());
        graph.process(out, in, 32);

        graph.begin_swap_edit();
        REQUIRE(graph.remove_node(midi_out));
        REQUIRE(graph.prepare_swap(48000.0, 32)
                == SignalGraph::SwapResult::NeedsEagerPrepare);

        // The old output-bearing snapshot remains live until eager prepare.
        // An empty block must not overwrite the pending note-off, and a later
        // nonempty block must queue behind it rather than replacing it.
        graph.process(out, in, 32);
        events.clear();
        events.add(pulp::midi::MidiEvent::note_on(0, 67, 100));
        REQUIRE(graph.inject_midi(midi_in, events));
        graph.process(out, in, 32);

        pulp::midi::MidiBuffer arrived;
        REQUIRE(graph.extract_midi(midi_out, arrived));
        REQUIRE(arrived.size() == 2);
        CHECK(arrived[0].is_note_off());
        CHECK(arrived[0].note() == 60);
        CHECK(arrived[1].is_note_on());
        CHECK(arrived[1].note() == 67);

        // The requested eager replacement adopts the output-node removal only
        // after old-snapshot egress was drained. The retired NodeId must not
        // retain a pointer into that snapshot.
        REQUIRE(graph.prepare(48000.0, 32));
        arrived.clear();
        REQUIRE_FALSE(graph.extract_midi(midi_out, arrived));
        CHECK(arrived.empty());
    }
}

TEST_CASE("SignalGraph MIDI egress queue retains earliest blocks on overflow",
          "[host][graph][midi][mailbox][egress][overflow]") {
    for (const int mode : {0, 1, 2}) {
        INFO("routing mode " << mode);
        SignalGraph graph;
        const auto midi_in = graph.add_midi_input_node("keys");
        const auto midi_out = graph.add_midi_output_node("thru");
        REQUIRE(graph.connect_midi(midi_in, midi_out));
        graph.set_canonical_executor_routing_enabled(mode != 0);
        graph.set_parallel_routing_enabled(mode == 2);
        graph.set_parallel_min_work_units(0);
        REQUIRE(graph.prepare(48000.0, 32));

        float sample = 0.0f;
        const float* input_ptrs[] = {&sample};
        float* output_ptrs[] = {&sample};
        pulp::audio::BufferView<const float> in(input_ptrs, 0, 32);
        pulp::audio::BufferView<float> out(output_ptrs, 0, 32);
        for (int block = 0; block < 5; ++block) {
            pulp::midi::MidiBuffer events;
            events.add(pulp::midi::MidiEvent::note_off(0, 60 + block, 0));
            REQUIRE(graph.inject_midi(midi_in, events));
            graph.process(out, in, 32);
        }

        pulp::midi::MidiBuffer limited;
        limited.reserve(1);
        limited.set_realtime_capacity_limit(true);
        REQUIRE_FALSE(graph.extract_midi(midi_out, limited));
        REQUIRE(limited.size() == 1);
        CHECK(limited[0].is_note_off());
        CHECK(limited[0].note() == 60);

        pulp::midi::MidiBuffer resumed;
        REQUIRE_FALSE(graph.extract_midi(midi_out, resumed));
        REQUIRE(resumed.size() == 3);
        for (int index = 0; index < 3; ++index) {
            CHECK(resumed[static_cast<std::size_t>(index)].is_note_off());
            CHECK(resumed[static_cast<std::size_t>(index)].note() == 61 + index);
        }

        resumed.clear();
        REQUIRE(graph.extract_midi(midi_out, resumed));
        CHECK(resumed.empty());
    }
}

TEST_CASE("SignalGraph MIDI ingress sequence skips zero after wrap",
          "[host][graph][midi][mailbox][sequence-wrap]") {
    SignalGraph graph;
    const auto midi_in = graph.add_midi_input_node("keys");
    const auto midi_out = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));
    REQUIRE(graph.seed_midi_input_sequence_for_test(
        midi_in, std::numeric_limits<std::uint64_t>::max() - 1));

    float sample = 0.f;
    const float* input_ptrs[] = {&sample};
    float* output_ptrs[] = {&sample};
    pulp::audio::BufferView<const float> in(input_ptrs, 0, 32);
    pulp::audio::BufferView<float> out(output_ptrs, 0, 32);
    pulp::midi::MidiBuffer arrived;

    const std::array expected_sequences{
        std::numeric_limits<std::uint64_t>::max(), std::uint64_t{1}};
    for (std::size_t index = 0; index < expected_sequences.size(); ++index) {
        const int note = 61 + static_cast<int>(index);
        pulp::midi::MidiBuffer publication;
        publication.add(pulp::midi::MidiEvent::note_on(0, note, 100));
        REQUIRE(graph.inject_midi(midi_in, publication));
        CHECK(graph.midi_input_sequence_for_test(midi_in)
              == expected_sequences[index]);
        graph.process(out, in, 32);
        arrived.clear();
        REQUIRE(graph.extract_midi(midi_out, arrived));
        REQUIRE(arrived.size() == 1);
        CHECK(arrived[0].note() == note);

        graph.process(out, in, 32);
        arrived.clear();
        REQUIRE(graph.extract_midi(midi_out, arrived));
        CHECK(arrived.empty());
    }
}

TEST_CASE("SignalGraph audio-thread MIDI ingress is Race-free during swaps",
          "[host][graph][midi][mailbox][live-swap][race][tsan]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto plugin = graph.add_plugin_node(
        std::make_unique<MidiForwarder>(), 1, 1, "forwarder");
    const auto gain = graph.add_gain_node("inserted");
    const auto output = graph.add_output_node(1, "output");
    const auto midi_in = graph.add_midi_input_node("keys");
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.connect_midi(midi_in, plugin));
    REQUIRE(graph.prepare(48000.0, 32));

    std::atomic<bool> stop{false};
    std::atomic<bool> inject_failed{false};
    std::atomic<std::uint64_t> processed_blocks{0};
    std::thread audio_thread([&] {
        std::array<float, 32> source{};
        const float* input_ptrs[] = {source.data()};
        std::array<float, 32> rendered{};
        float* output_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(output_ptrs, 1, rendered.size());
        pulp::midi::MidiBuffer events;
        events.add(pulp::midi::MidiEvent::note_on(0, 60, 100));

        while (!stop.load(std::memory_order_acquire)) {
            if (!graph.inject_midi(midi_in, events)) {
                inject_failed.store(true, std::memory_order_relaxed);
            }
            graph.process(out, in, 32);
            processed_blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    while (processed_blocks.load(std::memory_order_relaxed) < 8) {
        std::this_thread::yield();
    }

    bool all_swaps_succeeded = true;
    for (int iteration = 0; iteration < 32; ++iteration) {
        graph.begin_swap_edit();
        if ((iteration % 2) == 0) {
            all_swaps_succeeded = graph.disconnect(plugin, 0, output, 0) &&
                graph.connect(plugin, 0, gain, 0) &&
                graph.connect(gain, 0, output, 0) && all_swaps_succeeded;
        } else {
            all_swaps_succeeded = graph.disconnect(plugin, 0, gain, 0) &&
                graph.disconnect(gain, 0, output, 0) &&
                graph.connect(plugin, 0, output, 0) && all_swaps_succeeded;
        }
        all_swaps_succeeded =
            graph.prepare_swap(48000.0, 32) == SignalGraph::SwapResult::Swapped &&
            all_swaps_succeeded;
    }

    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(all_swaps_succeeded);
    REQUIRE_FALSE(inject_failed.load(std::memory_order_relaxed));
    REQUIRE(processed_blocks.load(std::memory_order_relaxed) > 8);
}

TEST_CASE("SignalGraph MIDI ingress and egress mailboxes are Race-free",
          "[host][graph][midi][race][tsan]") {
    SignalGraph graph;
    auto midi_in = graph.add_midi_input_node("keys");
    auto midi_out = graph.add_midi_output_node("thru");
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));

    float in_sample = 0.f, out_sample = 0.f;
    const float* in_ptrs[1] = {&in_sample};
    float* out_ptrs[1] = {&out_sample};
    pulp::audio::BufferView<const float> iv(in_ptrs, 0, 32);
    pulp::audio::BufferView<float> ov(out_ptrs, 0, 32);

    std::atomic<bool> stop{false};
    std::atomic<bool> updates_active{false};
    std::atomic<int> processed_blocks{0};
    std::atomic<int> blocks_during_updates{0};
    std::thread audio_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.process(ov, iv, 32);
            processed_blocks.fetch_add(1, std::memory_order_relaxed);
            if (updates_active.load(std::memory_order_acquire)) {
                blocks_during_updates.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    bool all_injects_succeeded = true;
    bool saw_invalid_output = false;
    constexpr int kMinMidiUpdates = 10000;
    constexpr int kMinBlocksDuringUpdates = 4;
    constexpr int kMaxMidiUpdates = 1000000;
    int update_count = 0;
    updates_active.store(true, std::memory_order_release);
    while ((update_count < kMinMidiUpdates
            || blocks_during_updates.load(std::memory_order_relaxed)
                < kMinBlocksDuringUpdates)
           && update_count < kMaxMidiUpdates) {
        pulp::midi::MidiBuffer events;
        auto ev = pulp::midi::MidiEvent::note_on(
            0,
            60 + (update_count % 12),
            96);
        ev.sample_offset = update_count % 32;
        events.add(ev);
        all_injects_succeeded =
            graph.inject_midi(midi_in, events) && all_injects_succeeded;

        pulp::midi::MidiBuffer out;
        static_cast<void>(graph.extract_midi(midi_out, out));
        if (out.size() > 4 || out.sysex_size() != 0) {
            saw_invalid_output = true;
        }
        for (const auto& output_event : out) {
            if (output_event.note() < 60 || output_event.note() > 71
                || output_event.sample_offset < 0
                || output_event.sample_offset >= 32) {
                saw_invalid_output = true;
            }
        }
        if ((update_count % 64) == 0) std::this_thread::yield();
        ++update_count;
    }
    updates_active.store(false, std::memory_order_release);

    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(all_injects_succeeded);
    REQUIRE(processed_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(blocks_during_updates.load(std::memory_order_relaxed)
            >= kMinBlocksDuringUpdates);
    REQUIRE_FALSE(saw_invalid_output);
    graph.release();
}

TEST_CASE("SignalGraph live controls and MIDI handoff are TSan-clean together",
          "[host][graph][threading][race][tsan][midi]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "audio-in");
    auto gain = graph.add_gain_node("live-gain");
    auto output = graph.add_output_node(1, "audio-out");
    auto midi_in = graph.add_midi_input_node("keys");
    auto midi_out = graph.add_midi_output_node("midi-out");

    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.connect_midi(midi_in, midi_out));
    REQUIRE(graph.prepare(48000.0, 32));

    std::vector<float> input_buffer(32, 1.0f);
    std::vector<float> output_buffer(32, 0.0f);
    const float* input_ptrs[1] = {input_buffer.data()};
    float* output_ptrs[1] = {output_buffer.data()};
    pulp::audio::BufferView<const float> input_view(input_ptrs, 1, 32);
    pulp::audio::BufferView<float> output_view(output_ptrs, 1, 32);

    std::atomic<bool> stop{false};
    std::atomic<bool> updates_active{false};
    std::atomic<bool> saw_invalid_audio{false};
    std::atomic<int> processed_blocks{0};
    std::atomic<int> blocks_during_updates{0};
    std::promise<void> audio_started;
    auto started = audio_started.get_future();
    std::promise<void> first_block_processed;
    auto first_block = first_block_processed.get_future();

    std::thread audio_thread([&] {
        audio_started.set_value();
        bool first_block_signalled = false;
        while (!stop.load(std::memory_order_acquire)) {
            graph.process(output_view, input_view, 32);
            processed_blocks.fetch_add(1, std::memory_order_relaxed);
            if (updates_active.load(std::memory_order_acquire)) {
                blocks_during_updates.fetch_add(1, std::memory_order_relaxed);
            }
            for (float sample : output_buffer) {
                if (!std::isfinite(sample) || sample < -1.0e-6f
                    || sample > 1.0f + 1.0e-6f) {
                    saw_invalid_audio.store(true, std::memory_order_relaxed);
                    break;
                }
            }
            if (!first_block_signalled) {
                first_block_processed.set_value();
                first_block_signalled = true;
            }
        }
    });

    started.wait();
    first_block.wait();

    bool all_gain_updates_succeeded = true;
    bool all_injects_succeeded = true;
    bool saw_invalid_midi = false;
    constexpr int kMinControlUpdates = 10000;
    constexpr int kMinBlocksDuringUpdates = 4;
    constexpr int kMaxControlUpdates = 1000000;
    int update_count = 0;
    updates_active.store(true, std::memory_order_release);
    while ((update_count < kMinControlUpdates
            || blocks_during_updates.load(std::memory_order_relaxed)
                < kMinBlocksDuringUpdates)
           && update_count < kMaxControlUpdates) {
        const float live_gain =
            static_cast<float>((update_count % 17) + 1) / 17.0f;
        all_gain_updates_succeeded =
            graph.set_node_gain(gain, live_gain) && all_gain_updates_succeeded;

        pulp::midi::MidiBuffer events;
        auto event = pulp::midi::MidiEvent::note_on(
            0,
            60 + (update_count % 12),
            96);
        event.sample_offset = update_count % 32;
        events.add(event);
        all_injects_succeeded =
            graph.inject_midi(midi_in, events) && all_injects_succeeded;

        pulp::midi::MidiBuffer extracted;
        static_cast<void>(graph.extract_midi(midi_out, extracted));
        if (extracted.size() > 4 || extracted.sysex_size() != 0) {
            saw_invalid_midi = true;
        }
        for (const auto& output_event : extracted) {
            if (output_event.note() < 60 || output_event.note() > 71
                || output_event.sample_offset < 0
                || output_event.sample_offset >= 32) {
                saw_invalid_midi = true;
            }
        }

        if ((update_count % 64) == 0) std::this_thread::yield();
        ++update_count;
    }
    updates_active.store(false, std::memory_order_release);

    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(all_gain_updates_succeeded);
    REQUIRE(all_injects_succeeded);
    REQUIRE(processed_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(blocks_during_updates.load(std::memory_order_relaxed)
            >= kMinBlocksDuringUpdates);
    REQUIRE_FALSE(saw_invalid_audio.load(std::memory_order_relaxed));
    REQUIRE_FALSE(saw_invalid_midi);
    graph.release();
}

// ── connect_automation tests ─────────────────────────────────────────────

namespace {
// Plugin exposing a single automatable param and recording every
// ParameterEvent it receives. Audio output is silence.
class MockAutomatable final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 42;

    explicit MockAutomatable(ParamRate rate = ParamRate::ControlRate,
                             bool stepped = false,
                             bool automatable = true,
                             bool read_only = false,
                             float min_value = 0.0f,
                             float max_value = 1.0f,
                             bool modulatable = true)
        : rate_(rate),
          stepped_(stepped),
          automatable_(automatable),
          read_only_(read_only),
          min_value_(min_value),
          max_value_(max_value),
          modulatable_(modulatable) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        for (const auto& e : pe) received_.push_back(e);
        for (size_t c = 0; c < out.num_channels(); ++c)
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "mod";
        p.min_value = min_value_;
        p.max_value = max_value_;
        p.default_value = min_value_;
        p.flags.automatable = automatable_;
        p.flags.read_only = read_only_;
        p.flags.stepped = stepped_;
        p.flags.modulatable = modulatable_;
        p.rate = rate_;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    const std::vector<pulp::host::ParameterEvent>& received() const { return received_; }

private:
    PluginInfo info_ = make_plugin_info("MockAuto", 0, 1);
    ParamRate rate_ = ParamRate::ControlRate;
    bool stepped_ = false;
    bool automatable_ = true;
    bool read_only_ = false;
    float min_value_ = 0.0f;
    float max_value_ = 1.0f;
    bool modulatable_ = true;
    std::vector<pulp::host::ParameterEvent> received_;
};

class FixedStorageAutomatable final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 77;

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override {
        received_count_ = 0;
        return true;
    }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        received_count_ = 0;
        for (const auto& e : pe) {
            if (received_count_ < received_.size())
                received_[received_count_++] = e;
        }
        for (size_t c = 0; c < out.num_channels(); ++c)
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "audio-rate";
        p.min_value = -1.0f;
        p.max_value = 1.0f;
        p.default_value = 0.0f;
        p.flags.automatable = true;
        p.rate = ParamRate::AudioRate;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    std::size_t received_count() const { return received_count_; }

private:
    PluginInfo info_ = make_plugin_info("FixedStorageAuto", 0, 1);
    std::array<pulp::host::ParameterEvent, 16> received_{};
    std::size_t received_count_ = 0;
};

class FixedAutomationProbe final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 42;

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        received_count_ = 0;
        for (const auto& e : pe) {
            if (received_count_ < received_.size()) {
                received_[received_count_] = e;
                ++received_count_;
            }
        }
        for (size_t c = 0; c < out.num_channels(); ++c)
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "mod";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 0.0f;
        p.flags.automatable = true;
        p.rate = ParamRate::AudioRate;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    void clear_received() { received_count_ = 0; }
    size_t received_count() const { return received_count_; }
    const pulp::host::ParameterEvent& received(size_t index) const {
        return received_[index];
    }

private:
    PluginInfo info_ = make_plugin_info("FixedAutomationProbe", 1, 1);
    std::array<pulp::host::ParameterEvent, 16> received_{};
    size_t received_count_ = 0;
};

class ParameterMailboxProbe final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 42;

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        received_count_ = 0;
        overflowed_ = pe.overflowed();
        dropped_ = pe.dropped_event_count();
        for (const auto& event : pe) {
            if (received_count_ < received_.size()) {
                received_[received_count_++] = event;
            }
        }
        for (size_t c = 0; c < out.num_channels(); ++c) {
            std::memset(out.channel_ptr(c), 0, sizeof(float) * (size_t)n);
        }
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "mailbox";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 0.0f;
        p.flags.automatable = true;
        p.flags.modulatable = true;
        p.rate = ParamRate::AudioRate;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    std::size_t received_count() const { return received_count_; }
    const pulp::host::ParameterEvent& received(std::size_t index) const {
        return received_[index];
    }
    bool overflowed() const { return overflowed_; }
    std::uint32_t dropped() const { return dropped_; }

private:
    PluginInfo info_ = make_plugin_info("ParameterMailboxProbe", 1, 1);
    std::array<pulp::host::ParameterEvent,
               pulp::host::ParameterEventQueue::kCapacity> received_{};
    std::size_t received_count_ = 0;
    bool overflowed_ = false;
    std::uint32_t dropped_ = 0;
};
} // namespace

TEST_CASE("SignalGraph parameter event mailbox is TSan clean",
          "[host][graph][parameter][threading][race][tsan]") {
    SignalGraph graph;
    auto slot = std::make_unique<ParameterMailboxProbe>();
    const auto plugin = graph.add_plugin_node(std::move(slot), 0, 1, "probe");
    REQUIRE(graph.prepare(48000.0, 32));

    std::array<float, 32> output{};
    float* output_ptrs[] = {output.data()};
    pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());
    pulp::audio::BufferView<const float> in;

    std::atomic<bool> stop{false};
    std::atomic<bool> updates_active{false};
    std::atomic<int> blocks_during_updates{0};
    std::thread audio_thread([&] {
        while (!stop.load(std::memory_order_acquire)) {
            graph.process(out, in, 32);
            if (updates_active.load(std::memory_order_acquire)) {
                blocks_during_updates.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    bool all_injects_succeeded = true;
    constexpr int kMinUpdates = 10000;
    constexpr int kMinBlocksDuringUpdates = 4;
    constexpr int kMaxUpdates = 1000000;
    int update_count = 0;
    updates_active.store(true, std::memory_order_release);
    while ((update_count < kMinUpdates
            || blocks_during_updates.load(std::memory_order_relaxed)
                < kMinBlocksDuringUpdates)
           && update_count < kMaxUpdates) {
        pulp::host::ParameterEventQueue events;
        const bool pushed = events.push({
            ParameterMailboxProbe::kParamId,
            update_count % 32,
            static_cast<float>(update_count % 101) / 100.0f,
            0,
        });
        all_injects_succeeded =
            pushed && graph.inject_parameter_events(plugin, events)
            && all_injects_succeeded;
        if ((update_count % 64) == 0) std::this_thread::yield();
        ++update_count;
    }
    updates_active.store(false, std::memory_order_release);

    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(all_injects_succeeded);
    REQUIRE(blocks_during_updates.load(std::memory_order_relaxed)
            >= kMinBlocksDuringUpdates);
    graph.release();
}

TEST_CASE("SignalGraph parameter event mailbox is one shot and latest wins") {
    for (const int mode : {0, 1, 2}) {
        INFO("routing mode " << mode);
        SignalGraph graph;
        auto slot = std::make_unique<MockAutomatable>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 0, 1, "probe");
        graph.set_canonical_executor_routing_enabled(mode != 0);
        graph.set_parallel_routing_enabled(mode == 2);

        pulp::host::ParameterEventQueue first;
        REQUIRE(first.push({42, 1, 0.1f, 0}));
        REQUIRE_FALSE(graph.inject_parameter_events(plugin, first));
        REQUIRE(graph.prepare(48000.0, 16));

        pulp::host::ParameterEventQueue older;
        REQUIRE(older.push({42, 2, 0.2f, 0}));
        pulp::host::ParameterEventQueue latest;
        REQUIRE(latest.push({42, 9, 0.9f, 0}));
        REQUIRE(latest.push({42, 3, 0.3f, 0}));
        REQUIRE(graph.inject_parameter_events(plugin, older));
        REQUIRE(graph.inject_parameter_events(plugin, latest));

        std::array<float, 16> output{};
        float* output_ptrs[] = {output.data()};
        pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());
        pulp::audio::BufferView<const float> in;
        graph.process(out, in, 16);

        REQUIRE(probe->received().size() == 2);
        CHECK(probe->received()[0].sample_offset == 3);
        CHECK(probe->received()[0].value == 0.3f);
        CHECK(probe->received()[1].sample_offset == 9);
        CHECK(probe->received()[1].value == 0.9f);

        graph.process(out, in, 16);
        CHECK(probe->received().size() == 2);
        graph.release();
        REQUIRE_FALSE(graph.inject_parameter_events(plugin, latest));
    }
}

TEST_CASE("SignalGraph parameter event mailbox merges after graph automation") {
    for (const bool routed : {false, true}) {
        INFO("routed " << routed);
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "source");
        auto slot = std::make_unique<MockAutomatable>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 0, 1, "probe");
        REQUIRE(graph.connect_automation(input, 0, plugin, 42, 0.0f, 1.0f));
        graph.set_canonical_executor_routing_enabled(routed);
        REQUIRE(graph.prepare(48000.0, 8));

        pulp::host::ParameterEventQueue injected;
        REQUIRE(injected.push({42, 0, 0.7f, 0}));
        REQUIRE(injected.push({42, 3, 0.4f, 0}));
        REQUIRE(graph.inject_parameter_events(plugin, injected));

        std::array<float, 8> source{};
        source.front() = 0.1f;
        source.back() = 0.9f;
        const float* input_ptrs[] = {source.data()};
        std::array<float, 8> output{};
        float* output_ptrs[] = {output.data()};
        pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());
        graph.process(out, in, 8);

        REQUIRE(probe->received().size() == 4);
        CHECK(probe->received()[0].sample_offset == 0);
        CHECK(probe->received()[0].value == 0.1f);
        CHECK(probe->received()[1].sample_offset == 0);
        CHECK(probe->received()[1].value == 0.7f);
        CHECK(probe->received()[2].sample_offset == 3);
        CHECK(probe->received()[2].value == 0.4f);
        CHECK(probe->received()[3].sample_offset == 7);
        CHECK(probe->received()[3].value == 0.9f);
    }
}

TEST_CASE("SignalGraph parameter event mailbox preserves automation on overflow") {
    for (const int mode : {0, 1, 2}) {
        INFO("routing mode " << mode);
        SignalGraph graph;
        const auto input = graph.add_input_node(1, "source");
        auto slot = std::make_unique<ParameterMailboxProbe>();
        auto* probe = slot.get();
        const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1, "probe");
        REQUIRE(graph.connect_audio_rate_modulation(
            input, 0, plugin, ParameterMailboxProbe::kParamId, 0.0f, 1.0f));
        graph.set_canonical_executor_routing_enabled(mode != 0);
        graph.set_parallel_routing_enabled(mode == 2);
        REQUIRE(graph.prepare(48000.0, 8));

        pulp::host::ParameterEventQueue injected;
        for (std::size_t i = 0; i < injected.capacity(); ++i) {
            REQUIRE(injected.push({999, static_cast<int32_t>(i % 8), 0.5f, 0}));
        }
        REQUIRE(graph.inject_parameter_events(plugin, injected));

        std::array<float, 8> source{};
        const float* input_ptrs[] = {source.data()};
        std::array<float, 8> output{};
        float* output_ptrs[] = {output.data()};
        pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());
        graph.process(out, in, 8);

        REQUIRE(probe->received_count() == injected.capacity());
        REQUIRE(probe->overflowed());
        CHECK(probe->dropped() == 8);
        std::size_t automation_count = 0;
        std::size_t injected_count = 0;
        for (std::size_t i = 0; i < probe->received_count(); ++i) {
            if (probe->received(i).param_id == ParameterMailboxProbe::kParamId) {
                ++automation_count;
            } else if (probe->received(i).param_id == 999) {
                ++injected_count;
            }
        }
        CHECK(automation_count == 8);
        CHECK(injected_count == injected.capacity() - 8);

        graph.process(out, in, 8);
        CHECK(probe->received_count() == 8);
        CHECK_FALSE(probe->overflowed());
    }
}

TEST_CASE("SignalGraph parameter event mailbox allocates only at prepare") {
    SignalGraph graph;
    auto slot = std::make_unique<ParameterMailboxProbe>();
    auto* probe = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1, "probe");
    REQUIRE(graph.prepare(48000.0, 8));

    pulp::host::ParameterEventQueue injected;
    REQUIRE(injected.push({42, 4, 0.75f, 0}));
    std::array<float, 8> source{};
    const float* input_ptrs[] = {source.data()};
    std::array<float, 8> output{};
    float* output_ptrs[] = {output.data()};
    pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());

    pulp::test::RtAllocationProbe allocation_probe;
    REQUIRE(graph.inject_parameter_events(plugin, injected));
    graph.process(out, in, 8);
    CHECK(allocation_probe.allocation_count() == 0);
    CHECK(allocation_probe.allocated_bytes() == 0);
    REQUIRE(probe->received_count() == 1);
    CHECK(probe->received(0).sample_offset == 4);
}

TEST_CASE("SignalGraph parameter event mailbox survives a gap free swap") {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    auto slot = std::make_unique<MockAutomatable>();
    auto* probe = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1, "probe");
    const auto gain = graph.add_gain_node("gain");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));

    pulp::host::ParameterEventQueue injected;
    REQUIRE(injected.push({42, 5, 0.625f, 0}));
    REQUIRE(graph.inject_parameter_events(plugin, injected));

    graph.begin_swap_edit();
    REQUIRE(graph.disconnect(plugin, 0, output, 0));
    REQUIRE(graph.connect(plugin, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.prepare_swap(48000.0, 8) == SignalGraph::SwapResult::Swapped);

    std::array<float, 8> source{};
    const float* input_ptrs[] = {source.data()};
    std::array<float, 8> rendered{};
    float* output_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(output_ptrs, 1, rendered.size());
    graph.process(out, in, 8);

    REQUIRE(probe->received().size() == 1);
    CHECK(probe->received()[0].sample_offset == 5);
    CHECK(probe->received()[0].value == 0.625f);
}

TEST_CASE("SignalGraph parameter event mailbox reports a truncated source") {
    SignalGraph graph;
    auto slot = std::make_unique<ParameterMailboxProbe>();
    auto* probe = slot.get();
    const auto plugin = graph.add_plugin_node(std::move(slot), 1, 1, "probe");
    REQUIRE(graph.prepare(48000.0, 8));

    pulp::host::ParameterEventQueue injected;
    for (std::size_t i = 0; i < injected.capacity(); ++i) {
        REQUIRE(injected.push({42, static_cast<int32_t>(i % 8), 0.5f, 0}));
    }
    REQUIRE_FALSE(injected.push({42, 0, 1.0f, 0}));
    REQUIRE_FALSE(graph.inject_parameter_events(plugin, injected));

    std::array<float, 8> source{};
    const float* input_ptrs[] = {source.data()};
    std::array<float, 8> output{};
    float* output_ptrs[] = {output.data()};
    pulp::audio::BufferView<const float> in(input_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(output_ptrs, 1, output.size());
    graph.process(out, in, 8);
    CHECK(probe->received_count() == injected.capacity());
}

TEST_CASE("SignalGraph connect_automation delivers two-point events per block",
          "[host][graph][automation]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot    = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug    = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    // Source is audio port 0 of the AudioInput; target is the plugin's
    // kParamId with range [0, 1] (unity mapping so we can read values
    // directly from the input sample).
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48000.0, 64));

    std::vector<float> in_l(64, 0.0f);
    in_l[0]  = 0.25f;
    in_l[63] = 0.75f;
    std::vector<float> out_l(64, 0.0f);
    const float* in_ptrs[1]  = {in_l.data()};
    float*       out_ptrs[1] = {out_l.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 64);

    graph.process(ov, iv, 64);

    const auto& ev = slot_ptr->received();
    REQUIRE(ev.size() == 2);
    REQUIRE(ev[0].param_id == MockAutomatable::kParamId);
    REQUIRE(ev[0].sample_offset == 0);
    REQUIRE(std::abs(ev[0].value - 0.25f) < 1e-6f);
    REQUIRE(ev[1].sample_offset == 63);
    REQUIRE(std::abs(ev[1].value - 0.75f) < 1e-6f);
}

TEST_CASE("SignalGraph sparse automation remains bounded at large host blocks",
          "[host][graph][automation][capacity]") {
    constexpr int block_size = 4096;

    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48000.0, block_size));

    std::vector<float> input_samples(block_size, 0.0f);
    input_samples.front() = 0.125f;
    input_samples.back() = 0.875f;
    std::vector<float> output_samples(block_size, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, block_size);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, block_size);

    graph.process(out_view, in_view, block_size);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].param_id == MockAutomatable::kParamId);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE_THAT(events[0].value, WithinAbs(0.125f, 1e-6f));
    REQUIRE(events[1].param_id == MockAutomatable::kParamId);
    REQUIRE(events[1].sample_offset == block_size - 1);
    REQUIRE_THAT(events[1].value, WithinAbs(0.875f, 1e-6f));
}

TEST_CASE("SignalGraph sparse automation stays source-block relative under PDC",
          "[host][graph][automation][pdc]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto latency = graph.add_plugin_node(
        std::make_unique<MockLatencyPlugin>(2, 1), 1, 1, "latency");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto target = graph.add_plugin_node(std::move(slot), 1, 1, "target");
    auto out_node = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in_node, 0, latency, 0));
    REQUIRE(graph.connect(latency, 0, target, 0));
    REQUIRE(graph.connect_automation(
        in_node, 0, target, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.connect(target, 0, out_node, 0));

    REQUIRE(graph.prepare(48000.0, 4));
    REQUIRE(graph.node_latency_samples(target) == 2);
    REQUIRE(graph.latency_samples() == 2);

    std::vector<float> input_samples{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> output_samples(4, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].param_id == MockAutomatable::kParamId);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE_THAT(events[0].value, WithinAbs(1.0f, 1e-6f));
    REQUIRE(events[1].param_id == MockAutomatable::kParamId);
    REQUIRE(events[1].sample_offset == 3);
    REQUIRE_THAT(events[1].value, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("SignalGraph connect_automation rejects duplicate Replace edges",
          "[host][graph][automation]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1);
    auto plug    = graph.add_plugin_node(std::make_unique<MockAutomatable>(), 0, 1);
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    // Second Replace -> reject.
    REQUIRE_FALSE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.5f, 1.0f));
    // Add-mode alongside Replace is allowed.
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        0.0f, pulp::host::AutomationMix::Add));
}

TEST_CASE("SignalGraph connect_automation rejects invalid endpoints and params",
          "[host][graph][automation]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1);
    auto out_node = graph.add_output_node(1);
    auto plug = graph.add_plugin_node(std::make_unique<MockAutomatable>(), 0, 1);

    REQUIRE_FALSE(graph.connect_automation(
        999, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        in_node, 0, 999, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        in_node, 0, out_node, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        in_node, 2, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId + 1, 0.0f, 1.0f));

    REQUIRE(graph.connections().empty());
}

TEST_CASE("SignalGraph automation rejects non-writable params and cycle edges",
          "[host][graph][automation]") {
    SignalGraph graph;
    auto source = graph.add_input_node(1, "source");
    auto passthrough = graph.add_gain_node("passthrough");
    auto read_only = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::ControlRate, false, true, true),
        0, 1, "read-only");
    auto non_automatable = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::ControlRate, false, false),
        0, 1, "not-automatable");
    auto unresolved = graph.add_unresolved_plugin_node(
        make_plugin_info("missing", 0, 1),
        0, 1, "missing");
    auto target = graph.add_plugin_node(std::make_unique<MockAutomatable>(), 1, 1,
                                        "target");

    REQUIRE_FALSE(graph.connect_automation(
        source, 0, read_only, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        source, 0, non_automatable, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_automation(
        source, 0, unresolved, MockAutomatable::kParamId, 0.0f, 1.0f));

    REQUIRE(graph.connect(target, 0, passthrough, 0));
    REQUIRE_FALSE(graph.connect_automation(
        passthrough, 0, target, MockAutomatable::kParamId, 0.0f, 1.0f));

    REQUIRE(graph.connections().size() == 1);
}

TEST_CASE("SignalGraph automation clamps add-mode and stored smoothing",
          "[host][graph][automation]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        -25.0f, AutomationMix::Add));
    REQUIRE(graph.connections().size() == 1);
    REQUIRE(graph.connections().front().automation_smoothing_ms == 0.0f);
    REQUIRE(graph.connections().front().automation_mix == AutomationMix::Add);

    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        0.0f, AutomationMix::Add));
    REQUIRE(graph.connections().size() == 2);

    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples(4, 0.0f);
    input_samples[0] = 0.8f;
    input_samples[3] = 0.7f;
    std::vector<float> output_samples(4, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].param_id == MockAutomatable::kParamId);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[0].value == 1.0f);
    REQUIRE(events[1].sample_offset == 3);
    REQUIRE(events[1].value == 1.0f);
}

TEST_CASE("SignalGraph connect_audio_rate_modulation gates on audio-rate params",
          "[host][graph][automation][audio-rate]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto control = graph.add_plugin_node(std::make_unique<MockAutomatable>(),
                                         0, 1, "control");
    auto stepped = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate, true),
        0, 1, "stepped");
    auto audio = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate),
        0, 1, "audio-rate");

    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 0, control, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 0, stepped, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 4, audio, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId + 1, 0.0f, 1.0f));

    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId, -1.0f, 1.0f,
        -2.0f, AutomationMix::Replace));
    REQUIRE(graph.connections().size() == 1);

    const auto& edge = graph.connections().front();
    REQUIRE(edge.audio_rate_modulation);
    REQUIRE_FALSE(edge.automation);
    REQUIRE(edge.automation_param_id == MockAutomatable::kParamId);
    REQUIRE(edge.automation_range_lo == -1.0f);
    REQUIRE(edge.automation_range_hi == 1.0f);
    REQUIRE(edge.automation_smoothing_ms == 0.0f);
    REQUIRE(edge.automation_mix == AutomationMix::Replace);

    pulp::state::ModulationLane lane;
    REQUIRE(graph.audio_rate_modulation_lane(edge, lane));
    REQUIRE(lane.source.id == in_node);
    REQUIRE(lane.source.scope == pulp::state::ModulationScope::GraphNode);
    REQUIRE(lane.source.rate == pulp::state::ModulationRate::Audio);
    REQUIRE(lane.target.param_id == MockAutomatable::kParamId);
    REQUIRE(lane.target.scope == pulp::state::ModulationScope::GraphNode);
    REQUIRE(lane.target.param_rate == ParamRate::AudioRate);
    REQUIRE(lane.target.modulatable);
    REQUIRE(lane.target.writable);
    REQUIRE(lane.mix == pulp::state::ModulationMixMode::Replace);
    REQUIRE_THAT(lane.depth, WithinAbs(2.0f, 1e-6f));
    REQUIRE(pulp::state::validate_modulation_lane(lane).accepted);

    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId, 0.0f, 1.0f,
        0.0f, AutomationMix::Add));
}

TEST_CASE("SignalGraph audio-rate modulation rejects non-writable params and cycle edges",
          "[host][graph][automation][audio-rate]") {
    SignalGraph graph;
    auto source = graph.add_input_node(1, "source");
    auto passthrough = graph.add_gain_node("passthrough");
    auto read_only = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate, false, true, true),
        0, 1, "read-only");
    auto not_automatable = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate, false, false),
        0, 1, "not-automatable");
    auto not_modulatable = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(
            ParamRate::AudioRate, false, true, false, 0.0f, 1.0f, false),
        0, 1, "not-modulatable");
    auto unresolved = graph.add_unresolved_plugin_node(
        make_plugin_info("missing", 0, 1),
        0, 1, "missing");
    auto target = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate),
        1, 1, "target");

    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        999, 0, target, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        source, 0, 999, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        source, 0, read_only, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        source, 0, not_automatable, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        source, 0, not_modulatable, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        source, 0, unresolved, MockAutomatable::kParamId, 0.0f, 1.0f));

    REQUIRE(graph.connect(target, 0, passthrough, 0));
    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        passthrough, 0, target, MockAutomatable::kParamId, 0.0f, 1.0f));

    REQUIRE(graph.connections().size() == 1);
}

TEST_CASE("SignalGraph audio-rate modulation delivers one event per sample",
          "[host][graph][automation][audio-rate]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>(ParamRate::AudioRate);
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "audio-rate");

    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, MockAutomatable::kParamId, -1.0f, 1.0f));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples{0.0f, 0.25f, 0.5f, 1.0f};
    std::vector<float> output_samples(4, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 4);
    const float expected[] = {-1.0f, -0.5f, 0.0f, 1.0f};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(events[static_cast<size_t>(i)].param_id == MockAutomatable::kParamId);
        REQUIRE(events[static_cast<size_t>(i)].sample_offset == i);
        REQUIRE(std::abs(events[static_cast<size_t>(i)].value - expected[i]) < 1e-6f);
    }
}

TEST_CASE("SignalGraph audio-rate modulation validates large host-block capacity",
          "[host][graph][automation][audio-rate][capacity]") {
    SignalGraph max_capacity_graph;
    auto max_capacity_input = max_capacity_graph.add_input_node(1, "in");
    auto max_capacity_slot = std::make_unique<MockAutomatable>(ParamRate::AudioRate);
    auto* max_capacity_slot_ptr = max_capacity_slot.get();
    auto max_capacity_plugin = max_capacity_graph.add_plugin_node(
        std::move(max_capacity_slot), 0, 1, "audio-rate");

    REQUIRE(max_capacity_graph.connect_audio_rate_modulation(
        max_capacity_input, 0, max_capacity_plugin, MockAutomatable::kParamId,
        -1.0f, 1.0f));
    REQUIRE(max_capacity_graph.prepare(
        48000.0, static_cast<int>(ParameterEventQueue::kCapacity)));

    std::vector<float> input_samples(ParameterEventQueue::kCapacity, 0.0f);
    input_samples.front() = 0.0f;
    input_samples.back() = 1.0f;
    std::vector<float> output_samples(ParameterEventQueue::kCapacity, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(
        in_ptrs, 1, static_cast<int>(ParameterEventQueue::kCapacity));
    pulp::audio::BufferView<float> out_view(
        out_ptrs, 1, static_cast<int>(ParameterEventQueue::kCapacity));

    max_capacity_graph.process(
        out_view, in_view, static_cast<int>(ParameterEventQueue::kCapacity));
    const auto& events = max_capacity_slot_ptr->received();
    REQUIRE(events.size() == ParameterEventQueue::kCapacity);
    REQUIRE(events.front().sample_offset == 0);
    REQUIRE_THAT(events.front().value, WithinAbs(-1.0f, 1e-6f));
    REQUIRE(events.back().sample_offset ==
            static_cast<int32_t>(ParameterEventQueue::kCapacity - 1));
    REQUIRE_THAT(events.back().value, WithinAbs(1.0f, 1e-6f));

    auto rejects_large_audio_rate_block = [](int block_size) {
        SignalGraph graph;
        auto in_node = graph.add_input_node(1, "in");
        auto plug = graph.add_plugin_node(
            std::make_unique<MockAutomatable>(ParamRate::AudioRate),
            0, 1, "audio-rate");
        REQUIRE(graph.connect_audio_rate_modulation(
            in_node, 0, plug, MockAutomatable::kParamId, -1.0f, 1.0f));
        REQUIRE_FALSE(graph.prepare(48000.0, block_size));
    };

    rejects_large_audio_rate_block(2048);
    rejects_large_audio_rate_block(4096);
}

TEST_CASE("SignalGraph audio-rate add modulation clamps independent of edge order",
          "[host][graph][automation][audio-rate]") {
    auto run = [](bool reverse_order) {
        SignalGraph graph;
        auto in_node = graph.add_input_node(1, "in");
        auto slot = std::make_unique<MockAutomatable>(ParamRate::AudioRate);
        auto* slot_ptr = slot.get();
        auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "audio-rate");

        auto add_negative_edge = [&] {
            REQUIRE(graph.connect_audio_rate_modulation(
                in_node, 0, plug, MockAutomatable::kParamId, -1.0f, 0.0f,
                0.0f, AutomationMix::Add));
        };
        auto add_positive_edge = [&] {
            REQUIRE(graph.connect_audio_rate_modulation(
                in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 0.25f,
                0.0f, AutomationMix::Add));
        };
        if (reverse_order) {
            add_positive_edge();
            add_negative_edge();
        } else {
            add_negative_edge();
            add_positive_edge();
        }

        REQUIRE(graph.prepare(48000.0, 1));
        std::vector<float> input_samples{1.0f};
        std::vector<float> output_samples(1, 0.0f);
        const float* in_ptrs[1] = {input_samples.data()};
        float* out_ptrs[1] = {output_samples.data()};
        pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 1);
        pulp::audio::BufferView<float> out_view(out_ptrs, 1, 1);

        graph.process(out_view, in_view, 1);

        const auto& events = slot_ptr->received();
        REQUIRE(events.size() == 1);
        return events[0].value;
    };

    REQUIRE(std::abs(run(false) - 0.25f) < 1e-6f);
    REQUIRE(std::abs(run(true) - 0.25f) < 1e-6f);
}

TEST_CASE("SignalGraph audio-rate replace and add modulation clamp to parameter bounds",
          "[host][graph][automation][audio-rate]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>(
        ParamRate::AudioRate, false, true, false, -0.5f, 0.5f);
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "audio-rate");

    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, MockAutomatable::kParamId, -0.25f, 0.25f,
        0.0f, AutomationMix::Replace));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.4f, 0.4f,
        0.0f, AutomationMix::Add));
    REQUIRE(graph.prepare(48000.0, 3));

    std::vector<float> input_samples{0.0f, 0.5f, 1.0f};
    std::vector<float> output_samples(3, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 3);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 3);

    graph.process(out_view, in_view, 3);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 3);
    REQUIRE_THAT(events[0].value, WithinAbs(0.15f, 1e-6f));
    REQUIRE_THAT(events[1].value, WithinAbs(0.4f, 1e-6f));
    REQUIRE_THAT(events[2].value, WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("SignalGraph plugin automation path is allocation-free after prepare",
          "[host][graph][automation][rt-safety]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<FixedStorageAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "audio-rate");

    REQUIRE(graph.connect_automation(
        in_node, 0, plug, FixedStorageAutomatable::kParamId, -1.0f, 1.0f));
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, FixedStorageAutomatable::kParamId, 0.1f, 0.2f,
        0.0f, AutomationMix::Add));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, FixedStorageAutomatable::kParamId, -0.5f, 0.5f));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, FixedStorageAutomatable::kParamId, 0.05f, 0.05f,
        0.0f, AutomationMix::Add));
    REQUIRE(graph.prepare(48000.0, 4));

    std::array<float, 4> input_samples{0.0f, 0.25f, 0.5f, 1.0f};
    std::array<float, 4> output_samples{};
    std::array<const float*, 1> in_ptrs{input_samples.data()};
    std::array<float*, 1> out_ptrs{output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(), 4);
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(), 4);

    pulp::test::RtAllocationProbe probe;
    graph.process(out_view, in_view, 4);

    REQUIRE(probe.allocation_count() == 0);
    REQUIRE(probe.allocated_bytes() == 0);
    REQUIRE(slot_ptr->received_count() == 6);
}

TEST_CASE("SignalGraph audio-rate modulation composes with PDC",
          "[host][graph][automation][audio-rate][pdc]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto latency = graph.add_plugin_node(
        std::make_unique<MockLatencyPlugin>(2, 1), 1, 1, "latency");
    auto target_slot = std::make_unique<MockAutomatable>(ParamRate::AudioRate);
    auto* target_ptr = target_slot.get();
    auto target = graph.add_plugin_node(std::move(target_slot), 1, 1, "target");
    auto out_node = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in_node, 0, latency, 0));
    REQUIRE(graph.connect(latency, 0, target, 0));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, target, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.connect(target, 0, out_node, 0));

    REQUIRE(graph.prepare(48000.0, 4));
    REQUIRE(graph.node_latency_samples(target) == 2);
    REQUIRE(graph.latency_samples() == 2);

    std::vector<float> input_samples{1.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> output_samples(4, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);

    const auto& events = target_ptr->received();
    REQUIRE(events.size() == 4);
    const float expected[] = {0.0f, 0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 4; ++i) {
        REQUIRE(events[static_cast<size_t>(i)].param_id == MockAutomatable::kParamId);
        REQUIRE(events[static_cast<size_t>(i)].sample_offset == i);
        REQUIRE(std::abs(events[static_cast<size_t>(i)].value - expected[i]) < 1e-6f);
    }
}

TEST_CASE("SignalGraph audio-rate modulation fails closed when event capacity is exceeded",
          "[host][graph][automation][audio-rate]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto plug = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate),
        0, 1, "audio-rate");

    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE_FALSE(graph.prepare(48000.0,
                                static_cast<int>(ParameterEventQueue::kCapacity + 1)));
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("SignalGraph plugin automation process uses prepared scratch without allocation",
          "[host][graph][automation][rt-safety]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<FixedAutomationProbe>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 1, 1, "fixed-auto");
    auto out_node = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in_node, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out_node, 0));
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, FixedAutomationProbe::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, plug, FixedAutomationProbe::kParamId, 0.0f, 1.0f,
        0.0f, AutomationMix::Add));
    REQUIRE(graph.prepare(48000.0, 4));

    std::vector<float> input_samples{0.25f, 0.5f, 0.75f, 1.0f};
    std::vector<float> output_samples(4, 0.0f);
    const float* in_ptrs[1] = {input_samples.data()};
    float* out_ptrs[1] = {output_samples.data()};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);

    graph.process(out_view, in_view, 4);
    slot_ptr->clear_received();

    {
        pulp::native_components::test::RtNoAllocScope no_alloc;
        graph.process(out_view, in_view, 4);
    }

    REQUIRE(slot_ptr->received_count() == 6);
    for (size_t i = 0; i < slot_ptr->received_count(); ++i) {
        REQUIRE(slot_ptr->received(i).param_id == FixedAutomationProbe::kParamId);
    }
}
#endif

// ── Item 4.6 automation smoothing ──────────────────────────────────────

TEST_CASE("SignalGraph automation smoothing slews step input over the declared time",
          "[host][graph][automation][smoothing]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    // 100 ms slew across a [0, 1] range at 48 kHz = 4800 samples to
    // traverse the full range. A 32-sample block can move at most
    // 32 / 4800 = 0.00667 per block, so a step from 0 → 1 takes many
    // blocks to land.
    const double sr = 48000.0;
    const int block = 32;
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        100.0f /*ms*/, AutomationMix::Replace));
    REQUIRE(graph.prepare(sr, block));

    std::vector<float> in_buf(block, 1.0f);  // step to 1 immediately
    std::vector<float> out_buf(block, 0.0f);
    const float* in_ptrs[1] = {in_buf.data()};
    float* out_ptrs[1] = {out_buf.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, block);
    pulp::audio::BufferView<float> ov(out_ptrs, 1, block);

    // Block 0: first block primes to source value (snap on first
    // encounter), so v0 = 1.0 here. We assert that on the SECOND
    // block — which represents a *new* step away from the previous
    // post-slew value — the slew limits how far we can move.
    graph.process(ov, iv, block);
    REQUIRE(slot_ptr->received().size() == 2);

    // Now step the source back to 0.0. The slew rate caps the per-
    // sample delta at ~1/4800 ≈ 0.000208, so over `last = 31` samples
    // we can only move ~0.00646 from the previous 1.0 — meaning vN
    // should sit near 0.9935, NOT 0.0.
    std::fill(in_buf.begin(), in_buf.end(), 0.0f);
    graph.process(ov, iv, block);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 4);
    // Event indexes 2 (v0) and 3 (vN) come from block 1.
    // v0 was forced down by one sample of slew from 1.0 (≈ 0.999792).
    REQUIRE(events[2].sample_offset == 0);
    REQUIRE(events[2].value < 1.0f);  // moved down
    REQUIRE(events[2].value > 0.95f); // but only by one sample's slew

    // vN should still be near 1.0 (we can only move 31 samples-worth
    // toward 0 in this block — far short of reaching it).
    REQUIRE(events[3].sample_offset == block - 1);
    REQUIRE(events[3].value > 0.99f);  // still far from 0
    REQUIRE(events[3].value < events[2].value); // monotonically decreasing
}

TEST_CASE("SignalGraph automation smoothing is bypassed when smoothing_ms == 0",
          "[host][graph][automation][smoothing]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        0.0f, AutomationMix::Replace));
    REQUIRE(graph.prepare(48000.0, 8));

    // Step from 0 to 1 in one block — without smoothing, vN should
    // land at exactly 1.0.
    std::vector<float> in_buf(8, 0.0f);
    in_buf[0] = 0.0f;
    in_buf[7] = 1.0f;
    std::vector<float> out_buf(8, 0.0f);
    const float* in_ptrs[1] = {in_buf.data()};
    float* out_ptrs[1] = {out_buf.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 8);
    pulp::audio::BufferView<float> ov(out_ptrs, 1, 8);

    graph.process(ov, iv, 8);

    const auto& events = slot_ptr->received();
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].value == 0.0f);
    REQUIRE(events[1].value == 1.0f);  // step delivered with no slew
}

TEST_CASE("SignalGraph automation smoothing eventually reaches the target",
          "[host][graph][automation][smoothing]") {
    SignalGraph graph;
    auto in_node = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockAutomatable>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 0, 1, "auto");

    // 10 ms slew over [0,1] at 48 kHz = 480 samples. With a 64-sample
    // block, the source rises by ~0.133 per block. ~8 blocks to land.
    REQUIRE(graph.connect_automation(
        in_node, 0, plug, MockAutomatable::kParamId, 0.0f, 1.0f,
        10.0f, AutomationMix::Replace));
    REQUIRE(graph.prepare(48000.0, 64));

    std::vector<float> in_buf(64, 1.0f);
    std::vector<float> out_buf(64, 0.0f);
    const float* in_ptrs[1] = {in_buf.data()};
    float* out_ptrs[1] = {out_buf.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
    pulp::audio::BufferView<float> ov(out_ptrs, 1, 64);

    // Block 0 primes to source value (snap) per the documented contract.
    // After that, every step from 0 → 1 has to slew — but here the
    // source is held at 1.0 continuously, so we should reach the target
    // exactly in block 0 (snap-on-prime).
    graph.process(ov, iv, 64);
    REQUIRE_THAT(slot_ptr->received().back().value, WithinAbs(1.0f, 1e-6f));

    // Now step the source back to 0 and watch the slew take ~8 blocks
    // to reach it. We assert the value monotonically decreases and
    // eventually crosses below epsilon.
    std::fill(in_buf.begin(), in_buf.end(), 0.0f);
    float last_value = 1.0f;
    bool reached = false;
    for (int b = 0; b < 32; ++b) {
        graph.process(ov, iv, 64);
        const float v = slot_ptr->received().back().value;
        REQUIRE(v <= last_value + 1e-6f);  // non-increasing
        last_value = v;
        if (v < 1e-3f) { reached = true; break; }
    }
    REQUIRE(reached);
}

namespace {
class ProcessBufferAwareSlot final : public PluginSlot {
public:
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int n) override {
        ++legacy_process_calls;
        for (std::size_t c = 0; c < out.num_channels(); ++c) {
            float* dst = out.channel_ptr(c);
            const float* src = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int i = 0; i < n; ++i) {
                dst[i] = src ? src[i] : 0.0f;
            }
        }
    }

    void process(pulp::format::ProcessBuffers& audio,
                 const pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& param_events,
                 int n) override {
        ++process_buffer_calls;
        saw_layout_ok = audio.layouts_match_descriptors();
        saw_storage_ok = audio.active_buses_have_storage();
        saw_input_count = audio.inputs.active_count();
        saw_output_count = audio.outputs.active_count();
        saw_main_input_channels =
            audio.main_input() ? audio.main_input()->num_channels() : 0;
        saw_main_output_channels =
            audio.main_output() ? audio.main_output()->num_channels() : 0;

        auto* out = audio.main_output();
        pulp::audio::BufferView<const float> empty;
        auto* in = audio.main_input();
        if (out) {
            process(*out, in ? *in : empty, midi_in, midi_out, param_events, n);
        }
    }

    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    int legacy_process_calls = 0;
    int process_buffer_calls = 0;
    bool saw_layout_ok = false;
    bool saw_storage_ok = false;
    std::size_t saw_input_count = 0;
    std::size_t saw_output_count = 0;
    std::size_t saw_main_input_channels = 0;
    std::size_t saw_main_output_channels = 0;

private:
    PluginInfo info_ = make_plugin_info("ProcessBufferAware", 2, 2);
};
} // namespace

TEST_CASE("SignalGraph dispatches plugin nodes through ProcessBuffers",
          "[host][graph][process-buffers]") {
    SignalGraph graph;
    auto in = graph.add_input_node(2, "in");
    auto slot = std::make_unique<ProcessBufferAwareSlot>();
    auto* slot_ptr = slot.get();
    auto plugin = graph.add_plugin_node(std::move(slot), 2, 2, "slot");
    auto out = graph.add_output_node(2, "out");

    REQUIRE(graph.connect(in, 0, plugin, 0));
    REQUIRE(graph.connect(in, 1, plugin, 1));
    REQUIRE(graph.connect(plugin, 0, out, 0));
    REQUIRE(graph.connect(plugin, 1, out, 1));
    REQUIRE(graph.prepare(48000.0, 8));

    std::vector<float> in_l(8, 0.25f);
    std::vector<float> in_r(8, 0.75f);
    std::vector<float> out_l(8, 0.0f);
    std::vector<float> out_r(8, 0.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> input(in_ptrs, 2, 8);
    pulp::audio::BufferView<float> output(out_ptrs, 2, 8);

    graph.process(output, input, 8);

    REQUIRE(slot_ptr->process_buffer_calls == 1);
    REQUIRE(slot_ptr->legacy_process_calls == 1);
    REQUIRE(slot_ptr->saw_layout_ok);
    REQUIRE(slot_ptr->saw_storage_ok);
    REQUIRE(slot_ptr->saw_input_count == 1);
    REQUIRE(slot_ptr->saw_output_count == 1);
    REQUIRE(slot_ptr->saw_main_input_channels == 2);
    REQUIRE(slot_ptr->saw_main_output_channels == 2);
    for (int i = 0; i < 8; ++i) {
        REQUIRE_THAT(out_l[static_cast<std::size_t>(i)], WithinAbs(0.25f, 1e-6f));
        REQUIRE_THAT(out_r[static_cast<std::size_t>(i)], WithinAbs(0.75f, 1e-6f));
    }
}

namespace {
class MultiOutputProcessBufferSlot final : public PluginSlot {
public:
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int) override {
        ++legacy_process_calls;
    }

    void process(pulp::format::ProcessBuffers& audio,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int n) override {
        ++process_buffer_calls;
        saw_output_channels = audio.main_output()
            ? audio.main_output()->num_channels()
            : 0;
        auto* out = audio.main_output();
        if (!out) return;

        for (std::size_t c = 0; c < out->num_channels(); ++c) {
            float* dst = out->channel_ptr(c);
            const float value = 0.25f + static_cast<float>(c);
            for (int i = 0; i < n; ++i) {
                dst[static_cast<std::size_t>(i)] = value;
            }
        }
    }

    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    int legacy_process_calls = 0;
    int process_buffer_calls = 0;
    std::size_t saw_output_channels = 0;

private:
    PluginInfo info_ = make_plugin_info("MultiOut", 0, 4);
};
} // namespace

TEST_CASE("SignalGraph routes multi-output plugin ProcessBuffers to AudioOutput",
          "[host][graph][process-buffers][multi-output]") {
    SignalGraph graph;
    auto slot = std::make_unique<MultiOutputProcessBufferSlot>();
    auto* slot_ptr = slot.get();
    auto plugin = graph.add_plugin_node(std::move(slot), 0, 4, "multi");
    auto out = graph.add_output_node(4, "out");

    REQUIRE(graph.connect(plugin, 0, out, 0));
    REQUIRE(graph.connect(plugin, 1, out, 1));
    REQUIRE(graph.connect(plugin, 2, out, 2));
    REQUIRE(graph.connect(plugin, 3, out, 3));
    REQUIRE(graph.prepare(48000.0, 8));

    std::vector<float> out0(8, 0.0f);
    std::vector<float> out1(8, 0.0f);
    std::vector<float> out2(8, 0.0f);
    std::vector<float> out3(8, 0.0f);
    float* out_ptrs[4] = {out0.data(), out1.data(), out2.data(), out3.data()};
    pulp::audio::BufferView<const float> input;
    pulp::audio::BufferView<float> output(out_ptrs, 4, 8);

    graph.process(output, input, 8);

    REQUIRE(slot_ptr->process_buffer_calls == 1);
    REQUIRE(slot_ptr->legacy_process_calls == 0);
    REQUIRE(slot_ptr->saw_output_channels == 4);
    for (int i = 0; i < 8; ++i) {
        REQUIRE_THAT(out0[static_cast<std::size_t>(i)], WithinAbs(0.25f, 1e-6f));
        REQUIRE_THAT(out1[static_cast<std::size_t>(i)], WithinAbs(1.25f, 1e-6f));
        REQUIRE_THAT(out2[static_cast<std::size_t>(i)], WithinAbs(2.25f, 1e-6f));
        REQUIRE_THAT(out3[static_cast<std::size_t>(i)], WithinAbs(3.25f, 1e-6f));
    }
}

// ── Item 4.7 sidechain routing ────────────────────────────────────────

namespace {
// A simple "compressor": passes main input through unchanged, but copies
// the average of its sidechain inputs into a public field so the test
// can observe what arrived on the sidechain bus.
class SidechainCompressor final : public PluginSlot {
public:
    int main_inputs = 2;
    float last_sidechain_value = 0.0f;
    int last_sidechain_count = 0;

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const int nc_in = static_cast<int>(in.num_channels());
        const int nc_out = static_cast<int>(out.num_channels());

        // Main input → main output (port-for-port).
        for (int c = 0; c < std::min(main_inputs, nc_out); ++c) {
            const float* s = c < nc_in ? in.channel_ptr(c) : nullptr;
            float* d = out.channel_ptr(c);
            for (int i = 0; i < n; ++i) d[i] = s ? s[i] : 0.f;
        }

        // Average everything beyond the main bus and stash it.
        float accum = 0.f;
        int count = 0;
        for (int c = main_inputs; c < nc_in; ++c) {
            const float* s = in.channel_ptr(c);
            for (int i = 0; i < n; ++i) {
                accum += s[i];
                ++count;
            }
        }
        last_sidechain_value = count > 0 ? accum / (float)count : 0.f;
        last_sidechain_count = count;
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return false; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
private:
    PluginInfo info_ = make_plugin_info("Compressor", 4, 2);
};
} // namespace

TEST_CASE("SignalGraph connect_sidechain tags the edge and routes to the named port",
          "[host][graph][sidechain]") {
    // Layout: kick (2 channels) — port 0 drives the compressor's main
    // input, port 1 drives the compressor's sidechain. The compressor
    // declares 2 input ports: 0 = main, 1 = sidechain.
    //
    // We use a single 2-channel AudioInput node because the graph
    // dispatches `input[c]` into each AudioInput node's port `c`; using
    // two 1-port AudioInput nodes would make both nodes see input[0],
    // which collapses the test signal.
    SignalGraph graph;
    auto src = graph.add_input_node(2, "src");
    auto comp_slot = std::make_unique<SidechainCompressor>();
    comp_slot->main_inputs = 1;  // port 0 = main; port 1 = sidechain
    auto* comp_ptr = comp_slot.get();
    auto comp = graph.add_plugin_node(std::move(comp_slot), 2, 2, "compressor");
    auto out = graph.add_output_node(2, "out");

    REQUIRE(graph.connect(src, 0, comp, 0));            // main from input[0]
    REQUIRE(graph.connect_sidechain(src, 1, comp, 1));  // sidechain from input[1]
    REQUIRE(graph.connect(comp, 0, out, 0));
    REQUIRE(graph.connect(comp, 1, out, 1));

    // The sidechain edge should be present and tagged.
    int sidechain_edges = 0;
    for (const auto& c : graph.connections()) {
        if (c.sidechain) ++sidechain_edges;
    }
    REQUIRE(sidechain_edges == 1);

    REQUIRE(graph.prepare(48000.0, 16));

    // input[0] = main signal (0.2), input[1] = kick (0.8).
    std::vector<float> in_sig(16, 0.2f);
    std::vector<float> in_kick(16, 0.8f);
    std::vector<float> out_l(16, 0.f), out_r(16, 0.f);
    const float* in_ptrs[2] = {in_sig.data(), in_kick.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> iv(in_ptrs, 2, 16);
    pulp::audio::BufferView<float> ov(out_ptrs, 2, 16);

    graph.process(ov, iv, 16);

    // Sidechain saw 16 frames of 0.8 → mean 0.8.
    REQUIRE_THAT(comp_ptr->last_sidechain_value, WithinAbs(0.8f, 1e-6f));
    REQUIRE(comp_ptr->last_sidechain_count == 16);

    // Main signal pass-through delivered 0.2 to out[0]; out[1] stays at
    // zero (plugin's port-2 output was untouched by the test fixture).
    for (int i = 0; i < 16; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(0.2f, 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(0.0f, 1e-6f));
    }
    graph.release();
}

TEST_CASE("SignalGraph connect_sidechain rejects non-Plugin destinations and bad ports",
          "[host][graph][sidechain]") {
    SignalGraph graph;
    auto kick = graph.add_input_node(1, "kick");
    auto gain = graph.add_gain_node("gain");
    auto comp = graph.add_plugin_node(std::make_unique<SidechainCompressor>(),
                                      4, 2, "comp");

    // Non-Plugin destination: reject.
    REQUIRE_FALSE(graph.connect_sidechain(kick, 0, gain, 0));
    // Source port out of range: reject.
    REQUIRE_FALSE(graph.connect_sidechain(kick, 4, comp, 2));
    // Dest port out of range: reject.
    REQUIRE_FALSE(graph.connect_sidechain(kick, 0, comp, 99));
    // Unknown nodes: reject.
    REQUIRE_FALSE(graph.connect_sidechain(999, 0, comp, 2));
    REQUIRE_FALSE(graph.connect_sidechain(kick, 0, 999, 2));
    REQUIRE(graph.connections().empty());

    // Valid edge accepted; duplicate then rejected.
    REQUIRE(graph.connect_sidechain(kick, 0, comp, 2));
    REQUIRE_FALSE(graph.connect_sidechain(kick, 0, comp, 2));
    REQUIRE(graph.connections().size() == 1);
}

TEST_CASE("SignalGraph sidechain edges participate in cycle detection",
          "[host][graph][sidechain]") {
    // comp -> follow (audio), follow -> comp.sidechain would close a cycle.
    SignalGraph graph;
    auto comp = graph.add_plugin_node(std::make_unique<SidechainCompressor>(),
                                      3, 2, "comp");  // ports 0=main, 1,2=sidechain
    auto follow = graph.add_plugin_node(std::make_unique<SidechainCompressor>(),
                                        2, 2, "follow");

    REQUIRE(graph.connect(comp, 0, follow, 0));
    // Back-edge via sidechain would cycle — must be rejected.
    REQUIRE_FALSE(graph.connect_sidechain(follow, 0, comp, 1));
}

TEST_CASE("SignalGraph node_loads() reports per-node CPU load after processing",
          "[host][graph][telemetry]") {
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));

    // No telemetry before prepare() builds the measurers.
    REQUIRE(graph.node_loads().empty());

    constexpr int kFrames = 64;
    REQUIRE(graph.prepare(48000.0, kFrames));

    std::vector<float> l(kFrames, 0.5f), r(kFrames, 0.5f);
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{l.data(), r.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);

    constexpr std::uint64_t kBlocks = 5;
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
        graph.process(out_view, in_view, kFrames);
    }

    const auto loads = graph.node_loads();
    REQUIRE(loads.size() == 3);  // input, gain, output
    for (const auto& report : loads) {
        // Every prepared node's work ran through its measurer each block.
        REQUIRE(report.load.callback_count == kBlocks);
        // A non-zero available-ns budget proves the sample rate reached the
        // per-node measurer (begin(num_frames, sample_rate)).
        REQUIRE(report.load.available_ns > 0);
    }

    // Re-prepare preserves the measurers (persistent across snapshots): the
    // callback_count carries over rather than resetting.
    REQUIRE(graph.prepare(48000.0, kFrames));
    graph.process(out_view, in_view, kFrames);
    for (const auto& report : graph.node_loads()) {
        REQUIRE(report.load.callback_count == kBlocks + 1);
    }

    // Removing a node drops it from node_loads() — the lingering measurer is
    // filtered out rather than reported as a phantom node.
    REQUIRE(graph.remove_node(gain));
    REQUIRE(graph.prepare(48000.0, kFrames));
    const auto after_remove = graph.node_loads();
    REQUIRE(after_remove.size() == 2);  // input + output, gain gone
    for (const auto& report : after_remove) {
        REQUIRE(report.node_id != gain);
    }
}

TEST_CASE("SignalGraph node_loads() is reported on the canonical-executor path too",
          "[host][graph][telemetry][executor]") {
    // The routed executor must attribute per-node CPU load exactly as the legacy
    // walk: every node (incl. audio I/O) reports a callback per processed block,
    // with the sample rate threaded into its measurer. This is the telemetry
    // parity that lets canonical-executor routing be the default backend.
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto gain = graph.add_gain_node("Gain");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    graph.set_canonical_executor_routing_enabled(true);  // route, not walk

    constexpr int kFrames = 64;
    REQUIRE(graph.prepare(48000.0, kFrames));

    std::vector<float> l(kFrames, 0.5f), r(kFrames, 0.5f);
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{l.data(), r.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);

    constexpr std::uint64_t kBlocks = 5;
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
        graph.process(out_view, in_view, kFrames);
    }

    const auto loads = graph.node_loads();
    REQUIRE(loads.size() == 3);  // input, gain, output — all timed by the executor
    for (const auto& report : loads) {
        REQUIRE(report.load.callback_count == kBlocks);
        REQUIRE(report.load.available_ns > 0);  // sample rate reached the measurer
    }
}

TEST_CASE("SignalGraph node_loads() is reported on the parallel-executor path",
          "[host][graph][telemetry][executor][parallel]") {
    // The per-node measurers are timed on WORKER threads in the levelized parallel
    // path. A wide level (many independent gain nodes) forces worker dispatch;
    // every node must still report exactly one callback per processed block, with
    // no lost or double counts from the concurrent begin()/end() spans.
    SignalGraph graph;
    const auto input = graph.add_input_node(2, "Input");
    const auto output = graph.add_output_node(2, "Output");
    constexpr int kWidth = 8;  // a wide parallel level
    std::vector<NodeId> gains;
    for (int i = 0; i < kWidth; ++i) {
        const auto gn = graph.add_gain_node("G");
        REQUIRE(graph.connect(input, 0, gn, 0));
        REQUIRE(graph.connect(gn, 0, output, 0));  // fan-in mix at the output
        REQUIRE(graph.set_node_gain(gn, 0.1f + 0.01f * static_cast<float>(i)));
        gains.push_back(gn);
    }
    graph.set_canonical_executor_routing_enabled(true);
    graph.set_parallel_routing_enabled(true);  // route across worker threads

    constexpr int kFrames = 64;
    REQUIRE(graph.prepare(48000.0, kFrames));

    std::vector<float> l(kFrames, 0.5f), r(kFrames, 0.5f);
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{l.data(), r.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);

    constexpr std::uint64_t kBlocks = 8;
    for (std::uint64_t i = 0; i < kBlocks; ++i) {
        graph.process(out_view, in_view, kFrames);
    }

    const auto loads = graph.node_loads();
    REQUIRE(loads.size() == static_cast<std::size_t>(kWidth + 2));  // gains + I/O
    for (const auto& report : loads) {
        REQUIRE(report.load.callback_count == kBlocks);  // exactly one per block
        REQUIRE(report.load.available_ns > 0);
    }
}

TEST_CASE("SignalGraph routed_walk_fallbacks() stays zero for routed + walk-by-choice graphs",
          "[host][graph][telemetry][fallback]") {
    // routed_walk_fallbacks() must count ONLY the silent degradation case — an
    // eligible graph whose routed dispatch failed — not the ordinary fallbacks
    // (routing disabled, or a graph that simply runs the walk by choice). This
    // pins the discrimination so the counter is a trustworthy "a graph stopped
    // routing" signal.
    constexpr int kFrames = 64;
    constexpr int kBlocks = 4;
    std::vector<float> l(kFrames, 0.4f), r(kFrames, 0.4f);
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{l.data(), r.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);

    SECTION("an eligible routed graph never silently falls back") {
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto gn = g.add_gain_node("G");
        const auto out = g.add_output_node(2, "Out");
        REQUIRE(g.connect(in, 0, gn, 0));
        REQUIRE(g.connect(in, 1, gn, 1));
        REQUIRE(g.connect(gn, 0, out, 0));
        REQUIRE(g.connect(gn, 1, out, 1));
        REQUIRE(g.set_node_gain(gn, 0.5f));
        REQUIRE(g.prepare(48000.0, kFrames));  // canonical routing ON by default
        REQUIRE(signal_graph_executor_eligible(g));
        for (int b = 0; b < kBlocks; ++b) g.process(out_view, in_view, kFrames);
        REQUIRE(g.routed_walk_fallbacks() == 0);  // routed the whole time
    }

    SECTION("running the walk by choice is not counted as a fallback") {
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto gn = g.add_gain_node("G");
        const auto out = g.add_output_node(2, "Out");
        REQUIRE(g.connect(in, 0, gn, 0));
        REQUIRE(g.connect(in, 1, gn, 1));
        REQUIRE(g.connect(gn, 0, out, 0));
        REQUIRE(g.connect(gn, 1, out, 1));
        REQUIRE(g.set_node_gain(gn, 0.5f));
        g.set_canonical_executor_routing_enabled(false);  // force the walk
        REQUIRE(g.prepare(48000.0, kFrames));
        for (int b = 0; b < kBlocks; ++b) g.process(out_view, in_view, kFrames);
        REQUIRE(g.routed_walk_fallbacks() == 0);  // walk-by-choice is normal, not a failure
    }
}

namespace {
struct PreparedEditLevel {
    float level = 1.0f;
};

CustomNodeType make_prepared_edit_level_type(
    std::string id, float level, std::atomic<int>* creates = nullptr,
    std::atomic<int>* prepares = nullptr, std::atomic<int>* destroys = nullptr,
    bool fail_create = false) {
    CustomNodeType type;
    type.type_id = std::move(id);
    type.version = 1;
    type.num_input_ports = 1;
    type.num_output_ports = 1;
    type.default_name = "Prepared edit level";
    type.create = [level, creates, fail_create]() -> void* {
        if (creates) creates->fetch_add(1, std::memory_order_relaxed);
        return fail_create ? nullptr : static_cast<void*>(new PreparedEditLevel{level});
    };
    type.destroy = [destroys](void* raw) {
        if (destroys) destroys->fetch_add(1, std::memory_order_relaxed);
        delete static_cast<PreparedEditLevel*>(raw);
    };
    type.prepare = [prepares](void*, double, int) {
        if (prepares) prepares->fetch_add(1, std::memory_order_relaxed);
    };
    type.process_instance = [](
        void* raw, pulp::audio::BufferView<float>& output,
        const pulp::audio::BufferView<const float>& input, int frames) {
        const float level = static_cast<PreparedEditLevel*>(raw)->level;
        for (int i = 0; i < frames; ++i) {
            output.channel_ptr(0)[i] = input.channel_ptr(0)[i] * level;
        }
    };
    return type;
}

class PreparedEditCountingPlugin final : public PluginSlot {
public:
    explicit PreparedEditCountingPlugin(std::atomic<int>& prepare_calls)
        : prepare_calls_(prepare_calls) {
        info_.name = "PreparedEditCountingPlugin";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override {
        prepare_calls_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        for (int i = 0; i < frames; ++i) {
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        }
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    std::atomic<int>& prepare_calls_;
    PluginInfo info_;
};
} // namespace

TEST_CASE("SignalGraph prepared edit first prepare failure is a full rollback",
          "[host][graph][prepared-edit][transaction]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("custom creation fails after a complete candidate was authored") {
        std::atomic<int> creates{0};
        const auto json_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(
            make_prepared_edit_level_type("pulp.test.prepared.fail", 2.0f,
                                          &creates, nullptr, nullptr, true)));
        const auto input = edit->add_input_node(1, "input");
        const auto custom = edit->add_custom_node("pulp.test.prepared.fail");
        const auto output = edit->add_output_node(1, "output");
        REQUIRE(input != 0);
        REQUIRE(custom != 0);
        REQUIRE(output != 0);
        REQUIRE(edit->connect(input, 0, custom, 0));
        REQUIRE(edit->connect(custom, 0, output, 0));
        edit->set_canonical_executor_routing_enabled(false);

        REQUIRE(edit->prepare(48000.0, 8)
                == Result::CustomInstanceCreateFailed);
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::CustomInstanceCreateFailed);
        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        edit.reset();

        REQUIRE(graph.nodes().empty());
        REQUIRE(graph.connections().empty());
        REQUIRE(graph.custom_node_type_count() == 0);
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(graph.canonical_executor_routing_enabled());
        REQUIRE(GraphSerializer::to_json(graph) == json_before);
        REQUIRE(graph.add_input_node(1, "first real node") == 1);
    }

    SECTION("one failed route poisons the whole candidate") {
        const auto json_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        const auto input = edit->add_input_node(1, "input");
        REQUIRE(input == 1);
        REQUIRE_FALSE(edit->connect(input, 0, 9999, 0));
        REQUIRE(edit->prepare(48000.0, 8) == Result::InvalidMutation);
        edit.reset();
        REQUIRE(graph.nodes().empty());
        REQUIRE(graph.connections().empty());
        REQUIRE(GraphSerializer::to_json(graph) == json_before);
        REQUIRE(graph.add_input_node(1, "first real node") == 1);
    }

    SECTION("a stale candidate cannot overwrite newer authoring state") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_output_node(1, "candidate output") == 1);
        REQUIRE(graph.add_input_node(1, "newer owner input") == 1);
        const auto owner_json = GraphSerializer::to_json(graph);

        REQUIRE(edit->prepare(48000.0, 8) == Result::StaleBase);
        REQUIRE(edit->commit() == Result::NotPrepared);
        REQUIRE(GraphSerializer::to_json(graph) == owner_json);
        REQUIRE(graph.nodes().size() == 1);
        REQUIRE(graph.nodes().front().name == "newer owner input");
        REQUIRE(graph.add_output_node(1, "next owner output") == 2);
    }

    SECTION("the first successful graph is installed only at commit") {
        auto edit = graph.begin_prepared_topology_edit();
        const auto input = edit->add_input_node(1, "input");
        const auto output = edit->add_output_node(1, "output");
        REQUIRE(edit->connect(input, 0, output, 0));
        edit->set_parallel_routing_enabled(true);

        REQUIRE_FALSE(edit->routed_execution_ready(8));
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->routed_execution_ready(8));
        REQUIRE_FALSE(edit->routed_execution_ready(9));
        REQUIRE(graph.nodes().empty());
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(edit->commit() == Result::Committed);
        REQUIRE(graph.nodes().size() == 2);
        REQUIRE(graph.connections().size() == 1);
        REQUIRE(graph.is_prepared());
        std::array<float, 8> source{};
        source.fill(0.25f);
        std::array<float, 8> rendered{};
        const float* source_ptrs[] = {source.data()};
        float* rendered_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
        graph.process(out, in, 8);
        REQUIRE(rendered == source);
        REQUIRE(graph.routing_executor_stats().serial_levels_run > 0);
        REQUIRE(graph.routed_walk_fallbacks() == 0);
        REQUIRE(graph.add_gain_node("next owner node") == 3);
    }
}

TEST_CASE("SignalGraph prepared edit publishes old or new topology without silence",
          "[host][graph][prepared-edit][transaction][threading]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> prepares{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.level.1", 1.0f, &creates, &prepares, &destroys)));
    const auto input = graph.add_input_node(1, "input");
    auto current = graph.add_custom_node("pulp.test.prepared.level.1");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, current, 0));
    REQUIRE(graph.connect(current, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.level.2", 2.0f, &creates, &prepares, &destroys)));
    REQUIRE(edit->remove_node(current));
    const auto replacement =
        edit->add_custom_node("pulp.test.prepared.level.2");
    REQUIRE(replacement != 0);
    REQUIRE(edit->connect(input, 0, replacement, 0));
    REQUIRE(edit->connect(replacement, 0, output, 0));
    REQUIRE(edit->prune_unused_custom_node_types() == 1);

    std::atomic<bool> stop{false};
    std::atomic<int> old_blocks{0};
    std::atomic<int> new_blocks{0};
    std::atomic<int> invalid_blocks{0};
    std::atomic<std::size_t> audio_thread_allocations{0};
    std::thread audio([&] {
        std::array<float, 16> source{};
        source.fill(1.0f);
        std::array<float, 16> rendered{};
        const float* source_ptrs[] = {source.data()};
        float* rendered_ptrs[] = {rendered.data()};
        pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
        pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
        pulp::test::RtAllocationProbe allocation_probe;
        while (!stop.load(std::memory_order_acquire)) {
            graph.process(out, in, 16);
            const float value = rendered[0];
            const bool coherent = std::all_of(
                rendered.begin(), rendered.end(),
                [value](float sample) { return sample == value; });
            if (!coherent) {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (value == 1.0f) {
                old_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (value == 2.0f) {
                new_blocks.fetch_add(1, std::memory_order_relaxed);
            } else {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        audio_thread_allocations.store(
            allocation_probe.allocation_count(), std::memory_order_relaxed);
    });

    REQUIRE(edit->prepare(48000.0, 16) == Result::Prepared);
    REQUIRE(edit->commit() == Result::Committed);
    for (int i = 0; i < 10000
                    && new_blocks.load(std::memory_order_relaxed) == 0; ++i) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    audio.join();

    REQUIRE(old_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(new_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(invalid_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(audio_thread_allocations.load(std::memory_order_relaxed) == 0);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.custom_node_type_count() == 1);
    REQUIRE(creates.load(std::memory_order_relaxed) == 2);
    REQUIRE(prepares.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("SignalGraph prepared edit changes dimensions only for lifecycle-free custom nodes",
          "[host][graph][prepared-edit][dimensions]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;
    auto type = make_prepared_edit_level_type("pulp.test.prepared.dimension", 0.5f,
                                              &creates, nullptr, &destroys);
    type.prepare = {};
    REQUIRE(graph.register_custom_node_type(std::move(type)));
    const auto input = graph.add_input_node(1, "input");
    const auto custom = graph.add_custom_node("pulp.test.prepared.dimension");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, custom, 0));
    REQUIRE(graph.connect(custom, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE(creates.load(std::memory_order_relaxed) == 1);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->prepare(44100.0, 16) == Result::Prepared);
    REQUIRE(edit->routed_execution_ready(16));
    REQUIRE_FALSE(edit->routed_execution_ready(17));
    REQUIRE(graph.prepared_max_block_size() == 8);
    REQUIRE(edit->commit() == Result::Committed);
    REQUIRE(graph.prepared_max_block_size() == 16);
    REQUIRE(creates.load(std::memory_order_relaxed) == 1);

    std::array<float, 16> source{};
    source.fill(1.0f);
    std::array<float, 16> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 16);
    REQUIRE(std::all_of(rendered.begin(), rendered.end(),
                        [](float sample) { return sample == 0.5f; }));
}

TEST_CASE("SignalGraph prepared edit surface is fail closed and releases abandoned instances",
          "[host][graph][prepared-edit][surface]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("all owned node and route wrappers stay candidate local") {
        auto edit = graph.begin_prepared_topology_edit();
        auto type = make_prepared_edit_level_type("pulp.test.prepared.surface", 1.0f);
        REQUIRE(edit->register_custom_node_type(std::move(type)));
        const auto input = edit->add_input_node(1, "input");
        const auto output = edit->add_output_node(1, "output");
        const auto gain = edit->add_gain_node("gain");
        const auto midi_input = edit->add_midi_input_node("midi input");
        const auto midi_output = edit->add_midi_output_node("midi output");
        const auto custom = edit->add_custom_node("pulp.test.prepared.surface", 1);
        const auto unresolved = edit->add_unresolved_custom_node(
            "pulp.test.prepared.missing", 1, 1, 1, "missing");
        REQUIRE(input != 0);
        REQUIRE(output != 0);
        REQUIRE(gain != 0);
        REQUIRE(custom != 0);
        REQUIRE(unresolved != 0);
        REQUIRE(edit->connect(input, 0, gain, 0));
        REQUIRE(edit->connect(gain, 0, output, 0));
        REQUIRE(edit->connect_feedback(gain, 0, gain, 0));
        REQUIRE(edit->connect_midi(midi_input, midi_output));
        REQUIRE(edit->set_node_gain(gain, 0.25f));
        edit->set_canonical_executor_routing_enabled(false);
        edit->set_parallel_routing_enabled(false);
        edit->set_anticipation_enabled(false);
        REQUIRE(edit->node(custom) != nullptr);
        REQUIRE(edit->nodes().size() == 7);
        REQUIRE(edit->connections().size() == 4);
        REQUIRE_FALSE(edit->unregister_custom_node_type(
            "pulp.test.prepared.surface", 1));
        REQUIRE(edit->prepare(48000.0, 8) == Result::InvalidMutation);
        REQUIRE(graph.nodes().empty());
    }

    SECTION("unused types can be explicitly unregistered") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(
            make_prepared_edit_level_type("pulp.test.prepared.unused", 1.0f)));
        REQUIRE(edit->custom_node_type_count() == 1);
        REQUIRE(edit->unregister_custom_node_type("pulp.test.prepared.unused", 1));
        REQUIRE(edit->custom_node_type_count() == 0);
        REQUIRE(edit->prune_unused_custom_node_types() == 0);
        REQUIRE(edit->commit() == Result::NotPrepared);
    }

    SECTION("an abandoned prepared instance is released and destroyed once") {
        std::atomic<int> creates{0};
        std::atomic<int> prepares{0};
        std::atomic<int> releases{0};
        std::atomic<int> destroys{0};
        auto type = make_prepared_edit_level_type(
            "pulp.test.prepared.abandoned", 1.0f, &creates, &prepares, &destroys);
        type.release = [&](void*) {
            releases.fetch_add(1, std::memory_order_relaxed);
        };
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->register_custom_node_type(std::move(type)));
        REQUIRE(edit->add_custom_node("pulp.test.prepared.abandoned") != 0);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->add_gain_node("too late") == 0);
        edit.reset();
        REQUIRE(creates.load(std::memory_order_relaxed) == 1);
        REQUIRE(prepares.load(std::memory_order_relaxed) == 1);
        REQUIRE(releases.load(std::memory_order_relaxed) == 1);
        REQUIRE(destroys.load(std::memory_order_relaxed) == 1);
        REQUIRE(graph.nodes().empty());
    }
}

TEST_CASE("SignalGraph prepared edit registry remains bounded under binding churn",
          "[host][graph][prepared-edit][registry]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> creates{0};
    std::atomic<int> prepares{0};
    std::atomic<int> destroys{0};
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(make_prepared_edit_level_type(
        "pulp.test.prepared.churn.0", 1.0f, &creates, &prepares, &destroys)));
    const auto input = graph.add_input_node(1, "input");
    auto current = graph.add_custom_node("pulp.test.prepared.churn.0");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, current, 0));
    REQUIRE(graph.connect(current, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));

    constexpr int kReplacements = 64;
    for (int i = 1; i <= kReplacements; ++i) {
        auto edit = graph.begin_prepared_topology_edit();
        const std::string type_id =
            "pulp.test.prepared.churn." + std::to_string(i);
        REQUIRE(edit->register_custom_node_type(make_prepared_edit_level_type(
            type_id, 1.0f + static_cast<float>(i), &creates, &prepares,
            &destroys)));
        REQUIRE(edit->remove_node(current));
        const auto next = edit->add_custom_node(type_id);
        REQUIRE(next != 0);
        REQUIRE(edit->connect(input, 0, next, 0));
        REQUIRE(edit->connect(next, 0, output, 0));
        REQUIRE(edit->prune_unused_custom_node_types() == 1);
        REQUIRE(edit->custom_node_type_count() == 1);
        REQUIRE(edit->prepare(48000.0, 8) == Result::Prepared);
        REQUIRE(edit->commit() == Result::Committed);
        REQUIRE(graph.custom_node_type_count() == 1);
        current = next;
    }
    REQUIRE(creates.load(std::memory_order_relaxed) == kReplacements + 1);
    REQUIRE(prepares.load(std::memory_order_relaxed) == kReplacements + 1);
}

TEST_CASE("SignalGraph prepared edit rejects external plugin reprepare before mutation",
          "[host][graph][prepared-edit][plugin]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    std::atomic<int> prepare_calls{0};
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto plugin = graph.add_plugin_node(
        std::make_unique<PreparedEditCountingPlugin>(prepare_calls), 1, 1,
        "plugin");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 8));
    REQUIRE(prepare_calls.load(std::memory_order_relaxed) == 1);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->add_gain_node("candidate only") != 0);
    REQUIRE(edit->prepare(48000.0, 16)
            == Result::ExternalPluginReprepareRequired);
    edit.reset();

    REQUIRE(prepare_calls.load(std::memory_order_relaxed) == 1);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.connections().size() == 2);
    REQUIRE(graph.is_prepared());
}

TEST_CASE("SignalGraph prepared edit rejects snapshot-local MIDI output before mutation",
          "[host][graph][prepared-edit][midi-output]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;

    SECTION("candidate-only MIDI output is rejected without consuming an ID") {
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->add_midi_output_node("candidate output") == 1);
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::MidiOutputSnapshotLocalRequired);
        edit.reset();
        REQUIRE(graph.nodes().empty());
        REQUIRE_FALSE(graph.is_prepared());
        REQUIRE(graph.add_input_node(1, "owner input") == 1);
    }

    SECTION("live pending note-off remains on the old snapshot and drains once") {
        const auto midi_input = graph.add_midi_input_node("midi input");
        const auto midi_output = graph.add_midi_output_node("midi output");
        REQUIRE(graph.connect_midi(midi_input, midi_output));
        REQUIRE(graph.prepare(48000.0, 8));

        pulp::midi::MidiBuffer pending;
        pending.reserve(1);
        pending.add(pulp::midi::MidiEvent::note_off(0, 64, 0));
        REQUIRE(graph.inject_midi(midi_input, pending));
        float input_sample = 0.0f;
        float output_sample = 0.0f;
        const float* input_ptrs[] = {&input_sample};
        float* output_ptrs[] = {&output_sample};
        pulp::audio::BufferView<const float> in(input_ptrs, 0, 8);
        pulp::audio::BufferView<float> out(output_ptrs, 0, 8);
        graph.process(out, in, 8);

        const auto graph_before = GraphSerializer::to_json(graph);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->remove_node(midi_output));
        REQUIRE(edit->prepare(48000.0, 8)
                == Result::MidiOutputSnapshotLocalRequired);
        edit.reset();
        REQUIRE(GraphSerializer::to_json(graph) == graph_before);
        REQUIRE(graph.nodes().size() == 2);
        REQUIRE(graph.connections().size() == 1);
        REQUIRE(graph.is_prepared());

        pulp::midi::MidiBuffer arrived;
        arrived.reserve(1);
        REQUIRE(graph.extract_midi(midi_output, arrived));
        REQUIRE(arrived.size() == 1);
        REQUIRE(arrived[0].is_note_off());
        arrived.clear();
        graph.process(out, in, 8);
        REQUIRE(graph.extract_midi(midi_output, arrived));
        REQUIRE(arrived.empty());
    }
}

TEST_CASE("SignalGraph prepared edit adopts only unchanged PDC intersection",
          "[host][graph][prepared-edit][pdc]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto latency = graph.add_plugin_node(
        std::make_unique<MockLatencyPlugin>(2, 1), 1, 1, "latency");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.connect(input, 0, latency, 0));
    REQUIRE(graph.connect(latency, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 4));
    REQUIRE(graph.latency_samples() == 2);

    std::array<float, 4> source{};
    source.fill(1.0f);
    std::array<float, 4> rendered{};
    const float* source_ptrs[] = {source.data()};
    float* rendered_ptrs[] = {rendered.data()};
    pulp::audio::BufferView<const float> in(source_ptrs, 1, source.size());
    pulp::audio::BufferView<float> out(rendered_ptrs, 1, rendered.size());
    graph.process(out, in, 4); // prime the plugin and host delay histories

    auto add = graph.begin_prepared_topology_edit();
    const auto gain = add->add_gain_node("new PDC branch");
    REQUIRE(add->connect(input, 0, gain, 0));
    REQUIRE(add->connect(gain, 0, output, 0));
    REQUIRE(add->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(add->commit() == Result::Committed);
    REQUIRE(graph.is_prepared());
    REQUIRE(graph.latency_samples() == 2);
    REQUIRE(graph.connections().size() == 5);
    graph.process(out, in, 4);
    // Existing direct-edge history and plugin state carry (2.0 throughout);
    // the newly delayed gain branch starts at zero and reaches 1.0 after two.
    REQUIRE(rendered == (std::array<float, 4>{2.0f, 2.0f, 3.0f, 3.0f}));
    auto remove = graph.begin_prepared_topology_edit();
    REQUIRE(remove->remove_node(gain));
    REQUIRE(remove->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(remove->commit() == Result::Committed);
    REQUIRE(graph.connections().size() == 3);
    graph.process(out, in, 4);
    REQUIRE(rendered == (std::array<float, 4>{2.0f, 2.0f, 2.0f, 2.0f}));

    auto reconnect = graph.begin_prepared_topology_edit();
    REQUIRE(reconnect->disconnect(input, 0, output, 0));
    REQUIRE(reconnect->connect(input, 0, output, 0)); // equal value, new identity
    REQUIRE(reconnect->prepare(48000.0, 4) == Result::Prepared);
    REQUIRE(reconnect->commit() == Result::Committed);
    graph.process(out, in, 4);
    // The equal-looking reconnected edge must not receive the retired ring.
    REQUIRE(rendered == (std::array<float, 4>{1.0f, 1.0f, 2.0f, 2.0f}));
}

TEST_CASE("SignalGraph prepared edit feedback rejection preserves live graph",
          "[host][graph][prepared-edit][feedback]") {
    using Result = SignalGraph::PreparedTopologyEdit::Result;
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "input");
    const auto gain = graph.add_gain_node("gain");
    const auto output = graph.add_output_node(1, "output");
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.connect_feedback(gain, 0, gain, 0));
    REQUIRE(graph.prepare(48000.0, 4));
    const auto connections_before = graph.connections();

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->add_gain_node("candidate only") != 0);
    REQUIRE(edit->prepare(48000.0, 4) == Result::RuntimeAdoptionFailed);
    edit.reset();
    REQUIRE(graph.connections() == connections_before);
    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.is_prepared());
}

// ── GraphSerializer round-trip ───────────────────────────────────────────
