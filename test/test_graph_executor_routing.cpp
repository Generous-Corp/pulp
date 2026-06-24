// Phase 4b/4c: prove the canonical GraphRuntimeExecutor's ROUTING path moves
// audio between nodes correctly. Unlike the single-node output-parity baseline
// (test_graph_executor_parity.cpp), these build real topologies — a two-gain
// chain (g2's input must be g1's output) and a diamond (fan-out + fan-in) —
// where a routing bug (wrong slot, missed gather, wrong order) shows up as a
// bit-level mismatch against pulp::host::SignalGraph driving the same graph.
// Also asserts the routing path is allocation-free and rejects feedback graphs.
//
// A small RoutedHarness owns the per-call buffers/buses/block so each case is
// just "topology + bindings + assertion"; later phases (4d feedback variants,
// 4e reuse parity) add cases without re-hand-rolling the plumbing.

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
#include <span>
#include <vector>

namespace {

using pulp::format::BusBufferSet;
using pulp::format::BusRole;
using pulp::format::GraphRuntimeBufferPool;
using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeExecutorResult;
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
// per-node output view. Bus-agnostic — routing is the executor's job.
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

bool make_snapshot(GraphRuntimeSnapshot& snapshot,
                   std::span<const GraphRuntimeNodeSpec> nodes,
                   std::span<const GraphRuntimeConnectionSpec> conns,
                   std::span<const GraphRuntimeNodeBinding> bindings) {
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, conns);
    if (!plan.ok()) return false;
    return snapshot.reset(std::move(plan.plan), bindings);
}

// Owns the input/output channel storage + buses + block for one routed call.
// `inputs` is one buffer per main-input channel; the harness drives the routed
// executor and exposes per-channel output.
struct RoutedHarness {
    std::vector<std::vector<float>> ins;
    std::vector<std::vector<float>> outs;
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    BusBufferSet buses;
    ProcessBlock block;
    int frames = 0;

    RoutedHarness(double sr, int frames_,
                  const std::vector<std::vector<float>>& inputs,
                  std::size_t out_channels)
        : ins(inputs),
          outs(out_channels, std::vector<float>(static_cast<std::size_t>(frames_), 0.0f)),
          frames(frames_) {
        for (auto& c : ins) in_ptrs.push_back(c.data());
        for (auto& c : outs) out_ptrs.push_back(c.data());
        pulp::audio::BufferView<const float> in_view(
            in_ptrs.data(), in_ptrs.size(), static_cast<std::uint32_t>(frames));
        pulp::audio::BufferView<float> out_view(
            out_ptrs.data(), out_ptrs.size(), static_cast<std::uint32_t>(frames));
        REQUIRE(buses.add_input("main", in_view, BusRole::Main));
        REQUIRE(buses.add_output("main", out_view, BusRole::Main));
        block.sample_rate = sr;
        block.frame_count = static_cast<std::uint32_t>(frames);
        block.buses = &buses;
        REQUIRE(block.validate());
    }

    GraphRuntimeExecutorResult run(GraphRuntimeExecutor& exec,
                                   const GraphRuntimeSnapshot& snapshot,
                                   GraphRuntimeBufferPool& pool) {
        return exec.process_routed(block, snapshot, pool);
    }
};

GraphRuntimeBufferPool make_pool(const GraphRuntimeSnapshot& snapshot, int frames) {
    GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), static_cast<std::uint32_t>(frames)));
    return pool;
}

// Reference: input -> gain g1 -> gain g2 -> output (stereo). Per-channel output.
std::vector<std::vector<float>> signal_graph_chain(float g1, float g2, double sr, int frames,
                                                   const std::vector<std::vector<float>>& in) {
    pulp::host::SignalGraph g;
    const auto n_in = g.add_input_node(2, "In");
    const auto gn1 = g.add_gain_node("G1");
    const auto gn2 = g.add_gain_node("G2");
    const auto n_out = g.add_output_node(2, "Out");
    REQUIRE(g.connect(n_in, 0, gn1, 0));
    REQUIRE(g.connect(n_in, 1, gn1, 1));
    REQUIRE(g.connect(gn1, 0, gn2, 0));
    REQUIRE(g.connect(gn1, 1, gn2, 1));
    REQUIRE(g.connect(gn2, 0, n_out, 0));
    REQUIRE(g.connect(gn2, 1, n_out, 1));
    REQUIRE(g.set_node_gain(gn1, g1));
    REQUIRE(g.set_node_gain(gn2, g2));
    REQUIRE(g.prepare(sr, frames));

    std::vector<std::vector<float>> li = in;
    std::vector<std::vector<float>> lo(2, std::vector<float>(static_cast<std::size_t>(frames), 0.0f));
    std::array<const float*, 2> in_ch{li[0].data(), li[1].data()};
    std::array<float*, 2> out_ch{lo[0].data(), lo[1].data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2,
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return lo;
}

// Reference diamond: in -> {g1, g2} -> out (mono); out's input port sums g1+g2.
std::vector<float> signal_graph_diamond(float g1, float g2, double sr, int frames,
                                        const std::vector<float>& x) {
    pulp::host::SignalGraph g;
    const auto n_in = g.add_input_node(1, "In");
    const auto gn1 = g.add_gain_node("G1");
    const auto gn2 = g.add_gain_node("G2");
    const auto n_out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(n_in, 0, gn1, 0));
    REQUIRE(g.connect(n_in, 0, gn2, 0));
    REQUIRE(g.connect(gn1, 0, n_out, 0));
    REQUIRE(g.connect(gn2, 0, n_out, 0));
    REQUIRE(g.set_node_gain(gn1, g1));
    REQUIRE(g.set_node_gain(gn2, g2));
    REQUIRE(g.prepare(sr, frames));

    std::vector<float> xi = x;
    std::vector<float> yo(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 1> in_ch{xi.data()};
    std::array<float*, 1> out_ch{yo.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 1,
                                                 static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> out_view(out_ch.data(), 1,
                                            static_cast<std::uint32_t>(frames));
    g.process(out_view, in_view, frames);
    return yo;
}

// Reference feedback graph: in -> A(gain) -> out, plus A -> A feedback (mono).
// Per block, A's input = in + previous block's A output, so A_out[n] =
// g * (in[n] + A_out[n-1]). Returns this block's output; call across blocks
// with the same graph to drive the recurrence.
struct SignalGraphFeedback {
    pulp::host::SignalGraph g;
    pulp::host::NodeId gn = 0;
    int frames = 0;
    void build(float gain, double sr, int frames_) {
        frames = frames_;
        const auto n_in = g.add_input_node(1, "In");
        gn = g.add_gain_node("A");
        const auto n_out = g.add_output_node(1, "Out");
        REQUIRE(g.connect(n_in, 0, gn, 0));
        REQUIRE(g.connect(gn, 0, n_out, 0));
        REQUIRE(g.connect_feedback(gn, 0, gn, 0));
        REQUIRE(g.set_node_gain(gn, gain));
        REQUIRE(g.prepare(sr, frames));
    }
    std::vector<float> block(const std::vector<float>& x) {
        std::vector<float> xi = x;
        std::vector<float> yo(static_cast<std::size_t>(frames), 0.0f);
        std::array<const float*, 1> in_ch{xi.data()};
        std::array<float*, 1> out_ch{yo.data()};
        pulp::audio::BufferView<const float> in_view(in_ch.data(), 1,
                                                     static_cast<std::uint32_t>(frames));
        pulp::audio::BufferView<float> out_view(out_ch.data(), 1,
                                                static_cast<std::uint32_t>(frames));
        g.process(out_view, in_view, frames);
        return yo;
    }
};

constexpr double kSr = 48000.0;

} // namespace

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a two-gain chain",
          "[host][graph][executor][routing][phase4][parity]") {
    GraphRuntimeExecutor exec;
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
    for (int frames : {1, 64, 256, 1024}) {
        for (auto gains : {std::array<float, 2>{1.0f, 1.0f},
                           std::array<float, 2>{0.5f, 0.5f},
                           std::array<float, 2>{2.0f, 0.25f},
                           std::array<float, 2>{0.0f, 1.0f},
                           std::array<float, 2>{0.75f, 0.3f}}) {
            CAPTURE(frames, gains[0], gains[1]);
            GainState s1{gains[0]}, s2{gains[1]};
            const std::array bindings = {
                GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                GraphRuntimeNodeBinding{2, routing_gain, &s1, true},
                GraphRuntimeNodeBinding{3, routing_gain, &s2, true},
                GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
            };
            GraphRuntimeSnapshot snapshot;
            REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
            auto pool = make_pool(snapshot, frames);

            const std::vector<std::vector<float>> in{test_signal(frames, 0.8f),
                                                     test_signal(frames, 0.6f)};
            RoutedHarness h(kSr, frames, in, 2);
            REQUIRE(h.run(exec, snapshot, pool).ok());
            const auto ref = signal_graph_chain(gains[0], gains[1], kSr, frames, in);

            for (std::size_t c = 0; c < 2; ++c) {
                REQUIRE(ref[c].size() == h.outs[c].size());
                for (std::size_t i = 0; i < ref[c].size(); ++i) {
                    REQUIRE(ref[c][i] == h.outs[c][i]);  // bit-exact routed multiplies
                }
            }
        }
    }
}

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a diamond (fan-out + fan-in)",
          "[host][graph][executor][routing][phase4][parity]") {
    GraphRuntimeExecutor exec;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},  // fan-in: g1 and g2 -> out:0
    };
    for (int frames : {1, 64, 512}) {
        for (auto gains : {std::array<float, 2>{1.0f, 1.0f},
                           std::array<float, 2>{0.5f, 0.25f},
                           std::array<float, 2>{0.75f, 0.0f},
                           std::array<float, 2>{2.0f, 1.5f}}) {
            CAPTURE(frames, gains[0], gains[1]);
            GainState s1{gains[0]}, s2{gains[1]};
            const std::array bindings = {
                GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                GraphRuntimeNodeBinding{2, routing_gain, &s1, true},
                GraphRuntimeNodeBinding{3, routing_gain, &s2, true},
                GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
            };
            GraphRuntimeSnapshot snapshot;
            REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
            auto pool = make_pool(snapshot, frames);

            const std::vector<std::vector<float>> in{test_signal(frames, 0.8f)};
            RoutedHarness h(kSr, frames, in, 1);
            REQUIRE(h.run(exec, snapshot, pool).ok());
            const auto ref = signal_graph_diamond(gains[0], gains[1], kSr, frames, in[0]);

            REQUIRE(ref.size() == h.outs[0].size());
            for (std::size_t i = 0; i < ref.size(); ++i) {
                // 2-way fan-in is commutative in IEEE-754 -> bit-exact.
                REQUIRE(ref[i] == h.outs[0][i]);
            }
        }
    }
}

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a feedback loop",
          "[host][graph][executor][routing][phase4][parity][feedback]") {
    // in -> A(gain) -> out, with A -> A one-block feedback. The feedback is a
    // whole-block delay (not a per-sample IIR): block n's A input at sample i is
    // in[n][i] + A_out[n-1][i], so out[n][i] = g*(in[n][i] + A_out[n-1][i]).
    // This must match SignalGraph block-for-block, proving the previous-block
    // capture + gather are correct and stateful across blocks.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true, /*event=*/false},
    };
    constexpr int kFrames = 64;
    for (float gain : {0.5f, 0.3f, 0.0f}) {
        CAPTURE(gain);
        GainState s{gain};
        const std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            GraphRuntimeNodeBinding{2, routing_gain, &s, true},
            GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
        };
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
        CHECK(snapshot.buffer_assignment().has_feedback);
        auto pool = make_pool(snapshot, kFrames);  // persists across blocks
        GraphRuntimeExecutor exec;

        SignalGraphFeedback ref;
        ref.build(gain, kSr, kFrames);

        // Drive the recurrence over several blocks with distinct inputs.
        for (int blk = 0; blk < 6; ++blk) {
            CAPTURE(blk);
            const std::vector<std::vector<float>> in{
                test_signal(kFrames, 0.4f + 0.1f * static_cast<float>(blk))};
            RoutedHarness h(kSr, kFrames, in, 1);
            REQUIRE(h.run(exec, snapshot, pool).ok());
            const auto r = ref.block(in[0]);
            REQUIRE(r.size() == h.outs[0].size());
            for (std::size_t i = 0; i < r.size(); ++i) {
                REQUIRE(r[i] == h.outs[0][i]);  // bit-exact across the recurrence
            }
        }
    }
}

TEST_CASE("GraphRuntimeExecutor routing process path does not allocate",
          "[host][graph][executor][routing][phase4][rt-safety]") {
    constexpr int kFrames = 256;
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
    GainState s1{0.5f}, s2{0.75f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &s1, true},
        GraphRuntimeNodeBinding{3, routing_gain, &s2, true},
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    auto pool = make_pool(snapshot, kFrames);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.8f),
                                             test_signal(kFrames, 0.6f)};
    RoutedHarness h(kSr, kFrames, in, 2);
    GraphRuntimeExecutor exec;
    REQUIRE(h.run(exec, snapshot, pool).ok());  // warm-up outside the probe
    {
        pulp::test::RtAllocationProbe probe;
        const auto result = h.run(exec, snapshot, pool);
        REQUIRE(result.ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
}

TEST_CASE("GraphRuntimeExecutor feedback routing path does not allocate",
          "[host][graph][executor][routing][phase4][rt-safety][feedback]") {
    // Feedback adds the gather-from-prev-slot read and the end-of-block capture
    // copy; both must stay allocation-free on the RT thread.
    constexpr int kFrames = 128;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true, /*event=*/false},
    };
    GainState s{0.5f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &s, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    auto pool = make_pool(snapshot, kFrames);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.7f)};
    GraphRuntimeExecutor exec;
    {
        RoutedHarness warm(kSr, kFrames, in, 1);
        REQUIRE(warm.run(exec, snapshot, pool).ok());  // warm-up outside the probe
    }
    {
        RoutedHarness h(kSr, kFrames, in, 1);
        pulp::test::RtAllocationProbe probe;
        const auto result = h.run(exec, snapshot, pool);
        REQUIRE(result.ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
}
