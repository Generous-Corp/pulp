// Convergence proof for the SignalGraph -> canonical-executor translation: for
// graphs in the executor-eligible subset (AudioInput / AudioOutput / Gain /
// live Plugin / MidiInput / MidiOutput, with audio, sidechain, feedback, MIDI,
// sparse automation, dense audio-rate modulation, and PDC), driving the
// TRANSLATED routing through GraphRuntimeExecutor::process_routed() produces
// output bit-identical to SignalGraph's own process() walk. Also checks
// eligibility gating and that a post-prepare set_node_gain() is honored on the
// routed path (the binding reads the live snapshot's atomic).

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

using pulp::host::SignalGraph;
using pulp::host::SignalGraphExecutorRouting;
using pulp::host::build_signal_graph_executor_routing;
using pulp::host::signal_graph_executor_eligible;

std::vector<float> ramp(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

// Drive SignalGraph's own walk for one block; returns per-channel output.
std::vector<std::vector<float>> run_legacy(SignalGraph& g, int frames,
                                           const std::vector<std::vector<float>>& in,
                                           std::size_t out_channels) {
    // The walk oracle: force the legacy walk so this stays an independent
    // reference. Disable EVERY routing opt-in (canonical is ON by default;
    // parallel/anticipation take precedence in process() when enabled), so the
    // oracle is the walk regardless of any other flag a caller set.
    g.set_canonical_executor_routing_enabled(false);
    g.set_parallel_routing_enabled(false);
    g.set_anticipation_enabled(false);
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(out_channels,
                                         std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return outs;
}

// Drive SignalGraph::process() for one block WITHOUT forcing any routing flag —
// uses whatever routing the graph has enabled (canonical/parallel). This is the
// only way to exercise the genuinely-routed automation path, whose per-node
// MIDI/automation scratch lives in the CompiledGraph (the standalone
// build_signal_graph_executor_routing snapshot carries no automation scratch).
std::vector<std::vector<float>> run_graph_process(SignalGraph& g, int frames,
                                                  const std::vector<std::vector<float>>& in,
                                                  std::size_t out_channels) {
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(out_channels,
                                         std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return outs;
}

// Drive the translated routing for one block; returns per-channel output.
std::vector<std::vector<float>> run_routed(pulp::format::GraphRuntimeExecutor& exec,
                                           SignalGraphExecutorRouting& routing, int frames,
                                           const std::vector<std::vector<float>>& in,
                                           std::size_t out_channels) {
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(out_channels,
                                         std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(in_ptrs.data(), in_ptrs.size(),
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ptrs.data(), out_ptrs.size(),
                                            static_cast<std::uint32_t>(frames));
    pulp::format::BusBufferSet buses;
    REQUIRE(buses.add_input("main", in_view, pulp::format::BusRole::Main));
    REQUIRE(buses.add_output("main", out_view, pulp::format::BusRole::Main));
    pulp::format::ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = static_cast<std::uint32_t>(frames);
    block.buses = &buses;
    REQUIRE(block.validate());
    REQUIRE(exec.process_routed(block, routing.snapshot, routing.pool).ok());
    return outs;
}

void expect_equal(const std::vector<std::vector<float>>& a,
                  const std::vector<std::vector<float>>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t c = 0; c < a.size(); ++c) {
        REQUIRE(a[c].size() == b[c].size());
        for (std::size_t i = 0; i < a[c].size(); ++i) REQUIRE(a[c][i] == b[c][i]);
    }
}

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

} // namespace

TEST_CASE("Translated routing matches SignalGraph: stereo two-gain chain",
          "[host][graph][executor][routing][parity]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 1, g1, 1));
    REQUIRE(g.connect(g1, 0, g2, 0));
    REQUIRE(g.connect(g1, 1, g2, 1));
    REQUIRE(g.connect(g2, 0, out, 0));
    REQUIRE(g.connect(g2, 1, out, 1));
    REQUIRE(g.set_node_gain(g1, 0.5f));
    REQUIRE(g.set_node_gain(g2, 0.75f));
    REQUIRE(g.prepare(kSr, kFrames));

    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);

    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f), ramp(kFrames, 0.6f)};
    const auto legacy = run_legacy(g, kFrames, input, 2);
    const auto routed = run_routed(exec, routing, kFrames, input, 2);
    expect_equal(legacy, routed);
}

TEST_CASE("Translated routing matches SignalGraph: mono diamond with fan-in",
          "[host][graph][executor][routing][parity]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 0, g2, 0));
    REQUIRE(g.connect(g1, 0, out, 0));
    REQUIRE(g.connect(g2, 0, out, 0));  // out:0 sums g1 + g2
    REQUIRE(g.set_node_gain(g1, 0.5f));
    REQUIRE(g.set_node_gain(g2, 0.25f));
    REQUIRE(g.prepare(kSr, kFrames));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f)};
    expect_equal(run_legacy(g, kFrames, input, 1),
                 run_routed(exec, routing, kFrames, input, 1));
}

TEST_CASE("Translated routing matches SignalGraph: three-way fan-in summation order",
          "[host][graph][executor][routing][parity]") {
    // in -> {g1, g2, g3} -> out:0. Three sources sum into one dest port; with
    // 3+ terms float addition is reassociation-sensitive, so this only stays
    // bit-exact if the executor sums inbound edges in the SAME order SignalGraph
    // does (connection order). Distinct gains make the order observable.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto g3 = g.add_gain_node("G3");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 0, g2, 0));
    REQUIRE(g.connect(in, 0, g3, 0));
    REQUIRE(g.connect(g1, 0, out, 0));
    REQUIRE(g.connect(g2, 0, out, 0));
    REQUIRE(g.connect(g3, 0, out, 0));
    REQUIRE(g.set_node_gain(g1, 0.3333333f));
    REQUIRE(g.set_node_gain(g2, 0.1717171f));
    REQUIRE(g.set_node_gain(g3, 0.9191919f));
    REQUIRE(g.prepare(kSr, kFrames));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.77f)};
    expect_equal(run_legacy(g, kFrames, input, 1),
                 run_routed(exec, routing, kFrames, input, 1));
}

TEST_CASE("Translated routing matches SignalGraph: feedback loop across blocks",
          "[host][graph][executor][routing][parity][feedback]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.connect_feedback(a, 0, a, 0));
    REQUIRE(g.set_node_gain(a, 0.5f));
    REQUIRE(g.prepare(kSr, kFrames));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    pulp::format::GraphRuntimeExecutor exec;
    // Both paths carry independent feedback state seeded from zero; identical
    // per-block inputs must yield identical per-block outputs across the loop.
    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.3f + 0.1f * static_cast<float>(blk))};
        expect_equal(run_legacy(g, kFrames, input, 1),
                     run_routed(exec, routing, kFrames, input, 1));
    }
}

TEST_CASE("Translated routing honors set_node_gain after prepare",
          "[host][graph][executor][routing][parity]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.set_node_gain(a, 1.0f));
    REQUIRE(g.prepare(kSr, kFrames));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f)};

    // Change gain after the routing was built (no re-prepare): the binding reads
    // the live atomic, so both paths must still agree.
    REQUIRE(g.set_node_gain(a, 0.3f));
    expect_equal(run_legacy(g, kFrames, input, 1),
                 run_routed(exec, routing, kFrames, input, 1));
}

namespace {

// Build in -> A(gain) -> out + A->A feedback. `route_executor` selects the path.
void build_feedback_graph(SignalGraph& g, float gain, bool route_executor) {
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.connect_feedback(a, 0, a, 0));
    REQUIRE(g.set_node_gain(a, gain));
    g.set_canonical_executor_routing_enabled(route_executor);
    REQUIRE(g.prepare(kSr, kFrames));
}

} // namespace

TEST_CASE("SignalGraph::process opt-in executor path matches the legacy walk",
          "[host][graph][executor][routing][parity]") {
    // Two identical feedback graphs, one on the legacy walk and one routed
    // through the executor, fed the same per-block inputs: their output must
    // match block-for-block (proves the live opt-in dispatch + cross-block
    // feedback state agree with the legacy path).
    SignalGraph legacy, routed;
    build_feedback_graph(legacy, 0.5f, /*route_executor=*/false);
    build_feedback_graph(routed, 0.5f, /*route_executor=*/true);
    REQUIRE(signal_graph_executor_eligible(routed));

    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        const auto x = ramp(kFrames, 0.4f + 0.1f * static_cast<float>(blk));
        std::vector<float> li = x, ri = x, lo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 1> lic{li.data()}, ric{ri.data()};
        std::array<float*, 1> loc{lo.data()}, roc{ro.data()};
        pulp::audio::BufferView<const float> liv(lic.data(), 1, kFrames), riv(ric.data(), 1, kFrames);
        pulp::audio::BufferView<float> lov(loc.data(), 1, kFrames), rov(roc.data(), 1, kFrames);
        legacy.process(lov, liv, kFrames);
        routed.process(rov, riv, kFrames);
        for (int i = 0; i < kFrames; ++i)
            REQUIRE(lo[static_cast<std::size_t>(i)] == ro[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("SignalGraph::process routes a graph carrying a Custom node",
          "[host][graph][executor][routing]") {
    // A Custom node no longer disqualifies the routed path: even an UNRESOLVED
    // custom node (unregistered type) is eligible, because its binding falls back
    // to pass-through-or-zero exactly as the legacy walk does. Here the custom
    // node is disconnected, so the routed output is the same in*gain the walk
    // produces — proving the routed path handles the custom node without diverging.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    (void)g.add_unresolved_custom_node("test.custom", 1, 1, 1, "Custom");  // unresolved
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.set_node_gain(a, 0.5f));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    CHECK(signal_graph_executor_eligible(g));  // Custom node no longer disqualifies

    const auto x = ramp(kFrames, 0.8f);
    std::vector<float> xi = x, yo(kFrames, 0.0f);
    std::array<const float*, 1> ic{xi.data()};
    std::array<float*, 1> oc{yo.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 1, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 1, kFrames);
    g.process(ov, iv, kFrames);
    for (int i = 0; i < kFrames; ++i)
        REQUIRE(yo[static_cast<std::size_t>(i)] == x[static_cast<std::size_t>(i)] * 0.5f);
}

TEST_CASE("SignalGraph::process executor path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety]") {
    SignalGraph g;
    build_feedback_graph(g, 0.5f, /*route_executor=*/true);
    REQUIRE(signal_graph_executor_eligible(g));

    const auto x = ramp(kFrames, 0.8f);
    std::vector<float> xi = x, yo(kFrames, 0.0f);
    std::array<const float*, 1> ic{xi.data()};
    std::array<float*, 1> oc{yo.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 1, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 1, kFrames);

    g.process(ov, iv, kFrames);  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

namespace {

// Build in -> g1 -> g2 -> out (stereo, feedforward); `route_executor` selects path.
void build_stereo_chain(SignalGraph& g, float a, float b, bool route_executor) {
    const auto in = g.add_input_node(2, "In");
    const auto g1 = g.add_gain_node("G1");
    const auto g2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(in, 0, g1, 0));
    REQUIRE(g.connect(in, 1, g1, 1));
    REQUIRE(g.connect(g1, 0, g2, 0));
    REQUIRE(g.connect(g1, 1, g2, 1));
    REQUIRE(g.connect(g2, 0, out, 0));
    REQUIRE(g.connect(g2, 1, out, 1));
    REQUIRE(g.set_node_gain(g1, a));
    REQUIRE(g.set_node_gain(g2, b));
    g.set_canonical_executor_routing_enabled(route_executor);
    REQUIRE(g.prepare(kSr, kFrames));
}

} // namespace

TEST_CASE("SignalGraph::process opt-in dispatch matches legacy: stereo chain + multi-output",
          "[host][graph][executor][routing][parity]") {
    // Drive the EMBEDDED SignalGraph::process dispatch (not the standalone
    // routing) for a stereo chain and a two-AudioOutput mix, on vs off, and
    // require bit-identical output. Covers the executor branch's own bus wiring.
    SECTION("stereo chain") {
        SignalGraph off, on;
        build_stereo_chain(off, 0.5f, 0.75f, /*route_executor=*/false);
        build_stereo_chain(on, 0.5f, 0.75f, /*route_executor=*/true);
        const std::vector<std::vector<float>> in{ramp(kFrames, 0.8f), ramp(kFrames, 0.6f)};
        expect_equal(run_legacy(off, kFrames, in, 2), run_legacy(on, kFrames, in, 2));
    }
    SECTION("two AudioOutput nodes mix") {
        auto build = [](SignalGraph& g, bool route_executor) {
            const auto in = g.add_input_node(1, "In");
            const auto a = g.add_gain_node("A");
            const auto b = g.add_gain_node("B");
            const auto o1 = g.add_output_node(1, "O1");
            const auto o2 = g.add_output_node(1, "O2");
            REQUIRE(g.connect(in, 0, a, 0));
            REQUIRE(g.connect(in, 0, b, 0));
            REQUIRE(g.connect(a, 0, o1, 0));
            REQUIRE(g.connect(b, 0, o2, 0));
            REQUIRE(g.set_node_gain(a, 0.5f));
            REQUIRE(g.set_node_gain(b, 0.25f));
            g.set_canonical_executor_routing_enabled(route_executor);
            REQUIRE(g.prepare(kSr, kFrames));
        };
        SignalGraph off, on;
        build(off, false);
        build(on, true);
        const std::vector<std::vector<float>> in{ramp(kFrames, 0.8f)};
        expect_equal(run_legacy(off, kFrames, in, 1), run_legacy(on, kFrames, in, 1));
    }
}

TEST_CASE("SignalGraph::process feedforward executor toggle is seamless mid-stream",
          "[host][graph][executor][routing][parity]") {
    // For a feedforward graph (no per-path feedback state), toggling
    // Toggling the canonical executor between blocks must stay bit-identical to
    // the steady legacy path block-for-block.
    SignalGraph toggled, legacy;
    build_stereo_chain(toggled, 0.5f, 0.75f, /*route_executor=*/false);
    build_stereo_chain(legacy, 0.5f, 0.75f, /*route_executor=*/false);
    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        toggled.set_canonical_executor_routing_enabled(blk % 2 == 0);  // flip every block
        const std::vector<std::vector<float>> in{
            ramp(kFrames, 0.4f + 0.1f * static_cast<float>(blk)), ramp(kFrames, 0.5f)};
        expect_equal(run_legacy(toggled, kFrames, in, 2),
                     run_legacy(legacy, kFrames, in, 2));
    }
}

TEST_CASE("SignalGraph re-prepare while the executor path renders is race-free",
          "[host][graph][executor][routing][rt-safety][threads]") {
    // The routed scratch pool is owned per-snapshot and retired through the
    // same reader-count/raw-pointer handshake as the rest of CompiledGraph.
    // Hammer process() on one thread while another re-prepares; this catches a
    // shared-pool resize race or a retired-snapshot UAF (surfaces cleanly under
    // TSan).
    SignalGraph g;
    build_feedback_graph(g, 0.5f, /*route_executor=*/true);

    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> blocks{0};
    std::thread audio([&] {
        std::vector<float> xi(kFrames, 0.3f), yo(kFrames, 0.0f);
        std::array<const float*, 1> ic{xi.data()};
        std::array<float*, 1> oc{yo.data()};
        pulp::audio::BufferView<const float> iv(ic.data(), 1, kFrames);
        pulp::audio::BufferView<float> ov(oc.data(), 1, kFrames);
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            g.process(ov, iv, kFrames);  // may render or hit the silent gap
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    bool prepared_all = true;
    for (int i = 0; i < 200; ++i) {
        if (!g.prepare(kSr, kFrames)) {  // builds + publishes fresh snapshots
            prepared_all = false;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    audio.join();
    REQUIRE(prepared_all);
    CHECK(blocks.load() > 0);  // the audio thread actually ran
}

TEST_CASE("Executor eligibility rejects unprepared and non-eligible graphs",
          "[host][graph][executor][routing]") {
    SignalGraph unprepared;
    const auto in = unprepared.add_input_node(1, "In");
    const auto a = unprepared.add_gain_node("A");
    const auto out = unprepared.add_output_node(1, "Out");
    REQUIRE(unprepared.connect(in, 0, a, 0));
    REQUIRE(unprepared.connect(a, 0, out, 0));
    CHECK_FALSE(signal_graph_executor_eligible(unprepared));  // not prepared

    SignalGraphExecutorRouting routing;
    CHECK_FALSE(build_signal_graph_executor_routing(unprepared, routing));
    CHECK_FALSE(routing.valid);

    // A Custom node is now inside the eligible subset (audio-only): even an
    // unresolved type is eligible because its binding pass-through-or-zeros like
    // the walk, so a prepared custom graph routes and its routing build succeeds.
    SignalGraph custom;
    const auto cin = custom.add_input_node(1, "In");
    const auto cnode = custom.add_unresolved_custom_node("test.custom", 1, 1, 1, "Custom");
    const auto cout = custom.add_output_node(1, "Out");
    REQUIRE(custom.connect(cin, 0, cnode, 0));
    REQUIRE(custom.connect(cnode, 0, cout, 0));
    REQUIRE(custom.prepare(kSr, kFrames));
    CHECK(signal_graph_executor_eligible(custom));
    SignalGraphExecutorRouting custom_routing;
    CHECK(build_signal_graph_executor_routing(custom, custom_routing));
    CHECK(custom_routing.valid);
}

TEST_CASE("Executor routing build fails closed for oversized otherwise-eligible graphs",
          "[host][graph][executor][routing]") {
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    auto prev = in;

    // This stays within SignalGraph's default GraphLimits but exceeds
    // GraphRuntimePlan's default max_nodes, so the capability predicate remains
    // true while the actual route construction must fail cleanly.
    const auto gain_count = pulp::graph::GraphRuntimeLimits{}.max_nodes;
    for (std::uint32_t i = 0; i < gain_count; ++i) {
        const auto gain = g.add_gain_node("G");
        REQUIRE(g.connect(prev, 0, gain, 0));
        prev = gain;
    }

    const auto out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(prev, 0, out, 0));
    REQUIRE(g.prepare(kSr, kFrames));

    CHECK(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    CHECK_FALSE(build_signal_graph_executor_routing(g, routing));
    CHECK_FALSE(routing.valid);
}

namespace {

using pulp::host::PluginInfo;
using pulp::host::PluginFormat;
using pulp::host::PluginSlot;

PluginInfo test_plugin_info(int in_ch, int out_ch) {
    PluginInfo info{};
    info.name = "DeterministicTest";
    info.format = PluginFormat::CLAP;
    info.num_inputs = in_ch;
    info.num_outputs = out_ch;
    info.category = "Fx";
    return info;
}

// Deterministic, stateless test plugin for routing parity.
//   kFull            : out[c] = in[c] * scale for every channel (full write).
//   kPartialCh1Gated : out[0] = in[0] * scale always; out[1] is written only on
//                      blocks whose first input sample is > 0, and otherwise left
//                      untouched — so its buffer carries a non-trivial stale tail
//                      across blocks. Stateless (the decision depends only on the
//                      current block), so the same instance can drive both walks.
class DeterministicPlugin final : public PluginSlot {
public:
    enum class Mode { kFull, kPartialCh1Gated, kSidechainMix };
    DeterministicPlugin(Mode mode, float scale, int in_ch, int out_ch, int latency = 0)
        : mode_(mode), scale_(scale), latency_(latency),
          info_(test_plugin_info(in_ch, out_ch)) {}

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
        const std::size_t chs = out.num_channels();
        if (mode_ == Mode::kSidechainMix) {
            // out[c] = main[c] * scale + sidechain[c], where the sidechain
            // channels are the input channels just past the main channels (a
            // plugin exposing `chs` main + `chs` sidechain inputs). Reading the
            // sidechain channels makes a sidechain-routing or PDC-delay bug
            // surface as an output divergence.
            for (std::size_t c = 0; c < chs; ++c) {
                float* o = out.channel_ptr(c);
                const float* m = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
                const std::size_t sc = c + chs;
                const float* s = sc < in.num_channels() ? in.channel_ptr(sc) : nullptr;
                for (int k = 0; k < n; ++k) {
                    const auto i = static_cast<std::size_t>(k);
                    o[i] = (m ? m[i] * scale_ : 0.0f) + (s ? s[i] : 0.0f);
                }
            }
            return;
        }
        const bool gate_open = in.num_samples() > 0 && in.num_channels() > 0 &&
                               in.channel_ptr(0)[0] > 0.0f;
        for (std::size_t c = 0; c < chs; ++c) {
            if (mode_ == Mode::kPartialCh1Gated && c == 1 && !gate_open) {
                continue;  // leave channel 1 untouched -> stale tail persists
            }
            float* o = out.channel_ptr(c);
            const float* i = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int s = 0; s < n; ++s) {
                o[static_cast<std::size_t>(s)] =
                    i ? i[static_cast<std::size_t>(s)] * scale_ : 0.0f;
            }
        }
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
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
    Mode mode_;
    float scale_;
    int latency_;
    PluginInfo info_;
};

// Deterministic MIDI plugin: passes audio through (full write) and emits a copy
// of each inbound note transposed by `transpose_` semitones (other messages pass
// through). The transpose makes the plugin's MIDI output distinct from its input
// so a bug that routed the wrong buffer would surface.
class MidiTransposePlugin final : public PluginSlot {
public:
    MidiTransposePlugin(int transpose, int in_ch, int out_ch)
        : transpose_(transpose), info_(test_plugin_info(in_ch, out_ch)) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer& midi_in,
                 pulp::midi::MidiBuffer& midi_out,
                 const pulp::host::ParameterEventQueue&,
                 int n) override {
        const std::size_t chs = out.num_channels();
        for (std::size_t c = 0; c < chs; ++c) {
            float* o = out.channel_ptr(c);
            const float* i = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int s = 0; s < n; ++s) {
                o[static_cast<std::size_t>(s)] = i ? i[static_cast<std::size_t>(s)] : 0.0f;
            }
        }
        for (const auto& ev : midi_in) {
            const auto ch = static_cast<uint8_t>(ev.data()[0] & 0x0F);
            const auto note = static_cast<uint8_t>((ev.data()[1] + transpose_) & 0x7F);
            if (ev.is_note_on()) {
                midi_out.add(pulp::midi::MidiEvent::note_on(ch, note, ev.data()[2]));
            } else if (ev.is_note_off()) {
                midi_out.add(pulp::midi::MidiEvent::note_off(ch, note, ev.data()[2]));
            } else {
                midi_out.add(ev);
            }
        }
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override { return {}; }
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
    int transpose_;
    PluginInfo info_;
};

// A comparable snapshot of a MIDI buffer's events (3 status/data bytes + size +
// offset), in buffer order, for bit-exact comparison between the two walks.
struct MidiEvtKey {
    std::array<uint8_t, 3> bytes{};
    std::uint32_t size = 0;
    int32_t offset = 0;
    bool operator==(const MidiEvtKey& o) const {
        return bytes == o.bytes && size == o.size && offset == o.offset;
    }
};

std::vector<MidiEvtKey> collect_midi(const pulp::midi::MidiBuffer& buf) {
    std::vector<MidiEvtKey> out;
    for (const auto& ev : buf) {
        MidiEvtKey k;
        k.size = ev.size();
        for (std::uint32_t i = 0; i < ev.size() && i < 3; ++i) k.bytes[i] = ev.data()[i];
        k.offset = ev.sample_offset;
        out.push_back(k);
    }
    return out;
}

void expect_midi_equal(const std::vector<MidiEvtKey>& a,
                       const std::vector<MidiEvtKey>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

pulp::midi::MidiBuffer make_injected_midi(int seed) {
    pulp::midi::MidiBuffer buf;
    buf.reserve(64, 8, 256);
    buf.add(pulp::midi::MidiEvent::note_on(0, static_cast<uint8_t>(60 + seed), 100));
    buf.add(pulp::midi::MidiEvent::cc(0, 7, static_cast<uint8_t>(64 + seed)));
    buf.add(pulp::midi::MidiEvent::note_off(0, static_cast<uint8_t>(60 + seed), 0));
    return buf;
}

// Plugin with one automatable parameter (id 1, bounds [0,2]) that applies it as
// a per-sample gain ramp from the block's first control point to its last:
// out[c][i] = in[c][i] * lerp(v0, vN, i/last). Reading the parameter events makes
// any automation gather divergence (mapping, slew, mix, clamp, ordering) show up
// as an audio-output difference.
class AutomatedGainPlugin final : public PluginSlot {
public:
    explicit AutomatedGainPlugin(int channels)
        : info_(test_plugin_info(channels, channels)) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& params,
                 int n) override {
        const int last = n - 1;
        // Param 1 (sparse / control-rate): linear gain ramp v0 -> vN.
        float v0 = 1.0f;
        float vN = 1.0f;
        // Param 2 (dense / audio-rate): per-sample gain, default 1 where unset.
        std::array<float, 4096> g2;
        for (int s = 0; s < n && s < 4096; ++s) g2[static_cast<std::size_t>(s)] = 1.0f;
        for (const auto& ev : params) {
            if (ev.param_id == 1) {
                if (ev.sample_offset == 0) v0 = ev.value;
                if (ev.sample_offset == last) vN = ev.value;
            } else if (ev.param_id == 2 && ev.sample_offset >= 0 &&
                       ev.sample_offset < n && ev.sample_offset < 4096) {
                g2[static_cast<std::size_t>(ev.sample_offset)] = ev.value;
            }
        }
        const std::size_t chs = out.num_channels();
        for (std::size_t c = 0; c < chs; ++c) {
            float* o = out.channel_ptr(c);
            const float* i = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int s = 0; s < n; ++s) {
                const float t = last > 0 ? static_cast<float>(s) / static_cast<float>(last) : 0.0f;
                const float gain = (v0 + (vN - v0) * t) * g2[static_cast<std::size_t>(s)];
                o[static_cast<std::size_t>(s)] =
                    (i ? i[static_cast<std::size_t>(s)] : 0.0f) * gain;
            }
        }
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        pulp::host::HostParamInfo p;
        p.id = 1;
        p.name = "Gain";
        p.min_value = 0.0f;
        p.max_value = 2.0f;
        p.default_value = 1.0f;
        p.flags.automatable = true;
        // A second, audio-rate-capable parameter for dense-modulation coverage.
        pulp::host::HostParamInfo p2;
        p2.id = 2;
        p2.name = "AudioRateGain";
        p2.min_value = 0.0f;
        p2.max_value = 2.0f;
        p2.default_value = 1.0f;
        p2.flags.automatable = true;
        p2.flags.modulatable = true;
        p2.rate = pulp::state::ParamRate::AudioRate;
        return {p, p2};
    }
    float get_parameter(uint32_t) const override { return 1.0f; }
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
    PluginInfo info_;
};

// Plugin whose output reflects EVERY automated parameter it receives: per sample
// the gain is the sum of each parameter's most-recent event value (default 0),
// applied to the input. Distinct param ids 1..num_params (audio-rate-capable, so
// usable for both sparse and dense edges). Because the output depends on every
// param, a gather that dropped any parameter would diverge from the walk — which
// is what the over-cap parity tests rely on. Walk and routed feed identical
// event streams, so an exact match proves the routed gather carried them all.
class ManyParamSumGainPlugin final : public PluginSlot {
public:
    ManyParamSumGainPlugin(int num_params, int channels)
        : num_params_(num_params), info_(test_plugin_info(channels, channels)) {}
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    static constexpr int kMaxId = 512;  // ids 1..kMaxId; stack-only, RT-safe
    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& params, int n) override {
        // Running per-param value (indexed by id; ids are 1..num_params_) and its
        // running sum. Stack array (no audio-thread allocation). Events arrive
        // sorted by sample offset.
        std::array<float, kMaxId + 1> cur{};
        float sum = 0.0f;
        auto it = params.begin();
        const std::size_t chs = out.num_channels();
        for (int s = 0; s < n; ++s) {
            while (it != params.end() && it->sample_offset <= s) {
                const std::uint32_t pid = it->param_id;
                if (pid >= 1 && pid <= static_cast<std::uint32_t>(num_params_) &&
                    pid <= static_cast<std::uint32_t>(kMaxId)) {
                    sum -= cur[pid];
                    cur[pid] = it->value;
                    sum += cur[pid];
                }
                ++it;
            }
            for (std::size_t c = 0; c < chs; ++c) {
                const float* i = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
                out.channel_ptr(c)[static_cast<std::size_t>(s)] =
                    (i ? i[static_cast<std::size_t>(s)] : 0.0f) * sum;
            }
        }
    }
    std::vector<pulp::host::HostParamInfo> parameters() const override {
        std::vector<pulp::host::HostParamInfo> ps;
        for (int i = 0; i < num_params_; ++i) {
            pulp::host::HostParamInfo p;
            p.id = static_cast<uint32_t>(i + 1);
            p.min_value = 0.0f;
            p.max_value = 1.0f;
            p.flags.automatable = true;
            p.flags.modulatable = true;
            p.rate = pulp::state::ParamRate::AudioRate;
            ps.push_back(p);
        }
        return ps;
    }
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
    int num_params_;
    PluginInfo info_;
};

} // namespace

TEST_CASE("Translated routing matches SignalGraph: input -> plugin -> output",
          "[host][graph][executor][routing][parity][plugin]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2),
        2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(in, 0, p, 0));
    REQUIRE(g.connect(in, 1, p, 1));
    REQUIRE(g.connect(p, 0, out, 0));
    REQUIRE(g.connect(p, 1, out, 1));
    REQUIRE(g.prepare(kSr, kFrames));

    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);

    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f), ramp(kFrames, 0.6f)};
    expect_equal(run_legacy(g, kFrames, input, 2),
                 run_routed(exec, routing, kFrames, input, 2));
}

TEST_CASE("Translated routing matches SignalGraph: plugin chain",
          "[host][graph][executor][routing][parity][plugin]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p1 = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(DeterministicPlugin::Mode::kFull, 0.5f, 2, 2),
        2, 2, "P1");
    const auto p2 = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(DeterministicPlugin::Mode::kFull, 0.75f, 2, 2),
        2, 2, "P2");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p1, c));
        REQUIRE(g.connect(p1, c, p2, c));
        REQUIRE(g.connect(p2, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.9f), ramp(kFrames, 0.4f)};
    expect_equal(run_legacy(g, kFrames, input, 2),
                 run_routed(exec, routing, kFrames, input, 2));
}

TEST_CASE("Translated routing matches SignalGraph: plugin + gain fan-in mix",
          "[host][graph][executor][routing][parity][plugin]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(DeterministicPlugin::Mode::kFull, 0.5f, 2, 2),
        2, 2, "P");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(p, c, out, c));     // plugin -> output
        REQUIRE(g.connect(gain, c, out, c));  // gain   -> output (fan-in sum)
    }
    REQUIRE(g.set_node_gain(gain, 0.25f));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.7f), ramp(kFrames, 0.5f)};
    expect_equal(run_legacy(g, kFrames, input, 2),
                 run_routed(exec, routing, kFrames, input, 2));
}

TEST_CASE("Translated routing matches SignalGraph: non-full-writing plugin persists its tail across blocks",
          "[host][graph][executor][routing][parity][plugin][persistent]") {
    // The plugin leaves channel 1 untouched on blocks whose first input sample
    // is <= 0. SignalGraph keeps channel 1's previous value in its persistent
    // per-node buffer; the executor must do the same via the pinned persistent
    // output slot. Drive several blocks whose ch0 sign alternates, so ch1 is
    // written on some blocks and stale on others, and require block-for-block
    // equality across the whole sequence.
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kPartialCh1Gated, 0.5f, 2, 2),
        2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;

    // Block 0 opens the gate (ch0[0] > 0) so ch1 gets a real value; block 1
    // closes it (ch0[0] < 0) so ch1 must stay stale; alternate for 6 blocks.
    for (int blk = 0; blk < 6; ++blk) {
        const float sign = (blk % 2 == 0) ? 1.0f : -1.0f;
        std::vector<float> ch0 = ramp(kFrames, 0.8f);
        std::vector<float> ch1 = ramp(kFrames, 0.6f);
        ch0[0] = sign * 0.5f;  // drive the gate deterministically
        const std::vector<std::vector<float>> input{ch0, ch1};
        const auto legacy = run_legacy(g, kFrames, input, 2);
        const auto routed = run_routed(exec, routing, kFrames, input, 2);
        INFO("block " << blk);
        expect_equal(legacy, routed);
    }
}

TEST_CASE("Translated routing routes a Plugin node with no live slot as pass-through",
          "[host][graph][executor][routing][parity][plugin]") {
    // An unresolved/placeholder Plugin node (metadata, no live slot — e.g. a
    // GraphSerializer rehydration of a missing plugin) is eligible and routes
    // through the binding's pass-through-or-zero, exactly as SignalGraph's walk
    // does for a slot-less plugin node. Prove bit-exact parity on BOTH arms of
    // pass_through_or_zero: the channel-matched copy and the zero-fill of an
    // extra output channel.

    // Channel-matched (2 in -> 2 out): both channels copied straight through.
    {
        SignalGraph g;
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_unresolved_plugin_node(test_plugin_info(2, 2), 2, 2, "Missing");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.prepare(kSr, kFrames));
        REQUIRE(signal_graph_executor_eligible(g));
        SignalGraphExecutorRouting routing;
        REQUIRE(build_signal_graph_executor_routing(g, routing));
        REQUIRE(routing.valid);
        pulp::format::GraphRuntimeExecutor exec;
        const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f), ramp(kFrames, 0.6f)};
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }

    // Channel-mismatched (1 in -> 2 out): pass-through copies ch0, then zero-fills
    // the extra output channel — exercising the zero-fill arm.
    {
        SignalGraph g;
        const auto in = g.add_input_node(1, "In");
        const auto p = g.add_unresolved_plugin_node(test_plugin_info(1, 2), 1, 2, "Missing");
        const auto out = g.add_output_node(2, "Out");
        REQUIRE(g.connect(in, 0, p, 0));
        REQUIRE(g.connect(p, 0, out, 0));
        REQUIRE(g.connect(p, 1, out, 1));
        REQUIRE(g.prepare(kSr, kFrames));
        REQUIRE(signal_graph_executor_eligible(g));
        SignalGraphExecutorRouting routing;
        REQUIRE(build_signal_graph_executor_routing(g, routing));
        REQUIRE(routing.valid);
        pulp::format::GraphRuntimeExecutor exec;
        const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f)};
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process plugin executor path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety][plugin]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(DeterministicPlugin::Mode::kFull, 0.5f, 2, 2),
        2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    const auto l = ramp(kFrames, 0.8f), r = ramp(kFrames, 0.6f);
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);

    g.process(ov, iv, kFrames);  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("Translated routing matches SignalGraph: latency-reporting plugin in a chain",
          "[host][graph][executor][routing][parity][plugin][pdc]") {
    // A latency-reporting plugin is now eligible: the routed gather applies the
    // same per-connection delay compensation as the legacy walk. In a pure chain
    // every path is equally latent, so no connection is delayed — but the graph
    // must still be eligible and route bit-exactly across blocks.
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2, /*latency=*/64),
        2, 2, "LatencyP");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.8f + 0.01f * static_cast<float>(blk)),
            ramp(kFrames, 0.6f - 0.01f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }
}

namespace {

// A diamond where one arm carries a latency-reporting plugin and the other a
// plain gain. The plain arm must be delay-compensated to time-align with the
// plugin arm at the output fan-in, exercising the per-connection delay ring.
// Driven over several blocks (the ring carries state across blocks) and required
// to match SignalGraph's own walk block-for-block. Parameterized on the reported
// latency so both sub-block and supra-block delays are covered.
void run_latency_diamond_parity(int latency) {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2, latency),
        2, 2, "LatencyP");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));      // latent arm
        REQUIRE(g.connect(in, c, gain, c));   // plain arm (must be delayed)
        REQUIRE(g.connect(p, c, out, c));
        REQUIRE(g.connect(gain, c, out, c));  // fan-in sum at the output
    }
    REQUIRE(g.set_node_gain(gain, 0.75f));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    for (int blk = 0; blk < 8; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.03f * static_cast<float>(blk)),
            ramp(kFrames, 0.5f + 0.02f * static_cast<float>(blk))};
        INFO("latency " << latency << " block " << blk);
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }
}

}  // namespace

TEST_CASE("Translated routing matches SignalGraph: latency diamond, sub-block delay",
          "[host][graph][executor][routing][parity][plugin][pdc]") {
    run_latency_diamond_parity(/*latency=*/64);  // < kFrames (128)
}

TEST_CASE("Translated routing matches SignalGraph: latency diamond, supra-block delay",
          "[host][graph][executor][routing][parity][plugin][pdc]") {
    run_latency_diamond_parity(/*latency=*/200);  // > kFrames (128): ring wraps
}

TEST_CASE("Translated routing matches SignalGraph: latency diamond, mixed block sizes",
          "[host][graph][executor][routing][parity][plugin][pdc]") {
    // The ring is sized delay + max_frames precisely so a runtime block shorter
    // than the prepared maximum still wraps correctly. Prepare at kFrames but
    // drive blocks of varying length (including sub-max), requiring block-for-
    // block equality so the shorter-block ring advance is exercised.
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2, /*latency=*/96),
        2, 2, "LatencyP");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(p, c, out, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.75f));
    REQUIRE(g.prepare(kSr, kFrames));  // max_block = kFrames (128)
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    const std::array<int, 6> block_sizes{64, 128, 32, 100, 64, 17};
    for (std::size_t b = 0; b < block_sizes.size(); ++b) {
        const int frames = block_sizes[b];
        const std::vector<std::vector<float>> input{
            ramp(frames, 0.7f + 0.05f * static_cast<float>(b)),
            ramp(frames, 0.5f + 0.04f * static_cast<float>(b))};
        INFO("block " << b << " frames " << frames);
        expect_equal(run_legacy(g, frames, input, 2),
                     run_routed(exec, routing, frames, input, 2));
    }
}

TEST_CASE("SignalGraph::process PDC executor path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety][plugin][pdc]") {
    // The per-connection delay ring is sized at prepare(); the gather only does
    // bounded index arithmetic and copies over it. Exercise the delayed-edge
    // branch (a latency diamond) under the allocation probe.
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2, /*latency=*/200),
        2, 2, "LatencyP");
    const auto gain = g.add_gain_node("G");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(p, c, out, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.set_node_gain(gain, 0.75f));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    const auto l = ramp(kFrames, 0.8f), r = ramp(kFrames, 0.6f);
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);

    g.process(ov, iv, kFrames);  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("Translated routing matches SignalGraph: sidechain edges",
          "[host][graph][executor][routing][parity][plugin][sidechain]") {
    // A plugin with 2 main + 2 sidechain input ports reads its sidechain
    // channels (out = main*scale + sidechain). The sidechain source is a
    // separately-scaled copy of the input so main != sidechain. The routed
    // gather must fill the plugin's higher input ports from the sidechain edges
    // exactly as SignalGraph does.
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto sc = g.add_gain_node("SCGain");
    const auto p = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kSidechainMix, 0.5f, /*in_ch=*/4, /*out_ch=*/2),
        4, 2, "MixP");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));                       // main inputs
        REQUIRE(g.connect(in, c, sc, c));                      // feed the sc source
        REQUIRE(g.connect_sidechain(sc, c, p, c + 2));         // sidechain inputs
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.set_node_gain(sc, 0.75f));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.8f + 0.02f * static_cast<float>(blk)),
            ramp(kFrames, 0.6f - 0.02f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }
}

TEST_CASE("Translated routing matches SignalGraph: sidechain edge with PDC delay",
          "[host][graph][executor][routing][parity][plugin][sidechain][pdc]") {
    // The main path runs through a latency-reporting plugin, so the plugin
    // mixing in the sidechain sees its main inputs at latency 64 and its
    // sidechain inputs (a plain gain path) at latency 0 — the sidechain edges
    // must be PDC-delayed by 64 to time-align. Exercises PDC on sidechain edges;
    // requires block-for-block parity (the delay ring carries state).
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto latP = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kFull, 0.5f, 2, 2, /*latency=*/64),
        2, 2, "LatP");
    const auto sc = g.add_gain_node("SCGain");
    const auto mixP = g.add_plugin_node(
        std::make_unique<DeterministicPlugin>(
            DeterministicPlugin::Mode::kSidechainMix, 1.0f, /*in_ch=*/4, /*out_ch=*/2),
        4, 2, "MixP");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, latP, c));
        REQUIRE(g.connect(latP, c, mixP, c));                  // main (latency 64)
        REQUIRE(g.connect(in, c, sc, c));
        REQUIRE(g.connect_sidechain(sc, c, mixP, c + 2));      // sidechain (latency 0)
        REQUIRE(g.connect(mixP, c, out, c));
    }
    REQUIRE(g.set_node_gain(sc, 0.75f));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    for (int blk = 0; blk < 8; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.03f * static_cast<float>(blk)),
            ramp(kFrames, 0.5f + 0.02f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(g, kFrames, input, 2),
                     run_routed(exec, routing, kFrames, input, 2));
    }
}

namespace {

// Drive one block with a 1-channel silent audio bus (graphs whose only purpose
// is MIDI routing still need a valid block; the audio bus is unused).
void process_silent_block(SignalGraph& g) {
    std::vector<float> in(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames), 0.0f);
    std::array<const float*, 1> ic{in.data()};
    std::array<float*, 1> oc{out.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 1, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 1, kFrames);
    g.process(ov, iv, kFrames);
}

std::vector<MidiEvtKey> extract_collected(const SignalGraph& g, pulp::host::NodeId out_node) {
    pulp::midi::MidiBuffer buf;
    buf.reserve(64, 8, 256);
    REQUIRE(const_cast<SignalGraph&>(g).extract_midi(out_node, buf));
    return collect_midi(buf);
}

} // namespace

TEST_CASE("SignalGraph::process MIDI routing matches legacy: MidiInput -> MidiOutput",
          "[host][graph][executor][routing][parity][midi]") {
    // Pure MIDI edge routing: injected events flow MidiInput -> MidiOutput with
    // no plugin. The routed path's MIDI gather must deliver the same events to
    // the output mailbox that the legacy walk does.
    auto build = [](SignalGraph& g, bool route) {
        const auto mi = g.add_midi_input_node("MIDI In");
        const auto mo = g.add_midi_output_node("MIDI Out");
        REQUIRE(g.connect_midi(mi, mo));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
        return std::pair<pulp::host::NodeId, pulp::host::NodeId>{mi, mo};
    };
    SignalGraph legacy, routed;
    const auto [lmi, lmo] = build(legacy, false);
    const auto [rmi, rmo] = build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));

    for (int blk = 0; blk < 4; ++blk) {
        const auto inj = make_injected_midi(blk);
        REQUIRE(legacy.inject_midi(lmi, inj));
        REQUIRE(routed.inject_midi(rmi, inj));
        process_silent_block(legacy);
        process_silent_block(routed);
        INFO("block " << blk);
        expect_midi_equal(extract_collected(legacy, lmo), extract_collected(routed, rmo));
    }
}

TEST_CASE("SignalGraph::process MIDI routing matches legacy: overflow reports incomplete",
          "[host][graph][executor][routing][parity][midi]") {
    // Injecting more events than the per-node MIDI capacity (1024) overflows the
    // mailbox snapshot; both walks must deliver the same truncated events AND
    // report the same incomplete status through extract_midi()'s return value.
    auto build = [](SignalGraph& g, bool route) {
        const auto mi = g.add_midi_input_node("MIDI In");
        const auto mo = g.add_midi_output_node("MIDI Out");
        REQUIRE(g.connect_midi(mi, mo));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
        return std::pair<pulp::host::NodeId, pulp::host::NodeId>{mi, mo};
    };
    SignalGraph legacy, routed;
    const auto [lmi, lmo] = build(legacy, false);
    const auto [rmi, rmo] = build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));

    pulp::midi::MidiBuffer flood;
    flood.reserve(4096, 8, 256);
    for (int i = 0; i < 2000; ++i) {
        flood.add(pulp::midi::MidiEvent::note_on(0, static_cast<uint8_t>(i & 0x7F), 100));
    }
    // inject_midi returns false when the flood overflows the mailbox, but still
    // publishes the truncated + incomplete snapshot; both paths see the same.
    CHECK(legacy.inject_midi(lmi, flood) == routed.inject_midi(rmi, flood));
    process_silent_block(legacy);
    process_silent_block(routed);

    pulp::midi::MidiBuffer legacy_out, routed_out;
    legacy_out.reserve(4096, 8, 256);
    routed_out.reserve(4096, 8, 256);
    const bool legacy_complete = legacy.extract_midi(lmo, legacy_out);
    const bool routed_complete = routed.extract_midi(rmo, routed_out);
    CHECK(legacy_complete == routed_complete);   // both incomplete under overflow
    expect_midi_equal(collect_midi(legacy_out), collect_midi(routed_out));
}

TEST_CASE("SignalGraph::process MIDI routing matches legacy: MidiInput -> plugin -> MidiOutput",
          "[host][graph][executor][routing][parity][midi][plugin]") {
    // Audio and MIDI through one plugin: audio in -> plugin -> out, and
    // MIDI in -> plugin (transpose +7) -> MIDI out. Both the audio output and
    // the transposed MIDI at the sink must match the legacy walk block-for-block.
    struct Ids { pulp::host::NodeId mi, mo; };
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(
            std::make_unique<MidiTransposePlugin>(7, 2, 2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        const auto mi = g.add_midi_input_node("MIDI In");
        const auto mo = g.add_midi_output_node("MIDI Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_midi(mi, p));
        REQUIRE(g.connect_midi(p, mo));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
        return Ids{mi, mo};
    };
    SignalGraph legacy, routed;
    const auto lids = build(legacy, false);
    const auto rids = build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));

    for (int blk = 0; blk < 4; ++blk) {
        const auto inj = make_injected_midi(blk);
        REQUIRE(legacy.inject_midi(lids.mi, inj));
        REQUIRE(routed.inject_midi(rids.mi, inj));

        const auto x = ramp(kFrames, 0.7f + 0.05f * static_cast<float>(blk));
        const auto y = ramp(kFrames, 0.5f - 0.03f * static_cast<float>(blk));
        const std::vector<std::vector<float>> input{x, y};
        INFO("block " << blk);
        // run_legacy drives g.process(); the routed graph dispatches through the
        // executor (routing enabled), the other through the legacy walk.
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
        expect_midi_equal(extract_collected(legacy, lids.mo),
                          extract_collected(routed, rids.mo));
    }
}

TEST_CASE("SignalGraph::process MIDI routing matches legacy: plugin -> plugin MIDI chain",
          "[host][graph][executor][routing][parity][midi][plugin]") {
    // MIDI flows through two plugins in series (transpose +5 then +3); the
    // executor's MIDI gather must carry each plugin's MIDI output into the next.
    struct Ids { pulp::host::NodeId mi, mo; };
    auto build = [](SignalGraph& g, bool route) {
        const auto p1 = g.add_plugin_node(
            std::make_unique<MidiTransposePlugin>(5, 0, 0), 0, 0, "P1");
        const auto p2 = g.add_plugin_node(
            std::make_unique<MidiTransposePlugin>(3, 0, 0), 0, 0, "P2");
        const auto mi = g.add_midi_input_node("MIDI In");
        const auto mo = g.add_midi_output_node("MIDI Out");
        REQUIRE(g.connect_midi(mi, p1));
        REQUIRE(g.connect_midi(p1, p2));
        REQUIRE(g.connect_midi(p2, mo));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
        return Ids{mi, mo};
    };
    SignalGraph legacy, routed;
    const auto lids = build(legacy, false);
    const auto rids = build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));

    for (int blk = 0; blk < 4; ++blk) {
        const auto inj = make_injected_midi(blk);
        REQUIRE(legacy.inject_midi(lids.mi, inj));
        REQUIRE(routed.inject_midi(rids.mi, inj));
        process_silent_block(legacy);
        process_silent_block(routed);
        INFO("block " << blk);
        expect_midi_equal(extract_collected(legacy, lids.mo),
                          extract_collected(routed, rids.mo));
    }
}

TEST_CASE("SignalGraph::process MIDI executor path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety][midi]") {
    SignalGraph g;
    const auto p = g.add_plugin_node(
        std::make_unique<MidiTransposePlugin>(7, 0, 0), 0, 0, "P");
    const auto mi = g.add_midi_input_node("MIDI In");
    const auto mo = g.add_midi_output_node("MIDI Out");
    REQUIRE(g.connect_midi(mi, p));
    REQUIRE(g.connect_midi(p, mo));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    // Pre-allocate the audio bus OUTSIDE the probe (the graph is MIDI-only; the
    // 1-channel bus is unused but process() needs a valid block).
    std::vector<float> in(static_cast<std::size_t>(kFrames), 0.0f);
    std::vector<float> out(static_cast<std::size_t>(kFrames), 0.0f);
    std::array<const float*, 1> ic{in.data()};
    std::array<float*, 1> oc{out.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 1, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 1, kFrames);

    // Warm up the routed event path (mailbox read, gather, plugin emit, publish)
    // outside the probe, then inject immediately before the probed block so it
    // actually carries events (process() clears the MIDI input each block).
    REQUIRE(g.inject_midi(mi, make_injected_midi(1)));
    g.process(ov, iv, kFrames);
    REQUIRE(g.inject_midi(mi, make_injected_midi(2)));
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    pulp::midi::MidiBuffer drained;
    drained.reserve(64, 8, 256);
    static_cast<void>(g.extract_midi(mo, drained));
}

TEST_CASE("SignalGraph::process automation matches legacy: single sparse edge",
          "[host][graph][executor][routing][parity][automation]") {
    // in:0 drives the plugin's gain parameter (range [0,2]); the plugin applies
    // it to the audio. The routed automation gather must build the same two
    // control points the legacy walk does, so the audio output matches.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_automation(in, 0, p, 1, 0.0f, 2.0f, 0.0f,
                                     pulp::host::AutomationMix::Replace));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.05f * static_cast<float>(blk)),
            ramp(kFrames, 0.4f + 0.03f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: per-source slew across blocks",
          "[host][graph][executor][routing][parity][automation]") {
    // A non-zero smoothing time engages the per-source slew, whose state persists
    // across blocks. Drive several blocks with changing source values and require
    // block-for-block parity so the slew state is exercised.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_automation(in, 0, p, 1, 0.0f, 2.0f, /*smoothing_ms=*/5.0f,
                                     pulp::host::AutomationMix::Replace));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 8; ++blk) {
        // Alternate the source level hard so the slew must ramp, not snap.
        const float lvl = (blk % 2 == 0) ? 0.1f : 0.9f;
        std::vector<float> a(static_cast<std::size_t>(kFrames), lvl);
        std::vector<float> b = ramp(kFrames, 0.5f);
        const std::vector<std::vector<float>> input{a, b};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: two-source Add mix",
          "[host][graph][executor][routing][parity][automation]") {
    // Two automation sources (in:0, in:1) sum into the same parameter with Add
    // mix; the accumulate order + bounds clamp must match the legacy walk.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_automation(in, 0, p, 1, 0.0f, 1.5f, 0.0f,
                                     pulp::host::AutomationMix::Add));
        REQUIRE(g.connect_automation(in, 1, p, 1, 0.0f, 1.5f, 0.0f,
                                     pulp::host::AutomationMix::Add));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.04f * static_cast<float>(blk)),
            ramp(kFrames, 0.8f - 0.05f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: dense audio-rate modulation",
          "[host][graph][executor][routing][parity][automation]") {
    // in:0 drives the plugin's audio-rate parameter (id 2) per sample; the plugin
    // applies it sample-by-sample. The routed dense gather must build the same
    // per-sample parameter events the legacy walk does.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_audio_rate_modulation(in, 0, p, 2, 0.0f, 2.0f, 0.0f,
                                                pulp::host::AutomationMix::Replace));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.05f * static_cast<float>(blk)),
            ramp(kFrames, 0.4f + 0.03f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: dense audio-rate with PDC delay",
          "[host][graph][executor][routing][parity][automation][pdc]") {
    // The dense modulation source runs through a latency-64 plugin, so the
    // audio-rate edge into the mixing plugin is PDC-delayed by 64. Drive several
    // blocks (the delay ring carries state) and require block-for-block parity.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto latP = g.add_plugin_node(
            std::make_unique<DeterministicPlugin>(
                DeterministicPlugin::Mode::kFull, 1.0f, 2, 2, /*latency=*/64),
            2, 2, "LatP");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, latP, c));
            REQUIRE(g.connect(latP, c, p, c));  // main audio (latency 64)
            REQUIRE(g.connect(p, c, out, c));
        }
        // Dense modulation from the plain (0-latency) input -> p's audio-rate
        // param; p.input_latency=64, so this edge is delay-compensated by 64.
        REQUIRE(g.connect_audio_rate_modulation(in, 0, p, 2, 0.0f, 2.0f, 0.0f,
                                                pulp::host::AutomationMix::Replace));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 8; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.02f * static_cast<float>(blk)),
            ramp(kFrames, 0.5f + 0.02f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process automation executor path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety][automation]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.connect_automation(in, 0, p, 1, 0.0f, 2.0f, /*smoothing_ms=*/5.0f,
                                 pulp::host::AutomationMix::Replace));
    // Also a dense audio-rate edge so the probe covers the per-sample gather.
    REQUIRE(g.connect_audio_rate_modulation(in, 1, p, 2, 0.0f, 2.0f, 0.0f,
                                            pulp::host::AutomationMix::Replace));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    const auto l = ramp(kFrames, 0.8f), r = ramp(kFrames, 0.6f);
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);

    g.process(ov, iv, kFrames);  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("SignalGraph::process routes any per-node SPARSE automation param count",
          "[host][graph][executor][routing][parity][automation]") {
    // A node automating more distinct SPARSE parameters than the old fixed
    // on-stack cap (64) once forced the legacy walk; the gather now sizes its
    // per-node accumulator off-RT to the actual count, so an arbitrary count
    // routes with output bit-identical to the walk. ManyParamSumGainPlugin's
    // output sums every parameter, so a dropped param would diverge.
    auto build = [](SignalGraph& g, int nparams, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p =
            g.add_plugin_node(std::make_unique<ManyParamSumGainPlugin>(nparams, 2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        // Distinct param ids 1..nparams, each a sparse (control-rate) edge with a
        // small per-param smoothing so the slew state is exercised too.
        for (int i = 0; i < nparams; ++i) {
            REQUIRE(g.connect_automation(in, i % 2, p, static_cast<uint32_t>(i + 1), 0.0f,
                                         1.0f, /*smoothing_ms=*/3.0f,
                                         pulp::host::AutomationMix::Replace));
        }
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };

    // Just past the old fixed cap (65) and a comfortably larger count (100).
    for (int nparams : {65, 100}) {
        SignalGraph legacy, routed;
        build(legacy, nparams, /*route=*/false);
        build(routed, nparams, /*route=*/true);
        REQUIRE(signal_graph_executor_eligible(routed));
        for (int blk = 0; blk < 4; ++blk) {
            const std::vector<std::vector<float>> input{
                ramp(kFrames, 0.6f + 0.04f * static_cast<float>(blk)),
                ramp(kFrames, 0.5f - 0.02f * static_cast<float>(blk))};
            INFO("nparams " << nparams << " block " << blk);
            expect_equal(run_legacy(legacy, kFrames, input, 2),
                         run_graph_process(routed, kFrames, input, 2));
        }
    }

    // The routed gather is allocation-free on the audio thread even well past
    // the old cap (all accumulator storage was sized off-RT in reset()). Build
    // the I/O buffers OUTSIDE the probe and call g.process() directly inside, so
    // the probe sees only the engine's work (not the harness's buffer setup).
    {
        SignalGraph g;
        build(g, 100, /*route=*/true);
        REQUIRE(signal_graph_executor_eligible(g));
        const auto l = ramp(kFrames, 0.7f), r = ramp(kFrames, 0.6f);
        std::vector<float> li = l, ri = r;
        std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 2> ic{li.data(), ri.data()};
        std::array<float*, 2> oc{lo.data(), ro.data()};
        pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
        pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
        g.process(ov, iv, kFrames);  // warm-up outside the probe
        {
            pulp::test::RtAllocationProbe probe;
            g.process(ov, iv, kFrames);
            REQUIRE_FALSE(probe.saw_allocation());
        }
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: source also drives audio",
          "[host][graph][executor][routing][parity][automation]") {
    // A plugin's audio output both feeds a downstream plugin's audio input AND
    // drives its gain parameter. The source's output slot must stay live for the
    // automation gather to read it, and the routed output must match the walk.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto a = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "A");
        const auto b = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "B");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, a, c));
            REQUIRE(g.connect(a, c, b, c));   // A's audio -> B's audio
            REQUIRE(g.connect(b, c, out, c));
        }
        REQUIRE(g.connect_automation(a, 0, b, 1, 0.0f, 2.0f, 0.0f,  // A:0 -> B's gain param
                                     pulp::host::AutomationMix::Replace));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.04f * static_cast<float>(blk)),
            ramp(kFrames, 0.5f + 0.03f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process routes any per-node DENSE automation param count",
          "[host][graph][executor][routing][parity][automation]") {
    // The dense (audio-rate) gather's per-param accumulators were also a fixed
    // on-stack array; they now live in per-node scratch sized off-RT, so an
    // arbitrary distinct audio-rate param count routes bit-identically to the
    // walk. This PINS the dense-array move to per-node storage (a stack overflow
    // before the fix). A small block keeps the per-sample event count
    // (params * block) inside the parameter-event-queue capacity.
    constexpr int kSmall = 8;
    auto build = [kSmall](SignalGraph& g, int nparams, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p =
            g.add_plugin_node(std::make_unique<ManyParamSumGainPlugin>(nparams, 2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        for (int i = 0; i < nparams; ++i) {
            REQUIRE(g.connect_audio_rate_modulation(in, i % 2, p, static_cast<uint32_t>(i + 1),
                                                    0.0f, 1.0f, 0.0f,
                                                    pulp::host::AutomationMix::Replace));
        }
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kSmall));
    };

    // Just past the old fixed cap (65) and a larger count (100); 100 * 8 = 800
    // per-sample events stays within the per-node event-queue capacity.
    for (int nparams : {65, 100}) {
        SignalGraph legacy, routed;
        build(legacy, nparams, /*route=*/false);
        build(routed, nparams, /*route=*/true);
        REQUIRE(signal_graph_executor_eligible(routed));
        for (int blk = 0; blk < 4; ++blk) {
            const std::vector<std::vector<float>> input{
                ramp(kSmall, 0.6f + 0.04f * static_cast<float>(blk)),
                ramp(kSmall, 0.5f - 0.02f * static_cast<float>(blk))};
            INFO("nparams " << nparams << " block " << blk);
            expect_equal(run_legacy(legacy, kSmall, input, 2),
                         run_graph_process(routed, kSmall, input, 2));
        }
    }
}

TEST_CASE("SignalGraph::process parallel path routes large per-node automation counts",
          "[host][graph][executor][routing][parity][parallel][automation]") {
    // Several plugin nodes at the same graph level, each automating more distinct
    // parameters than the old fixed cap, run concurrently on the parallel path.
    // Each node's automation gather touches only its own per-node accumulator
    // slice; the parallel-routed output must stay bit-identical to the legacy
    // walk across blocks — locking the per-node-slice parallel-safety of both the
    // sparse and dense accumulators.
    constexpr int kSmall = 8;
    constexpr int kPlugins = 4;
    constexpr int kParams = 100;  // well past the old fixed cap (64)
    auto build = [kPlugins, kParams, kSmall](SignalGraph& g, bool dense, bool parallel) {
        const auto in = g.add_input_node(2, "In");
        const auto out = g.add_output_node(2, "Out");
        for (int k = 0; k < kPlugins; ++k) {
            const auto p = g.add_plugin_node(
                std::make_unique<ManyParamSumGainPlugin>(kParams, 2), 2, 2, "P");
            for (int c = 0; c < 2; ++c) {
                REQUIRE(g.connect(in, c, p, c));
                REQUIRE(g.connect(p, c, out, c));
            }
            for (int i = 0; i < kParams; ++i) {
                const auto pid = static_cast<uint32_t>(i + 1);
                if (dense) {
                    REQUIRE(g.connect_audio_rate_modulation(in, i % 2, p, pid, 0.0f, 1.0f, 0.0f,
                                                            pulp::host::AutomationMix::Replace));
                } else {
                    REQUIRE(g.connect_automation(in, i % 2, p, pid, 0.0f, 1.0f, 3.0f,
                                                 pulp::host::AutomationMix::Replace));
                }
            }
        }
        g.set_parallel_routing_enabled(parallel);
        REQUIRE(g.prepare(kSr, dense ? kSmall : kFrames));
    };

    for (bool dense : {false, true}) {
        const int frames = dense ? kSmall : kFrames;
        SignalGraph legacy, par;
        build(legacy, dense, /*parallel=*/false);
        build(par, dense, /*parallel=*/true);
        REQUIRE(signal_graph_executor_eligible(par));
        for (int blk = 0; blk < 4; ++blk) {
            const std::vector<std::vector<float>> input{
                ramp(frames, 0.6f + 0.03f * static_cast<float>(blk)),
                ramp(frames, 0.5f - 0.02f * static_cast<float>(blk))};
            INFO((dense ? "dense" : "sparse") << " block " << blk);
            expect_equal(run_legacy(legacy, frames, input, 2),
                         run_graph_process(par, frames, input, 2));
        }
        // Guard the test's premise: the worker path must have actually dispatched
        // a parallel level (4 plugins at one level), so this stays a parallel-vs-
        // walk check and can't silently degrade to serial if a default/threshold
        // changes.
        REQUIRE(par.routing_executor_stats().parallel_levels_dispatched > 0);
    }
}

TEST_CASE("SignalGraph::process automation matches legacy: dense Replace + Add on one param",
          "[host][graph][executor][routing][parity][automation]") {
    // Two dense edges into the same audio-rate parameter — one Replace, one Add —
    // so the per-sample accumulate order + bounds clamp must match the walk.
    auto build = [](SignalGraph& g, bool route) {
        const auto in = g.add_input_node(2, "In");
        const auto p = g.add_plugin_node(std::make_unique<AutomatedGainPlugin>(2), 2, 2, "P");
        const auto out = g.add_output_node(2, "Out");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, p, c));
            REQUIRE(g.connect(p, c, out, c));
        }
        REQUIRE(g.connect_audio_rate_modulation(in, 0, p, 2, 0.0f, 1.2f, 0.0f,
                                                pulp::host::AutomationMix::Replace));
        REQUIRE(g.connect_audio_rate_modulation(in, 1, p, 2, 0.0f, 1.2f, 0.0f,
                                                pulp::host::AutomationMix::Add));
        g.set_canonical_executor_routing_enabled(route);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph legacy, routed;
    build(legacy, false);
    build(routed, true);
    REQUIRE(signal_graph_executor_eligible(routed));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.03f * static_cast<float>(blk)),
            ramp(kFrames, 0.6f - 0.02f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(routed, kFrames, input, 2));
    }
}

namespace {

// in(2ch) -> N parallel gains -> out(2ch). A wide level so the parallel executor
// actually dispatches across workers. `parallel` opts into the parallel path.
void build_wide_signal_graph(SignalGraph& g, int n, bool parallel) {
    const auto in = g.add_input_node(2, "In");
    const auto out = g.add_output_node(2, "Out");
    for (int i = 0; i < n; ++i) {
        const auto gn = g.add_gain_node("G");
        for (int c = 0; c < 2; ++c) {
            REQUIRE(g.connect(in, c, gn, c));
            REQUIRE(g.connect(gn, c, out, c));
        }
        REQUIRE(g.set_node_gain(gn, 0.02f + 0.003f * static_cast<float>(i)));
    }
    g.set_parallel_routing_enabled(parallel);
    REQUIRE(g.prepare(kSr, kFrames));
}

}  // namespace

TEST_CASE("SignalGraph::process parallel path matches legacy: wide graph",
          "[host][graph][executor][routing][parity][parallel]") {
    SignalGraph legacy, par;
    build_wide_signal_graph(legacy, 16, /*parallel=*/false);
    build_wide_signal_graph(par, 16, /*parallel=*/true);
    REQUIRE(signal_graph_executor_eligible(par));
    for (int blk = 0; blk < 4; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.7f + 0.04f * static_cast<float>(blk)),
            ramp(kFrames, 0.5f - 0.02f * static_cast<float>(blk))};
        INFO("block " << blk);
        expect_equal(run_legacy(legacy, kFrames, input, 2),
                     run_legacy(par, kFrames, input, 2));
    }
}

TEST_CASE("SignalGraph::process parallel path matches legacy: feedback across blocks",
          "[host][graph][executor][routing][parity][parallel]") {
    // The parallel executor captures feedback after the level walk (serial), so a
    // feedback graph must match the legacy walk block-for-block on the parallel path.
    SignalGraph legacy, par;
    build_feedback_graph(legacy, 0.5f, /*route_executor=*/false);
    par.set_parallel_routing_enabled(true);
    {
        const auto in = par.add_input_node(1, "In");
        const auto a = par.add_gain_node("A");
        const auto out = par.add_output_node(1, "Out");
        REQUIRE(par.connect(in, 0, a, 0));
        REQUIRE(par.connect(a, 0, out, 0));
        REQUIRE(par.connect_feedback(a, 0, a, 0));
        REQUIRE(par.set_node_gain(a, 0.5f));
        REQUIRE(par.prepare(kSr, kFrames));
    }
    REQUIRE(signal_graph_executor_eligible(par));
    for (int blk = 0; blk < 6; ++blk) {
        const auto x = ramp(kFrames, 0.4f + 0.1f * static_cast<float>(blk));
        expect_equal(run_legacy(legacy, kFrames, {x}, 1),
                     run_legacy(par, kFrames, {x}, 1));
    }
}

TEST_CASE("SignalGraph::process parallel path does not allocate on the audio thread",
          "[host][graph][executor][routing][rt-safety][parallel]") {
    SignalGraph g;
    build_wide_signal_graph(g, 16, /*parallel=*/true);
    REQUIRE(signal_graph_executor_eligible(g));
    const auto l = ramp(kFrames, 0.8f), r = ramp(kFrames, 0.6f);
    std::vector<float> li = l, ri = r, lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> ic{li.data(), ri.data()};
    std::array<float*, 2> oc{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);  // warm up the workers outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("SignalGraph re-prepare while the parallel path renders is race-free",
          "[host][graph][executor][routing][rt-safety][threads][parallel]") {
    // Hammer process() (parallel path, multi-threaded) on the audio thread while
    // another thread re-prepares. The worker pool is a long-lived member (not
    // restarted on same-worker-count re-prepares), and each routed snapshot/pool
    // rides the RCU lifetime — surfaces a pool-lifecycle or retired-snapshot race
    // under TSan.
    SignalGraph g;
    build_wide_signal_graph(g, 8, /*parallel=*/true);

    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    std::atomic<std::uint64_t> blocks{0};
    std::thread audio([&] {
        std::vector<float> li(kFrames, 0.3f), ri(kFrames, 0.2f);
        std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 2> ic{li.data(), ri.data()};
        std::array<float*, 2> oc{lo.data(), ro.data()};
        pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
        pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
        started.store(true, std::memory_order_release);
        while (!stop.load(std::memory_order_relaxed)) {
            g.process(ov, iv, kFrames);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
    bool prepared_all = true;
    for (int i = 0; i < 200; ++i) {
        if (!g.prepare(kSr, kFrames)) prepared_all = false;
    }
    stop.store(true, std::memory_order_relaxed);
    audio.join();
    REQUIRE(prepared_all);
    CHECK(blocks.load() > 0);
}

namespace {

// in(2ch) -> 3 independent gain chains (chain A has one-block feedback, so the
// graph is STATEFUL across blocks) -> all three sum into out(2ch). The three
// chains share one parallel level (independent, fed from input); the output node
// accumulates 3 incoming connections per channel, so its level runs serially in
// topo order. Exercises parallel dispatch AND the multi-sink float-reduction
// order whose determinism this golden locks. `parallel` opts into the pool path.
void build_multisink_stateful(SignalGraph& g, bool parallel) {
    const auto in = g.add_input_node(2, "In");
    const auto out = g.add_output_node(2, "Out");
    const auto a = g.add_gain_node("A");
    const auto b = g.add_gain_node("B");
    const auto c = g.add_gain_node("C");
    for (int ch = 0; ch < 2; ++ch) {
        REQUIRE(g.connect(in, ch, a, ch));
        REQUIRE(g.connect(in, ch, b, ch));
        REQUIRE(g.connect(in, ch, c, ch));
        REQUIRE(g.connect(a, ch, out, ch));
        REQUIRE(g.connect(b, ch, out, ch));
        REQUIRE(g.connect(c, ch, out, ch));
        REQUIRE(g.connect_feedback(a, ch, a, ch));  // chain A carries state
    }
    REQUIRE(g.set_node_gain(a, 0.31f));
    REQUIRE(g.set_node_gain(b, 0.52f));
    REQUIRE(g.set_node_gain(c, 0.17f));
    g.set_parallel_routing_enabled(parallel);
    REQUIRE(g.prepare(kSr, kFrames));
    // Guard against a vacuous legacy-vs-legacy comparison: the topology must be
    // eligible so the parallel session genuinely routes through the worker pool.
    REQUIRE(signal_graph_executor_eligible(g));
}

// Render a fixed multi-block input sequence through a fresh graph, returning the
// concatenated per-channel output (state carried block to block).
std::vector<std::vector<float>> render_session(bool parallel, int blocks) {
    SignalGraph g;
    build_multisink_stateful(g, parallel);
    std::vector<std::vector<float>> seq(2);
    for (int blk = 0; blk < blocks; ++blk) {
        const std::vector<std::vector<float>> input{
            ramp(kFrames, 0.6f + 0.05f * static_cast<float>(blk)),
            ramp(kFrames, 0.4f + 0.03f * static_cast<float>(blk))};
        const auto out = run_legacy(g, kFrames, input, 2);
        for (int ch = 0; ch < 2; ++ch)
            seq[ch].insert(seq[ch].end(), out[ch].begin(), out[ch].end());
    }
    return seq;
}

}  // namespace

TEST_CASE("Golden: parallel multi-sink stateful graph renders identically run-to-run",
          "[host][graph][executor][routing][parallel][golden][determinism]") {
    // Two independent sessions (fresh graph each) over the SAME input sequence on
    // the parallel path must produce bit-identical output: the worker-pool
    // dispatch order must not perturb results, and the multi-sink accumulation
    // order is fixed (AudioOutput levels run serially in topo order — never
    // parallel-reduced). Locks the deterministic-reduction design decision.
    constexpr int kBlocks = 8;
    const auto run1 = render_session(/*parallel=*/true, kBlocks);
    const auto run2 = render_session(/*parallel=*/true, kBlocks);
    expect_equal(run1, run2);
    // And the parallel render equals the deterministic legacy walk (cross-engine
    // determinism over the full stateful session, not just one block).
    const auto legacy = render_session(/*parallel=*/false, kBlocks);
    expect_equal(run1, legacy);
}

// ── Custom-node routing parity ───────────────────────────────────────────────
// Custom (CustomNodeType) nodes route through the canonical executor by default,
// just like Plugin nodes. These prove the routed Custom binding is bit-identical
// to SignalGraph's own walk for the three cases that matter: a stateful custom
// node (state advanced per instance across blocks), a stateless one, and an
// unresolved one (the binding's pass-through-or-zero matching the walk).
namespace {

// A stateful custom node: its opaque instance carries a one-pole accumulator
// across blocks, so a routed render that double-ran or skipped state would
// diverge from the walk. Each graph gets its OWN instance (register + add per
// graph), so the walk-graph and routed-graph advance independent state.
struct CustomAccum { float state = 0.0f; };
pulp::host::CustomNodeType make_accum_type() {
    pulp::host::CustomNodeType t;
    t.type_id = "pulp.test.accum";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Accum";
    t.create = []() -> void* { return new CustomAccum(); };
    t.destroy = [](void* p) { delete static_cast<CustomAccum*>(p); };
    t.reset = [](void* p) { static_cast<CustomAccum*>(p)->state = 0.0f; };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& out,
                            const pulp::audio::BufferView<const float>& in, int n) {
        auto* s = static_cast<CustomAccum*>(p);
        for (int i = 0; i < n; ++i) {
            s->state = s->state * 0.75f + in.channel_ptr(0)[i];
            out.channel_ptr(0)[i] = s->state;
        }
    };
    return t;
}

// A stateless custom node: a fixed scale, no instance.
pulp::host::CustomNodeType make_scale_type() {
    pulp::host::CustomNodeType t;
    t.type_id = "pulp.test.scale";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Scale";
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        for (int i = 0; i < n; ++i) out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * 0.625f;
    };
    return t;
}

// A PARTIAL-writing custom node: writes only the first half of each block and
// leaves the tail untouched. A node that does not fully overwrite its output
// relies on a persistent per-node output buffer — the untouched tail must carry
// the PREVIOUS block's samples. Exercises the persistent-output contract for
// custom nodes (regression guard: without it the routed tail would be a recycled
// or zeroed slot while the walk keeps the stale tail).
pulp::host::CustomNodeType make_partial_type() {
    pulp::host::CustomNodeType t;
    t.type_id = "pulp.test.partial";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Partial";
    t.process = [](pulp::audio::BufferView<float>& out,
                   const pulp::audio::BufferView<const float>& in, int n) {
        for (int i = 0; i < n / 2; ++i) out.channel_ptr(0)[i] = in.channel_ptr(0)[i] * 0.5f;
        // [n/2, n) deliberately left untouched — persists across blocks.
    };
    return t;
}

}  // namespace

TEST_CASE("Translated routing matches SignalGraph: stateful custom node parity",
          "[host][graph][executor][routing][parity][custom]") {
    // Two identical graphs over the same per-block input — one forced onto the
    // legacy walk, one routed by default — must agree sample-for-sample across
    // many blocks, proving the routed Custom binding advances the same per-
    // instance state the walk does.
    auto build = [](SignalGraph& g, bool route_executor) {
        REQUIRE(g.register_custom_node_type(make_accum_type()));
        const auto in = g.add_input_node(1, "In");
        const auto node = g.add_custom_node("pulp.test.accum", "Accum");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(node != 0);
        REQUIRE(g.connect(in, 0, node, 0));
        REQUIRE(g.connect(node, 0, out, 0));
        g.set_canonical_executor_routing_enabled(route_executor);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph walk, routed;
    build(walk, /*route_executor=*/false);
    build(routed, /*route_executor=*/true);
    REQUIRE(signal_graph_executor_eligible(routed));

    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        const auto x = ramp(kFrames, 0.4f + 0.1f * static_cast<float>(blk));
        std::vector<float> wi = x, ri = x, wo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 1> wic{wi.data()}, ric{ri.data()};
        std::array<float*, 1> woc{wo.data()}, roc{ro.data()};
        pulp::audio::BufferView<const float> wiv(wic.data(), 1, kFrames),
            riv(ric.data(), 1, kFrames);
        pulp::audio::BufferView<float> wov(woc.data(), 1, kFrames),
            rov(roc.data(), 1, kFrames);
        walk.process(wov, wiv, kFrames);
        routed.process(rov, riv, kFrames);
        for (int i = 0; i < kFrames; ++i)
            REQUIRE(wo[static_cast<std::size_t>(i)] == ro[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Parallel routing matches SignalGraph: wide stateful custom level",
          "[host][graph][executor][routing][parity][custom][parallel]") {
    // A wide level of independent stateful custom nodes forces the levelized
    // parallel executor to run multiple process_instance callbacks on worker
    // threads at once. Each node owns a distinct instance, so the per-instance
    // state must advance exactly as the single-threaded walk does — proving the
    // custom binding is parallel-safe (and, under TSan, race-free).
    constexpr int kWidth = 6;
    auto build = [](SignalGraph& g, bool parallel) {
        REQUIRE(g.register_custom_node_type(make_accum_type()));
        const auto in = g.add_input_node(1, "In");
        const auto out = g.add_output_node(1, "Out");
        for (int i = 0; i < kWidth; ++i) {
            const auto node = g.add_custom_node("pulp.test.accum", "Accum");
            REQUIRE(node != 0);
            REQUIRE(g.connect(in, 0, node, 0));
            REQUIRE(g.connect(node, 0, out, 0));  // fan-in mix at the output
        }
        // Reference runs the legacy walk; the test graph routes across worker
        // threads. Bit-exact agreement proves both the routed Custom binding AND
        // its parallel dispatch match the walk.
        g.set_canonical_executor_routing_enabled(parallel);
        g.set_parallel_routing_enabled(parallel);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph walk, routed;
    build(walk, /*parallel=*/false);    // legacy walk (independent reference)
    build(routed, /*parallel=*/true);   // levelized parallel routed

    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        const auto x = ramp(kFrames, 0.3f + 0.08f * static_cast<float>(blk));
        std::vector<float> wi = x, ri = x, wo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 1> wic{wi.data()}, ric{ri.data()};
        std::array<float*, 1> woc{wo.data()}, roc{ro.data()};
        pulp::audio::BufferView<const float> wiv(wic.data(), 1, kFrames),
            riv(ric.data(), 1, kFrames);
        pulp::audio::BufferView<float> wov(woc.data(), 1, kFrames),
            rov(roc.data(), 1, kFrames);
        walk.process(wov, wiv, kFrames);
        routed.process(rov, riv, kFrames);
        for (int i = 0; i < kFrames; ++i)
            REQUIRE(wo[static_cast<std::size_t>(i)] == ro[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Translated routing matches SignalGraph: partial-writing custom node persists its tail",
          "[host][graph][executor][routing][parity][custom]") {
    // A custom node that overwrites only the first half of its output must, on
    // the routed path, keep the untouched tail from the previous block — exactly
    // as the walk's persistent per-node buffer does. Bit-exact across blocks
    // guards the custom persistent_output contract.
    auto build = [](SignalGraph& g, bool route_executor) {
        REQUIRE(g.register_custom_node_type(make_partial_type()));
        const auto in = g.add_input_node(1, "In");
        const auto node = g.add_custom_node("pulp.test.partial", "Partial");
        const auto out = g.add_output_node(1, "Out");
        REQUIRE(node != 0);
        REQUIRE(g.connect(in, 0, node, 0));
        REQUIRE(g.connect(node, 0, out, 0));
        g.set_canonical_executor_routing_enabled(route_executor);
        REQUIRE(g.prepare(kSr, kFrames));
    };
    SignalGraph walk, routed;
    build(walk, /*route_executor=*/false);
    build(routed, /*route_executor=*/true);

    for (int blk = 0; blk < 6; ++blk) {
        CAPTURE(blk);
        const auto x = ramp(kFrames, 0.5f + 0.09f * static_cast<float>(blk));
        std::vector<float> wi = x, ri = x, wo(kFrames, 0.0f), ro(kFrames, 0.0f);
        std::array<const float*, 1> wic{wi.data()}, ric{ri.data()};
        std::array<float*, 1> woc{wo.data()}, roc{ro.data()};
        pulp::audio::BufferView<const float> wiv(wic.data(), 1, kFrames),
            riv(ric.data(), 1, kFrames);
        pulp::audio::BufferView<float> wov(woc.data(), 1, kFrames),
            rov(roc.data(), 1, kFrames);
        walk.process(wov, wiv, kFrames);
        routed.process(rov, riv, kFrames);
        for (int i = 0; i < kFrames; ++i)
            REQUIRE(wo[static_cast<std::size_t>(i)] == ro[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Translated routing matches SignalGraph: stateless custom node parity",
          "[host][graph][executor][routing][parity][custom]") {
    SignalGraph g;
    REQUIRE(g.register_custom_node_type(make_scale_type()));
    const auto in = g.add_input_node(1, "In");
    const auto node = g.add_custom_node("pulp.test.scale", "Scale");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(node != 0);
    REQUIRE(g.connect(in, 0, node, 0));
    REQUIRE(g.connect(node, 0, out, 0));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.8f)};
    expect_equal(run_legacy(g, kFrames, input, 1),
                 run_routed(exec, routing, kFrames, input, 1));
}

TEST_CASE("Translated routing matches SignalGraph: unresolved custom node pass-through",
          "[host][graph][executor][routing][parity][custom]") {
    // An unresolved custom type (never registered) routes via the binding's
    // pass-through-or-zero, exactly as SignalGraph's walk handles it: input
    // copied straight to output. Both paths must be bit-identical.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto node = g.add_unresolved_custom_node("test.unregistered", 1, 1, 1, "Custom");
    const auto out = g.add_output_node(1, "Out");
    REQUIRE(node != 0);
    REQUIRE(g.connect(in, 0, node, 0));
    REQUIRE(g.connect(node, 0, out, 0));
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE(signal_graph_executor_eligible(g));

    SignalGraphExecutorRouting routing;
    REQUIRE(build_signal_graph_executor_routing(g, routing));
    REQUIRE(routing.valid);
    pulp::format::GraphRuntimeExecutor exec;
    const std::vector<std::vector<float>> input{ramp(kFrames, 0.7f)};
    expect_equal(run_legacy(g, kFrames, input, 1),
                 run_routed(exec, routing, kFrames, input, 1));
}
