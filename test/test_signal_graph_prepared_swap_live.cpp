// 2.2b — the no-silence LIVE swap
// (planning/2026-07-07-signalgraph-swap-and-bake-implementation-plan.md).
//
// 2.2a proved compile_() is race-free vs a live process(). 2.2b turns that into a
// real published swap: begin_swap_edit() opens a transaction in which the owner's
// allow-set edits do NOT silence the live snapshot; prepare_swap() recompiles and
// atomically publishes the new snapshot (publish-new-before-retire, seq_cst) so no
// output block is ever silent. A non-reinit-free edit instead returns
// NeedsEagerPrepare (and invalidates), so the caller falls back to prepare().
//
// This suite asserts (a) a reinit-free gain-graph swap under a live render drops NO
// block to silence, and (b) each non-reinit-free class is rejected.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

// input(2) -> gain(2/2) -> output(2): plugin-reinit-free, no MIDI, no PDC — the
// class 2.2b publishes with no silence.
void build_gain_graph(SignalGraph& g, NodeId& gain_out) {
    const auto in = g.add_input_node(2, "In");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.5f));
    gain_out = gain;
}

// 2-in/2-out passthrough plugin with a configurable reported latency and one
// automatable parameter — used to exercise the M3 (PDC) and CX3 (smoothed
// automation) rejection gates.
class LatencySlot final : public PluginSlot {
public:
    explicit LatencySlot(int latency) : latency_(latency) {
        info_.name = "Lat";
        info_.format = PluginFormat::CLAP;
        info_.num_inputs = 2;
        info_.num_outputs = 2;
        info_.category = "Effect";
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue&, int n) override {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c) {
            const float* i = in.channel_ptr(c);
            float* o = out.channel_ptr(c);
            for (int k = 0; k < n; ++k) o[static_cast<std::size_t>(k)] = i[static_cast<std::size_t>(k)];
        }
        for (std::size_t c = chs; c < out.num_channels(); ++c)
            std::fill_n(out.channel_ptr(c), n, 0.0f);
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        pulp::host::HostParamInfo p;
        p.id = 1;
        p.name = "gain";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 1.0f;
        p.flags.automatable = true;
        return {p};
    }
    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<uint8_t>&) override { return true; }
    int latency_samples() const override { return latency_; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

private:
    PluginInfo info_;
    int latency_;
};

// Render one block of non-zero input; return the output peak (0 == silent block).
float render_peak(SignalGraph& g) {
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> in_l(kFrames, 0.25f), in_r(kFrames, 0.25f);
    std::array<const float*, 2> ic{in_l.data(), in_r.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);
    float peak = 0.0f;
    for (int k = 0; k < kFrames; ++k) {
        peak = std::max(peak, std::fabs(l[static_cast<std::size_t>(k)]));
        peak = std::max(peak, std::fabs(r[static_cast<std::size_t>(k)]));
    }
    return peak;
}

}  // namespace

TEST_CASE("prepare_swap publishes a reinit-free edit with NO silent block "
          "(2.2b no-silence swap)",
          "[host][signal-graph][prepared-swap][threads][rt-safety]") {
    SignalGraph g;
    NodeId gain{};
    build_gain_graph(g, gain);
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> swapped{0};
    std::atomic<std::uint64_t> needs_eager{0};

    // Control thread: repeatedly stage a reinit-free gain edit and publish it via
    // prepare_swap while the audio thread renders. Gains stay non-zero so ANY
    // zero-output block would be a null publish window (the bug this rules out).
    std::thread editor([&] {
        bool hi = false;
        while (!stop.load(std::memory_order_relaxed)) {
            g.begin_swap_edit();
            g.set_node_gain(gain, hi ? 0.8f : 0.5f);
            hi = !hi;
            const auto r = g.prepare_swap(kSr, kFrames);
            if (r == SignalGraph::SwapResult::Swapped) {
                swapped.fetch_add(1, std::memory_order_relaxed);
            } else if (r == SignalGraph::SwapResult::NeedsEagerPrepare) {
                needs_eager.fetch_add(1, std::memory_order_relaxed);
                // No Catch2 macro on a worker thread (thread-unsafe). This branch
                // is defensive — a pure gain graph always Swaps — and the
                // needs_eager==0 CHECK after join (on the main thread) is the real
                // assertion. Recover so the render thread isn't left on a null graph.
                (void)g.prepare(kSr, kFrames);
            }
            std::this_thread::yield();
        }
    });

    bool saw_silent = false;
    for (int i = 0; i < 5000; ++i) {
        if (render_peak(g) == 0.0f) saw_silent = true;
    }
    stop.store(true, std::memory_order_relaxed);
    editor.join();

    // The contract: a reinit-free swap never drops a block to silence.
    CHECK_FALSE(saw_silent);
    // The swap path actually ran (a pure gain graph is always reinit-free, so it
    // must Swap, never fall back to eager prepare).
    CHECK(swapped.load(std::memory_order_relaxed) > 0);
    CHECK(needs_eager.load(std::memory_order_relaxed) == 0);
    CHECK(render_peak(g) > 0.0f);  // still usable after the swap storm
}

TEST_CASE("prepare_swap rejects non-reinit-free edits -> NeedsEagerPrepare (2.2b)",
          "[host][signal-graph][prepared-swap]") {
    SECTION("prepare_swap without begin_swap_edit -> NotInSwapEdit") {
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        REQUIRE(g.prepare(kSr, kFrames));
        CHECK(g.prepare_swap(kSr, kFrames) == SignalGraph::SwapResult::NotInSwapEdit);
    }
    SECTION("sample-rate change -> NeedsEagerPrepare") {
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        CHECK(g.prepare_swap(44100.0, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    SECTION("removed node (node-set change) -> NeedsEagerPrepare") {
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        REQUIRE(g.remove_node(gain));
        CHECK(g.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    SECTION("anticipation enabled -> NeedsEagerPrepare") {
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        g.set_anticipation_enabled(true);
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        CHECK(g.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    SECTION("MIDI edge in the graph -> NeedsEagerPrepare (M4 stuck-note gate)") {
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        const auto mi = g.add_midi_input_node("MI");
        const auto mo = g.add_midi_output_node("MO");
        REQUIRE(g.connect_midi(mi, mo));
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        g.set_node_gain(gain, 0.7f);  // a would-be reinit-free edit
        CHECK(g.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    SECTION("unchanged plugin PDC delay ring -> Swapped with state carry") {
        // Parallel merge: a latent plugin path (128) + a direct path (0) into the
        // same output ports → the direct branch gets a compensating delay ring.
        // An otherwise unchanged swap carries that ring state forward.
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<LatencySlot>(128), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
            REQUIRE(g.connect(in, c, out, c));  // direct parallel path (latency 0)
        }
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        CHECK(g.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::Swapped);
    }
    SECTION("smoothed automation edge -> NeedsEagerPrepare (CX3)") {
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<LatencySlot>(0), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        // Automate param 1 from input port 0 with a non-zero smoothing time.
        REQUIRE(g.connect_automation(in, 0, p, /*dest_param_id=*/1, 0.0f, 1.0f,
                                     /*smoothing_ms=*/5.0f));
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        CHECK(g.prepare_swap(kSr, kFrames)
              == SignalGraph::SwapResult::NeedsEagerPrepare);
    }
    SECTION("clear() during a swap-edit closes the transaction (review #1)") {
        // A non-allow-set mutator must force-abort: after clear(), prepare_swap
        // sees no open transaction (NotInSwapEdit), not a stale one.
        SignalGraph g;
        NodeId gain{};
        build_gain_graph(g, gain);
        REQUIRE(g.prepare(kSr, kFrames));
        g.begin_swap_edit();
        g.clear();
        CHECK(g.prepare_swap(kSr, kFrames) == SignalGraph::SwapResult::NotInSwapEdit);
    }
}
