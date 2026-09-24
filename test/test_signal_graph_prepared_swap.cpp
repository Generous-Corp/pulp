// 2.2a — the no-silence-swap CONTRACT (planning/2026-07-02-signalgraph-prepared-swap-design.md).
//
// A live topology edit should not need to silence the graph while the next
// snapshot compiles. That is only safe if compile_() never touches state the
// audio thread reads — the audio thread reads ONLY the RCU-pinned CompiledGraph
// snapshot, and compile_() builds a brand-new one. Today prepare() nulls the live
// pointer BEFORE compiling (a crude quiescence barrier for plugin re-init), which
// HIDES whether compile_() itself is concurrency-safe.
//
// This test removes that hiding: it runs process() on one thread against the live
// snapshot while compile_() rebuilds fresh snapshots on another thread (via the
// test-only compile_snapshot_for_test hook — no null, no publish). Under TSan a
// data race between compile_() and process() fails the build. A clean run is the
// contract the compile-first swap (2.2b) is built on. Uses a pure gain graph —
// the plugin-reinit-free class 2.2b targets (no plugin instance to re-init).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>

#include "support/thread_progress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 64;

// input(2) -> gain(2/2) -> output(2). Plugin-reinit-free: a compile-first swap of
// this graph reuses no plugin instances, so it is the safe no-silence case.
void build_gain_graph(SignalGraph& g) {
    const auto in = g.add_input_node(2, "In");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.5f));
}

// One audio block. Non-zero input so the gain path actually does work.
void render_block(SignalGraph& g) {
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> in_l(kFrames, 0.25f), in_r(kFrames, 0.25f);
    std::array<const float*, 2> ic{in_l.data(), in_r.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);
}

}  // namespace

TEST_CASE("SignalGraph compile_() is race-free against a live process() "
          "(2.2a no-silence-swap contract)",
          "[host][signal-graph][prepared-swap][threads][rt-safety]") {
    SignalGraph g;
    build_gain_graph(g);
    // Exercise the canonical-executor routed path (what a real swap publishes).
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> compiles{0};
    // Control thread: continuously recompile fresh snapshots WITHOUT the
    // null-first prologue and WITHOUT publishing — mirroring what a compile-first
    // prepare_swap() (2.2b) does off the audio thread while the old snapshot plays.
    std::thread compiler([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            g.compile_snapshot_for_test(kSr, kFrames);
            compiles.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    // Wait for the compiler thread to reach its first recompile before rendering.
    // Nothing guarantees it is scheduled before main finishes: if `stop` is set
    // first, the loop body never runs, `compiles` stays zero, and there was never
    // anything to race against. Waiting here — rather than after the render loop —
    // makes the overlap real instead of merely making the count non-zero.
    // The deadline is what keeps this wait honest: an unbounded spin would trade
    // the flake for a hang, and a compiler that genuinely never advances must
    // reach the CHECK below rather than park the suite here forever.
    (void)pulp::test::wait_for_progress(compiles);

    // Audio thread: keep rendering the live snapshot the whole time.
    std::uint64_t blocks = 0;
    for (int i = 0; i < 3000; ++i) {
        render_block(g);
        ++blocks;
    }

    stop.store(true, std::memory_order_relaxed);
    compiler.join();

    // The value is the TSan verdict (a race fails the build); these just prove
    // both threads actually ran and the graph stayed usable throughout.
    CHECK(blocks == 3000);
    CHECK(compiles.load(std::memory_order_relaxed) > 0);

    // The live snapshot must still render after the concurrent recompilation storm.
    render_block(g);
}

// compile_() writes each authored node's transport_sensitive readback into
// nodes_, so every caller — prepare(), prepare_swap(), and this test hook — must
// hold the graph mutation lock around it. A debug build proves that through the
// lock-owner assertion inside node_mut_locked_ (this graph has nodes, so the
// assertion is reached on every compile). This test proves it in every build
// type: a custom type's latency query runs inside compile_(), and from there a
// second thread's locking reader (node_gain) must be unable to complete until
// the hook returns. Without the lock the reader finishes at once.
TEST_CASE("compile_snapshot_for_test holds the graph mutation lock while it compiles",
          "[host][signal-graph][prepared-swap][mutation-lock][threads]") {
    using namespace std::chrono_literals;

    SignalGraph g;
    // Null until the hook under test runs, so prepare()'s own compile_() — which
    // legitimately queries latency under the lock — does not start the probe.
    std::atomic<SignalGraph*> probe_graph{nullptr};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> reader_finished_during_compile{false};
    std::atomic<int> probes{0};
    std::thread reader;

    CustomNodeType probe;
    probe.type_id = "mutation-lock.probe";
    probe.num_input_ports = 2;
    probe.num_output_ports = 2;
    probe.process = [](pulp::audio::BufferView<float>& out,
                       const pulp::audio::BufferView<const float>& in, int n) {
        const std::size_t chs = std::min(out.num_channels(), in.num_channels());
        for (std::size_t c = 0; c < chs; ++c)
            std::copy_n(in.channel_ptr(c), n, out.channel_ptr(c));
    };
    probe.latency_samples = [&](double) {
        auto* graph = probe_graph.load(std::memory_order_acquire);
        // compile_() may query latency more than once per compile; one probe
        // per hook call is enough, and a second would orphan the first thread.
        if (graph == nullptr || reader.joinable()) return 0;
        probes.fetch_add(1, std::memory_order_relaxed);
        reader = std::thread([graph, &reader_done] {
            (void)graph->node_gain(1);
            reader_done.store(true, std::memory_order_release);
        });
        // Bounded: with the lock held this window always elapses in full, so it
        // is the only cost a correct build pays. Without the lock the reader
        // acquires the mutex immediately and is seen long before the deadline.
        const auto deadline = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < deadline) {
            if (reader_done.load(std::memory_order_acquire)) {
                reader_finished_during_compile.store(true, std::memory_order_relaxed);
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        return 0;
    };
    REQUIRE(g.register_custom_node_type(probe));

    const auto in = g.add_input_node(2, "In");
    const auto cust = g.add_custom_node("mutation-lock.probe", "Probe");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, cust, c));
        REQUIRE(g.connect(cust, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(probes.load(std::memory_order_relaxed) == 0);

    probe_graph.store(&g, std::memory_order_release);
    g.compile_snapshot_for_test(kSr, kFrames);
    probe_graph.store(nullptr, std::memory_order_release);

    // Instrument control: the latency query really ran inside the hook's
    // compile_() and the reader really ran — it completes once the hook returns.
    REQUIRE(probes.load(std::memory_order_relaxed) == 1);
    REQUIRE(reader.joinable());
    reader.join();
    REQUIRE(reader_done.load(std::memory_order_acquire));

    CHECK_FALSE(reader_finished_during_compile.load(std::memory_order_relaxed));
}
