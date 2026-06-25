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
    // A stray MIDI node makes the graph ineligible; with the opt-in ON, process()
    // must fall back to the legacy walk and still produce in*gain.
    SignalGraph g;
    const auto in = g.add_input_node(1, "In");
    const auto a = g.add_gain_node("A");
    const auto out = g.add_output_node(1, "Out");
    (void)g.add_midi_input_node("MIDI In");  // ineligible node type present
    REQUIRE(g.connect(in, 0, a, 0));
    REQUIRE(g.connect(a, 0, out, 0));
    REQUIRE(g.set_node_gain(a, 0.5f));
    g.set_canonical_executor_routing_enabled(true);
    REQUIRE(g.prepare(kSr, kFrames));
    CHECK_FALSE(signal_graph_executor_eligible(g));  // MIDI node disqualifies

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

    // A MIDI node is outside the eligible subset even when prepared.
    SignalGraph midi;
    const auto min = midi.add_midi_input_node("MIDI In");
    const auto mout = midi.add_midi_output_node("MIDI Out");
    REQUIRE(midi.connect_midi(min, mout));
    REQUIRE(midi.prepare(kSr, kFrames));
    CHECK_FALSE(signal_graph_executor_eligible(midi));
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
    enum class Mode { kFull, kPartialCh1Gated };
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
