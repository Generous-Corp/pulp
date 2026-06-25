// Convergence proof for the SignalGraph -> canonical-executor translation: for
// graphs in the executor-eligible subset (AudioInput / AudioOutput / Gain,
// plain audio feedforward + feedback), driving the TRANSLATED routing through
// GraphRuntimeExecutor::process_routed() produces output bit-identical to
// SignalGraph's own process() walk. Also checks eligibility gating and that a
// post-prepare set_node_gain() is honored on the routed path (the binding reads
// the live snapshot's atomic).

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <array>
#include <cstdint>
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
