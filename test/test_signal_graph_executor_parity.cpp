// Convergence proof for the SignalGraph -> canonical-executor translation: for
// graphs in the executor-eligible subset (AudioInput / AudioOutput / Gain,
// plain audio feedforward + feedback), driving the TRANSLATED routing through
// GraphRuntimeExecutor::process_routed() produces output bit-identical to
// SignalGraph's own process() walk. Also checks eligibility gating and that a
// post-prepare set_node_gain() is honored on the routed path (the binding reads
// the live snapshot's atomic).

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

TEST_CASE("SignalGraph::process opt-in falls back to legacy for ineligible graphs",
          "[host][graph][executor][routing]") {
    // A stray Custom node makes the graph ineligible; with the opt-in ON,
    // process() must fall back to the legacy walk and still produce in*gain.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    (void)g.add_unresolved_custom_node("test.custom", 1, 1, 1, "Custom");  // ineligible type
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.set_node_gain(a, 0.5f));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    CHECK_FALSE(signal_graph_executor_eligible(g));  // Custom node disqualifies

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

    // A Custom node is outside the eligible subset even when prepared.
    SignalGraph custom;
    const auto cin = custom.add_input_node(1, "In");
    const auto cnode = custom.add_unresolved_custom_node("test.custom", 1, 1, 1, "Custom");
    const auto cout = custom.add_output_node(1, "Out");
    REQUIRE(custom.connect(cin, 0, cnode, 0));
    REQUIRE(custom.connect(cnode, 0, cout, 0));
    REQUIRE(custom.prepare(kSr, kFrames));
    CHECK_FALSE(signal_graph_executor_eligible(custom));
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

// Plugin exposing `num_params` automatable parameters (ids 1..num_params), for
// the per-node automated-parameter cap. Audio passes through unchanged.
class ManyParamPlugin final : public PluginSlot {
public:
    ManyParamPlugin(int num_params, int channels)
        : num_params_(num_params), info_(test_plugin_info(channels, channels)) {}
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
            std::copy_n(in.channel_ptr(c), n, out.channel_ptr(c));
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
            p.rate = pulp::state::ParamRate::AudioRate;  // also dense-modulatable
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

TEST_CASE("Executor eligibility rejects a Plugin node with no live slot",
          "[host][graph][executor][routing][eligibility][plugin]") {
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    // An unresolved plugin node carries metadata but no slot.
    const auto p = g.add_unresolved_plugin_node(test_plugin_info(2, 2), 2, 2, "Missing");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    REQUIRE_FALSE(signal_graph_executor_eligible(g));
    SignalGraphExecutorRouting routing;
    REQUIRE_FALSE(build_signal_graph_executor_routing(g, routing));
    REQUIRE_FALSE(routing.valid);
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

TEST_CASE("SignalGraph::process opt-in falls back to legacy past the per-node automation cap",
          "[host][graph][executor][routing][automation]") {
    // A node automating more distinct parameters than the gather's on-stack
    // accumulator holds must stay ineligible (kept on the legacy walk) rather
    // than route and silently drop the overflow.
    constexpr int kOverCap =
        static_cast<int>(pulp::format::GraphRuntimeAutomationScratch::kMaxParamsPerNode) + 1;
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(std::make_unique<ManyParamPlugin>(kOverCap, 2), 2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    for (int i = 0; i < kOverCap; ++i) {
        REQUIRE(g.connect_automation(in, 0, p, static_cast<uint32_t>(i + 1), 0.0f, 1.0f, 0.0f,
                                     pulp::host::AutomationMix::Add));
    }
    REQUIRE(g.prepare(kSr, kFrames));
    CHECK_FALSE(signal_graph_executor_eligible(g));

    // Exactly at the cap is still eligible.
    SignalGraph at_cap;
    const auto in2 = at_cap.add_input_node(2, "In");
    const auto p2 = at_cap.add_plugin_node(
        std::make_unique<ManyParamPlugin>(kOverCap - 1, 2), 2, 2, "P");
    const auto out2 = at_cap.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(at_cap.connect(in2, c, p2, c));
        REQUIRE(at_cap.connect(p2, c, out2, c));
    }
    for (int i = 0; i < kOverCap - 1; ++i) {
        REQUIRE(at_cap.connect_automation(in2, 0, p2, static_cast<uint32_t>(i + 1), 0.0f, 1.0f,
                                          0.0f, pulp::host::AutomationMix::Add));
    }
    REQUIRE(at_cap.prepare(kSr, kFrames));
    CHECK(signal_graph_executor_eligible(at_cap));
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

TEST_CASE("SignalGraph::process opt-in falls back to legacy past the per-node dense automation cap",
          "[host][graph][executor][routing][automation]") {
    // The dense gather's per-param accumulators are also a fixed on-stack array;
    // a node receiving more distinct audio-rate params than the cap must stay on
    // the legacy walk.
    constexpr int kOverCap =
        static_cast<int>(pulp::format::GraphRuntimeAutomationScratch::kMaxParamsPerNode) + 1;
    SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto p = g.add_plugin_node(std::make_unique<ManyParamPlugin>(kOverCap, 2), 2, 2, "P");
    const auto out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, p, c));
        REQUIRE(g.connect(p, c, out, c));
    }
    for (int i = 0; i < kOverCap; ++i) {
        REQUIRE(g.connect_audio_rate_modulation(in, 0, p, static_cast<uint32_t>(i + 1),
                                                0.0f, 1.0f, 0.0f,
                                                pulp::host::AutomationMix::Add));
    }
    // Prepare with a tiny block so the per-sample events (kOverCap * block) fit
    // the parameter-event-queue capacity (1024) — otherwise prepare rejects the
    // graph before eligibility is queried. With a prepared graph, the per-node
    // dense param cap is the gate that keeps it on the legacy walk.
    REQUIRE(g.prepare(kSr, /*max_block=*/8));
    CHECK_FALSE(signal_graph_executor_eligible(g));
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
