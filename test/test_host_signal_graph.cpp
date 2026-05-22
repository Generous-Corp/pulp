// SignalGraph tests, split out of test_host.cpp (P11-5, #2647) to bring the
// 2,190-line parent under the 2,000-line target. Self-contained: uses
// SignalGraph from pulp/host/signal_graph.hpp (in the shared host includes) and
// carries its own interleaved helper namespaces.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;

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
          "[host][graph][coverage][phase3]") {
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

TEST_CASE("SignalGraph ignores stale audio connections with invalid ports",
          "[host][graph][routing][coverage][phase3]") {
    SignalGraph graph;
    auto input = graph.add_input_node(1, "in");
    auto gain = graph.add_gain_node("gain");
    auto output = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(input, 99, gain, 0));
    REQUIRE(graph.connect(input, 0, gain, 99));
    REQUIRE(graph.connect(gain, 42, output, 0));
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
        // behaviour can be measured end-to-end: the plugin's internal delay
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
          "[host][graph][coverage][phase3]") {
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
          "[host][graph][coverage][phase3]") {
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
          "[host][graph][coverage][phase3]") {
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
          "[host][graph][routing][coverage][phase3]") {
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
          "[host][graph][routing][coverage][phase3]") {
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
        for (const auto& ev : midi_in) {
            last_seen_.add(ev);
            midi_out.add(ev);
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
private:
    PluginInfo info_ = make_plugin_info("MidiFwd", 0, 0, "MidiEffect");
    pulp::midi::MidiBuffer last_seen_;
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

    // MidiOutput sink received the forwarded events.
    pulp::midi::MidiBuffer arrived;
    REQUIRE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.size() == 2);
    REQUIRE(arrived[0].sample_offset == 0);
    REQUIRE(arrived[1].sample_offset == 16);

    arrived.clear();
    graph.process(ov, iv, 32);
    REQUIRE(graph.extract_midi(mo, arrived));
    REQUIRE(arrived.empty());
    REQUIRE(fwd_ptr->last_seen().empty());

    graph.release();
}

TEST_CASE("SignalGraph MIDI injection and extraction require a live node runtime",
          "[host][graph][midi][coverage][phase3]") {
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

// ── Phase 1E connect_automation test ────────────────────────────────────

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
                             float max_value = 1.0f)
        : rate_(rate),
          stepped_(stepped),
          automatable_(automatable),
          read_only_(read_only),
          min_value_(min_value),
          max_value_(max_value) {}

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
    std::vector<pulp::host::ParameterEvent> received_;
};
} // namespace

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
          "[host][graph][automation][coverage][phase3]") {
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
          "[host][graph][automation][coverage][phase3]") {
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
          "[host][graph][automation][coverage][phase3]") {
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

    REQUIRE_FALSE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId, 0.0f, 1.0f));
    REQUIRE(graph.connect_audio_rate_modulation(
        in_node, 0, audio, MockAutomatable::kParamId, 0.0f, 1.0f,
        0.0f, AutomationMix::Add));
}

TEST_CASE("SignalGraph audio-rate modulation rejects non-writable params and cycle edges",
          "[host][graph][automation][audio-rate][coverage][phase3]") {
    SignalGraph graph;
    auto source = graph.add_input_node(1, "source");
    auto passthrough = graph.add_gain_node("passthrough");
    auto read_only = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate, false, true, true),
        0, 1, "read-only");
    auto not_automatable = graph.add_plugin_node(
        std::make_unique<MockAutomatable>(ParamRate::AudioRate, false, false),
        0, 1, "not-automatable");
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
          "[host][graph][automation][audio-rate][coverage][phase3]") {
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

// ── Phase 3 GraphSerializer round-trip ──────────────────────────────────
