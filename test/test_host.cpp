#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
#include <atomic>
#include <thread>

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
} // namespace

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
    PluginInfo info_{"SC", "", "", "", "", PluginFormat::CLAP};
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
    PluginInfo info_{"MidiFwd", "", "", "", "", PluginFormat::CLAP};
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

    graph.release();
}

// ── Phase 1E connect_automation test ────────────────────────────────────

namespace {
// Plugin exposing a single automatable param and recording every
// ParameterEvent it receives. Audio output is silence.
class MockAutomatable final : public PluginSlot {
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
        for (const auto& e : pe) received_.push_back(e);
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
    PluginInfo info_{"MockAuto", "", "", "", "", PluginFormat::CLAP};
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

// ── Phase 3 GraphSerializer round-trip ──────────────────────────────────

#include <pulp/host/graph_serializer.hpp>

TEST_CASE("GraphSerializer round-trips topology + connections + layout",
          "[host][serializer]") {
    SignalGraph g1;
    auto a = g1.add_input_node(2, "in");
    auto b = g1.add_gain_node("g");
    auto c = g1.add_output_node(2, "out");
    REQUIRE(g1.connect(a, 0, b, 0));
    REQUIRE(g1.connect(a, 1, b, 1));
    REQUIRE(g1.connect(b, 0, c, 0));
    REQUIRE(g1.connect(b, 1, c, 1));
    g1.set_node_gain(b, 0.75f);

    std::unordered_map<NodeId, std::pair<float,float>> layout = {
        {a, {10.f, 20.f}}, {b, {200.f, 20.f}}, {c, {400.f, 20.f}}
    };
    std::string json = GraphSerializer::to_json(g1, layout);
    REQUIRE(!json.empty());

    SignalGraph g2;
    auto result = GraphSerializer::from_json(g2, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());
    REQUIRE(g2.nodes().size() == 3);
    REQUIRE(g2.connections().size() == 4);
    REQUIRE(result.editor_layout.size() == 3);

    // The new node ids may differ from the originals; verify by name.
    NodeId in2 = 0, g2id = 0, out2 = 0;
    for (const auto& n : g2.nodes()) {
        if (n.name == "in")  in2 = n.id;
        if (n.name == "g")   g2id = n.id;
        if (n.name == "out") out2 = n.id;
    }
    REQUIRE(in2 != 0);
    REQUIRE(g2id != 0);
    REQUIRE(out2 != 0);
    REQUIRE(std::abs(g2.node_gain(g2id) - 0.75f) < 1e-6f);
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

    pulp::host::ParameterEventQueue pe;
    slot->process(out, in, mi, mo, pe, 256);

    // PulpGain is a unity-ish gain plugin; output should not be all zeros
    // after processing a non-zero input.
    float sum = 0.0f;
    for (int i = 0; i < 256; ++i) sum += std::abs(out_l[i]) + std::abs(out_r[i]);
    REQUIRE(sum > 0.0f);

    slot->release();
}
#endif

// ── Phase 0B race-stress test ─────────────────────────────────────────────
//
// Exercises the snapshot-publish semantic: one thread runs process() in a
// tight loop while another thread mutates the graph. No TSan assertion here
// (builds without TSan must pass), but with TSan enabled this run should be
// clean. The behavioral contract we assert is "no crash, no wild output":
// every sample in the audio thread's output buffer is either silence (post-
// invalidate, pre-reprepare) or a valid sample in [-1, 1].

TEST_CASE("SignalGraph snapshot publish is race-clean", "[host][graph][race]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto g   = graph.add_gain_node("g");
    auto out = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(in, 0, g, 0));
    REQUIRE(graph.connect(g,  0, out, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(graph.set_node_gain(g, 0.5f));

    std::atomic<bool> stop{false};
    std::atomic<int>  blocks{0};
    std::atomic<int>  bad_samples{0};  // REQUIREs in audio thread would throw
                                       // and take down the process; count
                                       // violations and assert on the main
                                       // thread after join.

    std::thread audio([&] {
        std::vector<float> in_l(64, 0.25f);
        std::vector<float> out_l(64, 0.0f);
        const float* in_ptrs[1]  = {in_l.data()};
        float*       out_ptrs[1] = {out_l.data()};
        pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
        pulp::audio::BufferView<float>       ov(out_ptrs, 1, 64);
        while (!stop.load(std::memory_order_relaxed)) {
            graph.process(ov, iv, 64);
            for (int i = 0; i < 64; ++i) {
                const float v = out_l[i];
                // Either silence (snapshot invalidated mid-run) or the
                // set-gain path (0.25 * 0.5 = 0.125). Anything else = race.
                if (!(v == 0.0f || v == 0.125f)) {
                    bad_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Hammer the graph with mutation cycles from the "UI" thread.
    const int cycles = 200;
    for (int i = 0; i < cycles; ++i) {
        REQUIRE(graph.disconnect(g, 0, out, 0));
        REQUIRE(graph.connect(g, 0, out, 0));
        REQUIRE(graph.prepare(48000.0, 64));
    }

    // Let the audio thread see the final prepared state for a few blocks
    // so the test ends with the snapshot published (not nullptr).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    REQUIRE(blocks.load() > 0);
    REQUIRE(bad_samples.load() == 0);
}
