// Phase 4b: prove the canonical GraphRuntimeExecutor's ROUTING path moves audio
// between nodes correctly. Unlike the single-node output-parity baseline
// (test_graph_executor_parity.cpp), this builds a real chain
// (AudioInput -> gain g1 -> gain g2 -> AudioOutput) where node g2's input must
// be node g1's output. The executor gathers each node's inputs from upstream
// output slots through a pre-allocated GraphRuntimeBufferPool, so a routing bug
// (wrong slot, missed gather, wrong order) shows up as a bit-level mismatch
// against pulp::host::SignalGraph driving the same two-gain chain.
//
// Also asserts the routing process() path is allocation-free on the RT thread.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace {

using pulp::format::BusBufferSet;
using pulp::format::BusDirection;
using pulp::format::BusRole;
using pulp::format::GraphRuntimeBufferPool;
using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeNodeProcessContext;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::ProcessBlock;
using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;

struct GainState {
    float gain = 1.0f;
};

// Routing-path gain binding: reads the gathered per-node input view, writes the
// per-node output view. No knowledge of buses or other nodes — that is the
// executor's routing job.
bool routing_gain(ProcessBlock&,
                  const GraphRuntimeNodeProcessContext& ctx,
                  void* user_data) noexcept {
    const float gain = static_cast<const GainState*>(user_data)->gain;
    const auto& in = ctx.node_inputs;
    auto out = ctx.node_outputs;  // copy the view so channel_ptr() yields float*
    const std::size_t chs = std::min(in.num_channels(), out.num_channels());
    const std::size_t frames = out.num_samples();
    for (std::size_t c = 0; c < chs; ++c) {
        const float* ip = in.channel_ptr(c);
        float* op = out.channel_ptr(c);
        for (std::size_t i = 0; i < frames; ++i) op[i] = ip[i] * gain;
    }
    return true;
}

std::vector<float> test_signal(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

// Reference: input -> gain g1 -> gain g2 -> output through the host graph.
std::vector<float> run_signal_graph_chain(float g1, float g2, double sr, int frames,
                                          const std::vector<float>& l,
                                          const std::vector<float>& r) {
    pulp::host::SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto gn1 = g.add_gain_node("G1");
    const auto gn2 = g.add_gain_node("G2");
    const auto out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(in, 0, gn1, 0));
    REQUIRE(g.connect(in, 1, gn1, 1));
    REQUIRE(g.connect(gn1, 0, gn2, 0));
    REQUIRE(g.connect(gn1, 1, gn2, 1));
    REQUIRE(g.connect(gn2, 0, out, 0));
    REQUIRE(g.connect(gn2, 1, out, 1));
    REQUIRE(g.set_node_gain(gn1, g1));
    REQUIRE(g.set_node_gain(gn2, g2));
    REQUIRE(g.prepare(sr, frames));

    std::vector<float> li = l, ri = r;
    std::vector<float> lo(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> ro(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 2> in_ch{li.data(), ri.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2,
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);

    std::vector<float> result;
    result.insert(result.end(), lo.begin(), lo.end());
    result.insert(result.end(), ro.begin(), ro.end());
    return result;
}

// Build the AudioInput -> g1 -> g2 -> AudioOutput plan + bindings.
struct ChainGraph {
    GraphRuntimeSnapshot snapshot;
    GainState s1;
    GainState s2;
};

bool build_chain(ChainGraph& cg, float g1, float g2) {
    cg.s1.gain = g1;
    cg.s2.gain = g2;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 2},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 2, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 1, 2, 1},
        GraphRuntimeConnectionSpec{2, 0, 3, 0}, GraphRuntimeConnectionSpec{2, 1, 3, 1},
        GraphRuntimeConnectionSpec{3, 0, 4, 0}, GraphRuntimeConnectionSpec{3, 1, 4, 1},
    };
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, conns);
    if (!plan.ok()) return false;
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},          // AudioInput
        GraphRuntimeNodeBinding{2, routing_gain, &cg.s1, true},       // g1
        GraphRuntimeNodeBinding{3, routing_gain, &cg.s2, true},       // g2
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},          // AudioOutput
    };
    return cg.snapshot.reset(std::move(plan.plan), bindings);
}

std::vector<float> run_executor_routed(GraphRuntimeExecutor& exec, ChainGraph& cg,
                                       GraphRuntimeBufferPool& pool, double sr, int frames,
                                       const std::vector<float>& l,
                                       const std::vector<float>& r) {
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> ro(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 2> in_ch{li.data(), ri.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2,
                                            static_cast<std::uint32_t>(frames));

    BusBufferSet buses;
    REQUIRE(buses.add_input("main", in_view, BusRole::Main));
    REQUIRE(buses.add_output("main", out_view, BusRole::Main));

    ProcessBlock block;
    block.sample_rate = sr;
    block.frame_count = static_cast<std::uint32_t>(frames);
    block.buses = &buses;
    REQUIRE(block.validate());

    const auto result = exec.process_routed(block, cg.snapshot, pool);
    REQUIRE(result.ok());
    REQUIRE(result.nodes_processed == 4);

    std::vector<float> out;
    out.insert(out.end(), lo.begin(), lo.end());
    out.insert(out.end(), ro.begin(), ro.end());
    return out;
}

} // namespace

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a two-gain chain",
          "[host][graph][executor][routing][phase4][parity]") {
    constexpr double kSr = 48000.0;
    GraphRuntimeExecutor exec;
    for (int frames : {1, 64, 256, 1024}) {
        for (auto gains : {std::array<float, 2>{1.0f, 1.0f},
                           std::array<float, 2>{0.5f, 0.5f},
                           std::array<float, 2>{2.0f, 0.25f},
                           std::array<float, 2>{0.0f, 1.0f},
                           std::array<float, 2>{0.75f, 0.3f}}) {
            CAPTURE(frames);
            CAPTURE(gains[0]);
            CAPTURE(gains[1]);
            const auto l = test_signal(frames, 0.8f);
            const auto r = test_signal(frames, 0.6f);

            ChainGraph cg;
            REQUIRE(build_chain(cg, gains[0], gains[1]));
            GraphRuntimeBufferPool pool;
            REQUIRE(pool.reset(cg.snapshot.buffer_slot_count(),
                               static_cast<std::uint32_t>(frames)));

            const auto sg = run_signal_graph_chain(gains[0], gains[1], kSr, frames, l, r);
            const auto ex = run_executor_routed(exec, cg, pool, kSr, frames, l, r);

            REQUIRE(sg.size() == ex.size());
            for (std::size_t i = 0; i < sg.size(); ++i) {
                REQUIRE(sg[i] == ex[i]);  // bit-exact: same routed multiplies
            }
        }
    }
}

TEST_CASE("GraphRuntimeExecutor routing rejects feedback graphs until 4d",
          "[host][graph][executor][routing][phase4]") {
    // in -> A -> out, plus a feedback edge out-of-band (A -> A feedback). The
    // serial routing path can't honor feedback yet, so it must fail loudly
    // rather than silently treat the feedback edge as silence.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        // feedback edge: node 2's output feeds its own input next block.
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true, /*event=*/false},
    };
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, conns);
    REQUIRE(plan.ok());
    REQUIRE(plan.plan.connections.size() == 3);

    GainState s;
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &s, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(snapshot.reset(std::move(plan.plan), bindings));
    CHECK(snapshot.buffer_assignment().has_feedback);

    GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), 64));

    std::vector<float> in(64, 0.5f), out(64, 0.0f);
    std::array<const float*, 1> in_ch{in.data()};
    std::array<float*, 1> out_ch{out.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 1, 64);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 1, 64);
    BusBufferSet buses;
    REQUIRE(buses.add_input("main", in_view, BusRole::Main));
    REQUIRE(buses.add_output("main", out_view, BusRole::Main));
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = 64;
    block.buses = &buses;
    REQUIRE(block.validate());

    GraphRuntimeExecutor exec;
    const auto result = exec.process_routed(block, snapshot, pool);
    CHECK_FALSE(result.ok());
    CHECK(result.error ==
          pulp::format::GraphRuntimeExecutorErrorCode::UnsupportedFeedbackEdge);
}

TEST_CASE("GraphRuntimeExecutor routing process path does not allocate",
          "[host][graph][executor][routing][phase4][rt-safety]") {
    constexpr int kFrames = 256;
    ChainGraph cg;
    REQUIRE(build_chain(cg, 0.5f, 0.75f));
    GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(cg.snapshot.buffer_slot_count(), kFrames));

    const auto l = test_signal(kFrames, 0.8f);
    const auto r = test_signal(kFrames, 0.6f);
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(kFrames, 0.0f), ro(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{li.data(), ri.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);
    BusBufferSet buses;
    REQUIRE(buses.add_input("main", in_view, BusRole::Main));
    REQUIRE(buses.add_output("main", out_view, BusRole::Main));
    ProcessBlock block;
    block.sample_rate = 48000.0;
    block.frame_count = kFrames;
    block.buses = &buses;
    REQUIRE(block.validate());

    GraphRuntimeExecutor exec;
    // Warm-up block outside the probe (touches no allocator, but mirrors steady
    // state), then measure the hot path.
    REQUIRE(exec.process_routed(block, cg.snapshot, pool).ok());
    {
        pulp::test::RtAllocationProbe probe;
        const auto result = exec.process_routed(block, cg.snapshot, pool);
        REQUIRE(result.ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
}
