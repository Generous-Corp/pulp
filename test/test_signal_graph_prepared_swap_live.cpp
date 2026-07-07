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
#include <cstdint>
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
                REQUIRE(g.prepare(kSr, kFrames));
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
}
