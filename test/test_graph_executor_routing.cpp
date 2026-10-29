// Prove the canonical GraphRuntimeExecutor's ROUTING path moves
// audio between nodes correctly. Unlike the single-node output-parity baseline
// (test_graph_executor_parity.cpp), these build real topologies — a two-gain
// chain (g2's input must be g1's output) and a diamond (fan-out + fan-in) —
// where a routing bug (wrong slot, missed gather, wrong order) shows up as a
// bit-level mismatch against pulp::host::SignalGraph driving the same graph.
// Covers feedforward, fan-out/fan-in, multi-output mixing, and one-block
// feedback, and asserts the routing path is allocation-free.
//
// A small RoutedHarness owns the per-call buffers/buses/block so each case is
// just "topology + bindings + assertion"; new topologies add cases without
// re-hand-rolling the plumbing.

#include "harness/graph_routing_harness.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using pulp::format::GraphRuntimeBufferPool;
using pulp::format::GraphRuntimeCommandDecision;
using pulp::format::GraphRuntimeCommandStatus;
using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeSnapshot;
using pulp::graph::GraphCommandType;
using pulp::graph::GraphRuntimeConnectionSpec;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphTimedCommand;

// Shared routing plumbing: per-node gain state + binding, deterministic input
// signal, snapshot/pool construction, and the per-call buffer/bus/block harness
// all live in the harness header so the fixed-shape and differential-parity
// suites build the routed executor the same way.
using pulp::test::graph_routing::GainState;
using pulp::test::graph_routing::make_pool;
using pulp::test::graph_routing::make_snapshot;
using pulp::test::graph_routing::RoutedHarness;
using pulp::test::graph_routing::routing_gain;
using pulp::test::graph_routing::test_signal;

// A two-input reduction binding makes an unconnected-port gather observable.
// The direct-copy path must zero every unused input port before this callback
// runs, just as the general clear-then-gather path does.
bool routing_sum_inputs(pulp::format::ProcessBlock&,
                        const pulp::format::GraphRuntimeNodeProcessContext& ctx, void*) noexcept {
    const auto& in = ctx.node_inputs;
    auto out = ctx.node_outputs;
    if (out.num_channels() == 0)
        return true;
    float* destination = out.channel_ptr(0);
    for (std::size_t frame = 0; frame < out.num_samples(); ++frame) {
        float value = 0.0f;
        for (std::size_t channel = 0; channel < in.num_channels(); ++channel)
            value += in.channel_ptr(channel)[frame];
        destination[frame] = value;
    }
    return true;
}

// Writes both output channels so the first node in the unused-port test leaves
// a nonzero same-size region that the allocator can recycle into the target's
// input region. This catches a fast path that forgets to clear unconnected ports.
bool routing_duplicate_outputs(pulp::format::ProcessBlock&,
                               const pulp::format::GraphRuntimeNodeProcessContext& ctx,
                               void*) noexcept {
    const auto& in = ctx.node_inputs;
    auto out = ctx.node_outputs;
    if (in.num_channels() == 0)
        return true;
    const float* source = in.channel_ptr(0);
    for (std::size_t frame = 0; frame < out.num_samples(); ++frame) {
        if (out.num_channels() > 0)
            out.channel_ptr(0)[frame] = source[frame] * 2.0f;
        if (out.num_channels() > 1)
            out.channel_ptr(1)[frame] = source[frame] * 3.0f;
    }
    return true;
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

// Reference split-lifetime graph: in -> A; A -> B -> out and A -> out. A's
// output feeds an EARLY consumer (B) and a LATE consumer (out's fan-in), so A's
// scratch must stay live across the intervening node while other regions are
// recycled. out:0 = A + B = a*in + b*a*in. Mono.
std::vector<float> signal_graph_split(float a, float b, double sr, int frames,
                                      const std::vector<float>& x) {
    pulp::host::SignalGraph g;
    const auto n_in = g.add_input_node(1, "In");
    const auto na = g.add_gain_node("A");
    const auto nb = g.add_gain_node("B");
    const auto n_out = g.add_output_node(1, "Out");
    REQUIRE(g.connect(n_in, 0, na, 0));
    REQUIRE(g.connect(na, 0, nb, 0));
    REQUIRE(g.connect(nb, 0, n_out, 0));
    REQUIRE(g.connect(na, 0, n_out, 0));  // A also feeds out (late consumer)
    REQUIRE(g.set_node_gain(na, a));
    REQUIRE(g.set_node_gain(nb, b));
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

constexpr double kSr = 48000.0;

} // namespace

TEST_CASE("GraphRuntimeExecutor command drain uses snapshot node id index",
          "[graph][executor][commands]") {
    constexpr int kFrames = 16;
    const std::array<GraphRuntimeNodeSpec, 3> nodes{{
        {42, GraphRuntimeNodeKind::AudioInput, 0, 1},
        {7, GraphRuntimeNodeKind::Processor, 1, 1},
        {99, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    }};
    const std::array<GraphRuntimeConnectionSpec, 2> conns{{
        {42, 0, 7, 0},
        {7, 0, 99, 0},
    }};
    GainState gain{1.0f};
    const std::array<GraphRuntimeNodeBinding, 3> bindings{{
        {42, nullptr, nullptr, false},
        {7, routing_gain, &gain, true},
        {99, nullptr, nullptr, false},
    }};

    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    REQUIRE(snapshot.contains_node(42));
    REQUIRE(snapshot.contains_node(7));
    REQUIRE(snapshot.contains_node(99));
    REQUIRE_FALSE(snapshot.contains_node(8));

    auto pool = make_pool(snapshot, kFrames);
    const std::vector<std::vector<float>> input{{test_signal(kFrames, 0.2f)}};
    RoutedHarness h(48000.0, kFrames, input, 1);

    std::array<GraphTimedCommand, 3> commands{};
    commands[0].command.sequence_id = 1;
    commands[0].command.type = GraphCommandType::SetNodeGain;
    commands[0].command.node_id = 7;
    commands[1].command.sequence_id = 2;
    commands[1].command.type = GraphCommandType::SetNodeGain;
    commands[1].command.node_id = 8;
    commands[2].command.sequence_id = 3;
    commands[2].command.type = GraphCommandType::TransportJump;
    commands[2].command.node_id = 123456;

    std::array<GraphRuntimeCommandDecision, 3> decisions{};
    GraphRuntimeExecutor exec;
    const auto result = exec.process_routed(h.block, snapshot, pool, nullptr, nullptr,
                                            commands, decisions);
    REQUIRE(result.ok());
    REQUIRE(result.commands_drained == commands.size());
    REQUIRE(result.commands_accepted == 2);
    REQUIRE(result.commands_rejected == 1);
    REQUIRE(decisions[0].status == GraphRuntimeCommandStatus::Accepted);
    REQUIRE(decisions[1].status == GraphRuntimeCommandStatus::Rejected);
    REQUIRE(decisions[2].status == GraphRuntimeCommandStatus::Accepted);
}

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a two-gain chain",
          "[host][graph][executor][routing][parity]") {
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
          "[host][graph][executor][routing][parity]") {
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

TEST_CASE("GraphRuntimeExecutor routing sums multiple AudioOutput nodes into the bus",
          "[host][graph][executor][routing][parity]") {
    // in -> A -> out1 and in -> B -> out2; out1 and out2 are SEPARATE
    // AudioOutput nodes both driving main-output channel 0. SignalGraph zeroes
    // the output bus once and each output node accumulates, so bus = A + B.
    // This is the case the AudioOutput accumulate (+=) + zero-bus-once change
    // exists for; overwrite semantics would be last-writer-wins (= B only).
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},   // A
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},   // B
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},  // in -> A
        GraphRuntimeConnectionSpec{1, 0, 3, 0},  // in -> B
        GraphRuntimeConnectionSpec{2, 0, 4, 0},  // A -> out1
        GraphRuntimeConnectionSpec{3, 0, 5, 0},  // B -> out2
    };
    constexpr int kFrames = 64;
    const float a = 0.5f, b = 0.25f;
    GainState sa{a}, sb{b};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &sa, true},
        GraphRuntimeNodeBinding{3, routing_gain, &sb, true},
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{5, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    auto pool = make_pool(snapshot, kFrames);
    const auto x = test_signal(kFrames, 0.8f);
    const std::vector<std::vector<float>> in{x};
    RoutedHarness h(kSr, kFrames, in, 1);
    GraphRuntimeExecutor exec;
    REQUIRE(h.run(exec, snapshot, pool).ok());
    for (int i = 0; i < kFrames; ++i) {
        const float expected = x[static_cast<std::size_t>(i)] * a +
                               x[static_cast<std::size_t>(i)] * b;
        REQUIRE(h.outs[0][static_cast<std::size_t>(i)] == expected);
    }
}

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a split-lifetime fan-out",
          "[host][graph][executor][routing][parity]") {
    // A's output feeds B (early) and out (late); slot reuse must keep A's
    // scratch live across B. out:0 = A + B.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},  // A
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},  // B
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},  // in -> A
        GraphRuntimeConnectionSpec{2, 0, 3, 0},  // A -> B (early)
        GraphRuntimeConnectionSpec{3, 0, 4, 0},  // B -> out
        GraphRuntimeConnectionSpec{2, 0, 4, 0},  // A -> out (late; fan-in with B)
    };
    GraphRuntimeExecutor exec;
    for (int frames : {1, 64, 256}) {
        for (auto gains : {std::array<float, 2>{0.5f, 0.5f},
                           std::array<float, 2>{0.8f, 0.3f},
                           std::array<float, 2>{1.0f, 0.0f}}) {
            CAPTURE(frames, gains[0], gains[1]);
            GainState sa{gains[0]}, sb{gains[1]};
            const std::array bindings = {
                GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
                GraphRuntimeNodeBinding{2, routing_gain, &sa, true},
                GraphRuntimeNodeBinding{3, routing_gain, &sb, true},
                GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
            };
            GraphRuntimeSnapshot snapshot;
            REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
            auto pool = make_pool(snapshot, frames);
            const std::vector<std::vector<float>> in{test_signal(frames, 0.7f)};
            RoutedHarness h(kSr, frames, in, 1);
            REQUIRE(h.run(exec, snapshot, pool).ok());
            const auto ref = signal_graph_split(gains[0], gains[1], kSr, frames, in[0]);
            REQUIRE(ref.size() == h.outs[0].size());
            for (std::size_t i = 0; i < ref.size(); ++i) {
                REQUIRE(ref[i] == h.outs[0][i]);  // bit-exact under reuse
            }
        }
    }
}

TEST_CASE("GraphRuntimeExecutor direct copy clears unused multi-port inputs",
          "[graph][executor][routing][parity][fastpath]") {
    // The dead node's two-slot output is recycled as the target's two-slot
    // input region. The target receives only port 1; port 0 therefore contains
    // stale nonzero data unless the one-audio-edge fast path clears it.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 2},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 1},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
    };
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_duplicate_outputs, nullptr, true},
        GraphRuntimeNodeBinding{3, routing_sum_inputs, nullptr, true},
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
    };
    constexpr int kFrames = 64;
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));

    const auto& plan = snapshot.plan();
    const auto& assignment = snapshot.buffer_assignment();
    REQUIRE(assignment.nodes.size() == plan.nodes.size());
    // Verify the setup really creates the stale-region alias that makes the
    // clear observable; this is an allocator contract, not an output oracle.
    REQUIRE(assignment.nodes[1].output_base == assignment.nodes[2].input_base);
    REQUIRE(plan.nodes[2].audio_inbound_connection_count == 1);
    REQUIRE(plan.nodes[2].single_audio_inbound_connection != 0xFFFFFFFFu);

    auto pool = make_pool(snapshot, kFrames);
    const auto input = test_signal(kFrames, 0.8f);
    RoutedHarness h(kSr, kFrames, {input}, 1);
    GraphRuntimeExecutor exec;
    REQUIRE(h.run(exec, snapshot, pool).ok());
    for (int frame = 0; frame < kFrames; ++frame)
        REQUIRE(h.outs[0][static_cast<std::size_t>(frame)] ==
                input[static_cast<std::size_t>(frame)]);
}

TEST_CASE("GraphRuntimeExecutor direct copy preserves a feedback-only fast path",
          "[graph][executor][routing][parity][feedback][fastpath]") {
    // With no feedforward input, the feedback edge is the node's sole audio
    // inbound connection, so this exercises the one-audio-edge branch rather
    // than the general reduction. Seed its previous-block slot explicitly and
    // then check the recurrence across several blocks.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true},
    };
    constexpr int kFrames = 32;
    constexpr float kGain = 0.5f;
    GainState gain{kGain};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &gain, true},
        GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    REQUIRE(snapshot.plan().nodes[1].audio_inbound_connection_count == 1);
    REQUIRE(snapshot.plan().nodes[1].single_audio_inbound_connection == 1);
    auto pool = make_pool(snapshot, kFrames);
    const auto feedback_index = snapshot.plan().nodes[1].single_audio_inbound_connection;
    const auto feedback_slot = snapshot.buffer_assignment().feedback_prev_slot[feedback_index];
    REQUIRE(feedback_slot != pulp::graph::kGraphRuntimeNoSlot);
    float* previous = pool.slot_data(feedback_slot);
    REQUIRE(previous != nullptr);
    const auto seed = test_signal(kFrames, 0.6f);
    std::copy(seed.begin(), seed.end(), previous);

    std::vector<float> expected = seed;
    GraphRuntimeExecutor exec;
    for (int block = 0; block < 5; ++block) {
        CAPTURE(block);
        RoutedHarness h(kSr, kFrames, {std::vector<float>(kFrames, 0.0f)}, 1);
        REQUIRE(h.run(exec, snapshot, pool).ok());
        for (int frame = 0; frame < kFrames; ++frame) {
            expected[static_cast<std::size_t>(frame)] *= kGain;
            REQUIRE(h.outs[0][static_cast<std::size_t>(frame)] ==
                    expected[static_cast<std::size_t>(frame)]);
        }
    }
}

TEST_CASE("GraphRuntimeExecutor delayed edge falls back from direct copy across split blocks",
          "[graph][executor][routing][parity][pdc][fastpath]") {
    // The two-port mixer has one edge that must be delayed by PDC. The
    // one-edge-per-port shape is otherwise eligible for direct copies, so the
    // delayed-ring check must force the general gather path. Vary block sizes
    // while retaining one pool to exercise the ring state across boundaries.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 2},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1, 0, 0, false, 4},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::Processor, 2, 1},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 1, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0}, GraphRuntimeConnectionSpec{3, 0, 4, 1},
        GraphRuntimeConnectionSpec{4, 0, 5, 0},
    };
    GainState latent_gain{1.0f};
    GainState plain_gain{1.0f};
    GainState mix_gain{1.0f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &latent_gain, true},
        GraphRuntimeNodeBinding{3, routing_gain, &plain_gain, true},
        GraphRuntimeNodeBinding{4, routing_sum_inputs, &mix_gain, true},
        GraphRuntimeNodeBinding{5, nullptr, nullptr, false},
    };
    constexpr int kMaxFrames = 64;
    constexpr std::uint32_t kDelay = 4;
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    const auto& plan = snapshot.plan();
    const auto& assignment = snapshot.buffer_assignment();
    REQUIRE(assignment.connection_delay_samples.size() == conns.size());
    std::size_t delayed_index = conns.size();
    for (std::size_t i = 0; i < plan.connections.size(); ++i) {
        const auto& connection = plan.connections[i];
        if (connection.source_index == 2 && connection.dest_index == 3 &&
            connection.dest_port == 1) {
            delayed_index = i;
            break;
        }
    }
    REQUIRE(delayed_index < conns.size());
    REQUIRE(assignment.connection_delay_samples[delayed_index] == kDelay);
    REQUIRE(plan.nodes[3].audio_inbound_connection_count == 2);
    REQUIRE(plan.nodes[3].audio_inbound_port_mask == 0x3u);

    GraphRuntimeBufferPool pool;
    REQUIRE(
        pool.reset(snapshot.buffer_slot_count(), kMaxFrames, assignment.connection_delay_samples));
    std::vector<float> delay_line(kDelay, 0.0f);
    GraphRuntimeExecutor exec;
    for (const int frames : {7, 32, 5, 64, 11}) {
        CAPTURE(frames);
        const auto left = test_signal(frames, 0.7f + static_cast<float>(frames) * 0.001f);
        const auto right = test_signal(frames, 0.3f + static_cast<float>(frames) * 0.002f);
        RoutedHarness h(kSr, frames, {left, right}, 1);
        REQUIRE(h.run(exec, snapshot, pool).ok());
        for (int frame = 0; frame < frames; ++frame) {
            const float delayed = delay_line.front();
            delay_line.erase(delay_line.begin());
            delay_line.push_back(right[static_cast<std::size_t>(frame)]);
            const float expected = left[static_cast<std::size_t>(frame)] + delayed;
            REQUIRE(h.outs[0][static_cast<std::size_t>(frame)] == expected);
        }
    }
}

TEST_CASE("GraphRuntimeExecutor routing matches SignalGraph for a feedback loop",
          "[host][graph][executor][routing][parity][feedback]") {
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
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true},
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
          "[host][graph][executor][routing][rt-safety]") {
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
          "[host][graph][executor][routing][rt-safety][feedback]") {
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
        GraphRuntimeConnectionSpec{2, 0, 2, 0, /*feedback=*/true},
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

// ── Levelized parallel execution (process_parallel) ─────────────────────────
// process_parallel must produce output bit-identical to the serial process_routed
// path (each node owns disjoint scratch; the level barrier orders producers
// before consumers). A wide fan-out graph puts many independent gains on one
// level so the parallel dispatch actually runs.

namespace {

// in(1ch) -> N parallel gains -> out(1ch) (fan-in sum). The N gains share level 1.
void build_wide_fanout(std::vector<GraphRuntimeNodeSpec>& nodes,
                       std::vector<GraphRuntimeConnectionSpec>& conns,
                       std::vector<GainState>& gains,
                       std::vector<GraphRuntimeNodeBinding>& bindings, int n) {
    nodes.push_back({1, GraphRuntimeNodeKind::AudioInput, 0, 1});
    const pulp::graph::NodeId out_id = static_cast<pulp::graph::NodeId>(n + 2);
    gains.assign(static_cast<std::size_t>(n), GainState{});
    for (int i = 0; i < n; ++i) {
        const auto id = static_cast<pulp::graph::NodeId>(i + 2);
        nodes.push_back({id, GraphRuntimeNodeKind::Processor, 1, 1});
        gains[static_cast<std::size_t>(i)].gain = 0.1f + 0.01f * static_cast<float>(i);
        conns.push_back({1, 0, id, 0});       // in -> gain
        conns.push_back({id, 0, out_id, 0});  // gain -> out (fan-in)
    }
    nodes.push_back({out_id, GraphRuntimeNodeKind::AudioOutput, 1, 0});
    bindings.push_back({1, nullptr, nullptr, false});
    for (int i = 0; i < n; ++i) {
        bindings.push_back({static_cast<pulp::graph::NodeId>(i + 2), routing_gain,
                            &gains[static_cast<std::size_t>(i)], true});
    }
    bindings.push_back({out_id, nullptr, nullptr, false});
}

} // namespace

TEST_CASE("Parallel routing matches the serial routed path: wide fan-out",
          "[format][graph][executor][routing][parallel]") {
    constexpr int kFrames = 64;
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> conns;
    std::vector<GainState> gains;
    std::vector<GraphRuntimeNodeBinding> bindings;
    build_wide_fanout(nodes, conns, gains, bindings, /*n=*/16);

    // Serial uses the compact (reuse) layout; parallel uses the reuse-free
    // layout. Comparing their outputs also proves both layouts compute identically.
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    GraphRuntimeSnapshot psnapshot;
    REQUIRE(make_snapshot(psnapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(psnapshot.plan());
    REQUIRE(levels.ok);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.8f)};
    GraphRuntimeExecutor exec;

    auto pool_serial = make_pool(snapshot, kFrames);
    RoutedHarness serial(kSr, kFrames, in, 1);
    REQUIRE(serial.run(exec, snapshot, pool_serial).ok());

    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));
    auto pool_parallel = make_pool(psnapshot, kFrames);
    RoutedHarness par(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(par.block, psnapshot, levels, pool_parallel, workers).ok());
    workers.stop();

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(serial.outs[0][static_cast<std::size_t>(i)] ==
                par.outs[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Parallel routing matches the serial routed path: multi-output mixing",
          "[format][graph][executor][routing][parallel]") {
    constexpr int kFrames = 64;
    // Two AudioOutput nodes accumulate into the same bus — they must run serially
    // in the parallel path (their level is forced serial). Output must still match.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0}, GraphRuntimeConnectionSpec{3, 0, 5, 0},
    };
    GainState g2{0.5f}, g3{0.25f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &g2, true},
        GraphRuntimeNodeBinding{3, routing_gain, &g3, true},
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{5, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    GraphRuntimeSnapshot psnapshot;
    REQUIRE(make_snapshot(psnapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(psnapshot.plan());
    REQUIRE(levels.ok);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.6f)};
    GraphRuntimeExecutor exec;
    auto pool_serial = make_pool(snapshot, kFrames);
    RoutedHarness serial(kSr, kFrames, in, 1);
    REQUIRE(serial.run(exec, snapshot, pool_serial).ok());

    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));
    auto pool_parallel = make_pool(psnapshot, kFrames);
    RoutedHarness par(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(par.block, psnapshot, levels, pool_parallel, workers).ok());
    workers.stop();

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(serial.outs[0][static_cast<std::size_t>(i)] ==
                par.outs[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Parallel routing is allocation-free on the audio thread",
          "[format][graph][executor][routing][parallel][rt-safety]") {
    constexpr int kFrames = 64;
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> conns;
    std::vector<GainState> gains;
    std::vector<GraphRuntimeNodeBinding> bindings;
    build_wide_fanout(nodes, conns, gains, bindings, /*n=*/16);
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(snapshot.plan());
    auto pool = make_pool(snapshot, kFrames);
    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));
    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.8f)};
    GraphRuntimeExecutor exec;
    {
        RoutedHarness warm(kSr, kFrames, in, 1);
        REQUIRE(exec.process_parallel(warm.block, snapshot, levels, pool, workers).ok());
    }
    {
        RoutedHarness h(kSr, kFrames, in, 1);
        pulp::test::RtAllocationProbe probe;
        REQUIRE(exec.process_parallel(h.block, snapshot, levels, pool, workers).ok());
        REQUIRE_FALSE(probe.saw_allocation());
    }
    workers.stop();
}

TEST_CASE("Parallel routing matches serial: 3 sinks accumulate in topo order",
          "[format][graph][executor][routing][parallel]") {
    // Three AudioOutput nodes sum into the same output channel at one level. The
    // serial-fallback for that level must accumulate in the SAME order the serial
    // walk does (processing order), or non-associative float += diverges. The
    // gains feeding the sinks are wired highest-id-first so processing order is
    // NOT ascending node-index — exercising the ordering guarantee.
    constexpr int kFrames = 64;
    // ids: in=1; gains 2,3,4; sinks 5,6,7. Wire in->gain edges in REVERSE so the
    // sink fed by the highest-id gain is processed first.
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        GraphRuntimeNodeSpec{6, GraphRuntimeNodeKind::AudioOutput, 1, 0},
        GraphRuntimeNodeSpec{7, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 4, 0},  // in -> g4 (readied first)
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{4, 0, 7, 0},  // g4 -> sink7
        GraphRuntimeConnectionSpec{3, 0, 6, 0},
        GraphRuntimeConnectionSpec{2, 0, 5, 0},
    };
    // Order-sensitive gains: a large term and two small ones so (a+b)+c float-
    // differs from a different order.
    GainState g2{1.0f}, g3{1.0e7f}, g4{1.0f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &g2, true},
        GraphRuntimeNodeBinding{3, routing_gain, &g3, true},
        GraphRuntimeNodeBinding{4, routing_gain, &g4, true},
        GraphRuntimeNodeBinding{5, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{6, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{7, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    GraphRuntimeSnapshot psnapshot;
    REQUIRE(make_snapshot(psnapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(psnapshot.plan());
    REQUIRE(levels.ok);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.9f)};
    GraphRuntimeExecutor exec;
    auto pool_serial = make_pool(snapshot, kFrames);
    RoutedHarness serial(kSr, kFrames, in, 1);
    REQUIRE(serial.run(exec, snapshot, pool_serial).ok());

    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));
    auto pool_parallel = make_pool(psnapshot, kFrames);
    RoutedHarness par(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(par.block, psnapshot, levels, pool_parallel, workers).ok());
    workers.stop();

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(serial.outs[0][static_cast<std::size_t>(i)] ==
                par.outs[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Parallel routing matches serial: deep and wide (two parallel levels)",
          "[format][graph][executor][routing][parallel]") {
    // in -> A[0..3] -> B[0..3] -> out. A is level 1 (parallel), B is level 2
    // (parallel), out level 3. Exercises two consecutive parallel levels and the
    // cross-level barrier (level-1 writes visible to level-2 workers).
    constexpr int kFrames = 64;
    constexpr int kW = 4;
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> conns;
    nodes.push_back({1, GraphRuntimeNodeKind::AudioInput, 0, 1});
    const auto out_id = static_cast<pulp::graph::NodeId>(2 + 2 * kW);
    for (int i = 0; i < kW; ++i) {
        const auto a = static_cast<pulp::graph::NodeId>(2 + i);
        const auto b = static_cast<pulp::graph::NodeId>(2 + kW + i);
        nodes.push_back({a, GraphRuntimeNodeKind::Processor, 1, 1});
        nodes.push_back({b, GraphRuntimeNodeKind::Processor, 1, 1});
        conns.push_back({1, 0, a, 0});       // in -> A[i]
        conns.push_back({a, 0, b, 0});       // A[i] -> B[i]
        conns.push_back({b, 0, out_id, 0});  // B[i] -> out (fan-in)
    }
    nodes.push_back({out_id, GraphRuntimeNodeKind::AudioOutput, 1, 0});

    std::vector<GainState> gains(static_cast<std::size_t>(2 * kW));
    std::vector<GraphRuntimeNodeBinding> bindings;
    bindings.push_back({1, nullptr, nullptr, false});
    for (int i = 0; i < kW; ++i) {
        gains[static_cast<std::size_t>(2 * i)].gain = 0.3f + 0.01f * static_cast<float>(i);
        gains[static_cast<std::size_t>(2 * i + 1)].gain = 0.5f - 0.02f * static_cast<float>(i);
        bindings.push_back({static_cast<pulp::graph::NodeId>(2 + i), routing_gain,
                            &gains[static_cast<std::size_t>(2 * i)], true});
        bindings.push_back({static_cast<pulp::graph::NodeId>(2 + kW + i), routing_gain,
                            &gains[static_cast<std::size_t>(2 * i + 1)], true});
    }
    bindings.push_back({out_id, nullptr, nullptr, false});

    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    GraphRuntimeSnapshot psnapshot;
    REQUIRE(make_snapshot(psnapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(psnapshot.plan());
    REQUIRE(levels.ok);
    REQUIRE(levels.level_count == 4);  // in / A / B / out

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.7f)};
    GraphRuntimeExecutor exec;
    auto pool_serial = make_pool(snapshot, kFrames);
    RoutedHarness serial(kSr, kFrames, in, 1);
    REQUIRE(serial.run(exec, snapshot, pool_serial).ok());

    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));
    auto pool_parallel = make_pool(psnapshot, kFrames);
    RoutedHarness par(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(par.block, psnapshot, levels, pool_parallel, workers).ok());
    workers.stop();

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(serial.outs[0][static_cast<std::size_t>(i)] ==
                par.outs[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("Parallel routing cost gate: a sub-threshold level runs serially",
          "[format][graph][executor][routing][parallel][cost]") {
    // in(1) -> 4 mono gains -> out(1). The 4-gain level is the only parallel-
    // eligible level (width>1, no AudioOutput); its static weight is 4*max(1,1)=4,
    // so at 64 frames it carries 256 channel-samples of work. The cost gate routes
    // it serially when the threshold exceeds that, and dispatches it when the
    // threshold is 0 — without ever changing the output.
    constexpr int kFrames = 64;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{6, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0}, GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{1, 0, 4, 0}, GraphRuntimeConnectionSpec{1, 0, 5, 0},
        GraphRuntimeConnectionSpec{2, 0, 6, 0}, GraphRuntimeConnectionSpec{3, 0, 6, 0},
        GraphRuntimeConnectionSpec{4, 0, 6, 0}, GraphRuntimeConnectionSpec{5, 0, 6, 0},
    };
    GainState g2{0.1f}, g3{0.2f}, g4{0.3f}, g5{0.4f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &g2, true},
        GraphRuntimeNodeBinding{3, routing_gain, &g3, true},
        GraphRuntimeNodeBinding{4, routing_gain, &g4, true},
        GraphRuntimeNodeBinding{5, routing_gain, &g5, true},
        GraphRuntimeNodeBinding{6, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot psnapshot;
    REQUIRE(make_snapshot(psnapshot, nodes, conns, bindings, /*parallel_safe=*/true));
    const auto levels = pulp::graph::build_graph_runtime_levelization(psnapshot.plan());
    REQUIRE(levels.ok);

    const std::vector<std::vector<float>> in{test_signal(kFrames, 0.8f)};
    GraphRuntimeExecutor exec;
    pulp::format::GraphRuntimeWorkerPool workers;
    REQUIRE(workers.start(4));

    // Threshold above the level's 256 channel-samples: it stays serial.
    exec.set_parallel_min_work_units(1000);
    exec.reset_stats();
    auto pool_hi = make_pool(psnapshot, kFrames);
    RoutedHarness hi(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(hi.block, psnapshot, levels, pool_hi, workers).ok());
    CHECK(exec.stats().parallel_levels_dispatched == 0);
    CHECK(exec.stats().serial_levels_run >= 1);

    // Threshold 0: the gain level dispatches across the worker pool.
    exec.set_parallel_min_work_units(0);
    exec.reset_stats();
    auto pool_lo = make_pool(psnapshot, kFrames);
    RoutedHarness lo(kSr, kFrames, in, 1);
    REQUIRE(exec.process_parallel(lo.block, psnapshot, levels, pool_lo, workers).ok());
    CHECK(exec.stats().parallel_levels_dispatched >= 1);
    workers.stop();

    // The gate only chooses the path; output is identical either way.
    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(hi.outs[0][static_cast<std::size_t>(i)] ==
                lo.outs[0][static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("process_routed skips a node and reads its pre-filled output slot",
          "[format][graph][executor][routing][skip]") {
    // in -> A -> B -> out. Skipping A and pre-filling A's output slot with exactly
    // what A would have produced (x * gainA) must yield the same output as running
    // A — the mechanism anticipative rendering uses to splice a pre-rendered
    // interior into the live graph without re-running (and re-advancing) it.
    constexpr int kFrames = 64;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
    };
    GainState gA{2.0f}, gB{3.0f};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &gA, true},
        GraphRuntimeNodeBinding{3, routing_gain, &gB, true},
        GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
    const auto x = test_signal(kFrames, 0.5f);

    GraphRuntimeExecutor exec;
    auto pool_full = make_pool(snapshot, kFrames);
    RoutedHarness full(kSr, kFrames, {x}, 1);
    REQUIRE(full.run(exec, snapshot, pool_full).ok());

    // A's plan index + output slot.
    const auto& plan = snapshot.plan();
    std::uint32_t a_idx = 0;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i)
        if (plan.nodes[i].id == 2) a_idx = i;
    const std::uint32_t a_slot = snapshot.buffer_assignment().nodes[a_idx].output_base;

    // Skip A; pre-fill its output slot with x * gainA.
    auto pool_skip = make_pool(snapshot, kFrames);
    float* slot = pool_skip.slot_data(a_slot);
    REQUIRE(slot != nullptr);
    for (int i = 0; i < kFrames; ++i) slot[i] = x[static_cast<std::size_t>(i)] * 2.0f;

    std::vector<std::uint8_t> skip(plan.nodes.size(), 0);
    skip[a_idx] = 1;
    RoutedHarness skipped(kSr, kFrames, {x}, 1);
    REQUIRE(exec.process_routed(skipped.block, snapshot, pool_skip, nullptr, nullptr,
                                {}, {}, {}, {}, skip)
                .ok());

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE(skipped.outs[0][static_cast<std::size_t>(i)] ==
                full.outs[0][static_cast<std::size_t>(i)]);
    }
    // Guard against a vacuous all-zero match: the signal is actually non-trivial.
    CHECK(full.outs[0][1] != 0.0f);
}

namespace {

// in -> G1, G2 (fan-out, copied); G1 + G2 -> G3 port 0 (summed), G3 port 1
// unconnected (zeroed); G3 -> G4 (G3's only reader, read in place); G4 -> G5,
// and G5 -> G5 one block late (feedback, summed); G5 -> out.
struct MixedModeGraph {
    std::array<GraphRuntimeNodeSpec, 7> nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::Processor, 2, 1},
        GraphRuntimeNodeSpec{5, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{6, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{7, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    std::array<GraphRuntimeConnectionSpec, 8> conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
        GraphRuntimeConnectionSpec{4, 0, 5, 0},
        GraphRuntimeConnectionSpec{5, 0, 6, 0},
        GraphRuntimeConnectionSpec{6, 0, 6, 0, /*feedback=*/true},
        GraphRuntimeConnectionSpec{6, 0, 7, 0},
    };
    GainState g1{0.5f}, g2{-0.75f}, g3{1.25f}, g4{0.375f}, g5{2.0f};
    std::array<GraphRuntimeNodeBinding, 7> bindings = {
        GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
        GraphRuntimeNodeBinding{2, routing_gain, &g1, true},
        GraphRuntimeNodeBinding{3, routing_gain, &g2, true},
        GraphRuntimeNodeBinding{4, routing_gain, &g3, true},
        GraphRuntimeNodeBinding{5, routing_gain, &g4, true},
        GraphRuntimeNodeBinding{6, routing_gain, &g5, true},
        GraphRuntimeNodeBinding{7, nullptr, nullptr, false},
    };
};

} // namespace

TEST_CASE("A bound pool routes bit-exact with an unbound pool across blocks and frame counts",
          "[format][graph][executor][routing][parity]") {
    using Mode = GraphRuntimeSnapshot::InputPortSource::Mode;
    constexpr int kMaxFrames = 64;
    MixedModeGraph graph;
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, graph.nodes, graph.conns, graph.bindings));
    REQUIRE(snapshot.buffer_assignment().has_feedback);

    // The topology exercises every way an input port can read.
    std::array<int, 4> mode_count{};
    for (std::uint32_t i = 0; i < snapshot.plan().nodes.size(); ++i)
        for (const auto& port : snapshot.input_port_sources(i))
            ++mode_count[static_cast<std::size_t>(port.mode)];
    CHECK(mode_count[static_cast<std::size_t>(Mode::Zero)] > 0);
    CHECK(mode_count[static_cast<std::size_t>(Mode::InPlace)] > 0);
    CHECK(mode_count[static_cast<std::size_t>(Mode::Copy)] > 0);
    CHECK(mode_count[static_cast<std::size_t>(Mode::Sum)] > 0);

    auto bound_pool = make_pool(snapshot, kMaxFrames);
    REQUIRE(bound_pool.bind(snapshot));
    REQUIRE(bound_pool.bound_to(snapshot));
    auto unbound_pool = make_pool(snapshot, kMaxFrames);
    REQUIRE_FALSE(unbound_pool.bound_to(snapshot));

    GraphRuntimeExecutor exec;
    float energy = 0.0f;
    int block_index = 0;
    for (int frames : {64, 17, 1, 64, 33, 64, 5}) {
        CAPTURE(block_index, frames);
        const std::vector<std::vector<float>> in{
            test_signal(frames, 0.3f + 0.1f * static_cast<float>(block_index++))};
        RoutedHarness bound(kSr, frames, in, 1);
        RoutedHarness unbound(kSr, frames, in, 1);
        REQUIRE(bound.run(exec, snapshot, bound_pool).ok());
        REQUIRE(unbound.run(exec, snapshot, unbound_pool).ok());
        for (int i = 0; i < frames; ++i) {
            const auto bound_sample = bound.outs[0][static_cast<std::size_t>(i)];
            const auto unbound_sample = unbound.outs[0][static_cast<std::size_t>(i)];
            REQUIRE(std::bit_cast<std::uint32_t>(bound_sample) ==
                    std::bit_cast<std::uint32_t>(unbound_sample));
            energy += std::abs(bound_sample);
        }
    }
    // Guard against a vacuous all-zero match.
    CHECK(energy > 1.0f);
}

TEST_CASE("A bound pool safely handles an empty valid snapshot",
          "[format][graph][executor][routing][rt-safety]") {
    const std::array<GraphRuntimeNodeSpec, 0> nodes{};
    const std::array<GraphRuntimeConnectionSpec, 0> connections{};
    const std::array<GraphRuntimeNodeBinding, 0> bindings{};
    GraphRuntimeSnapshot snapshot;
    REQUIRE(make_snapshot(snapshot, nodes, connections, bindings));
    REQUIRE(snapshot.valid());
    REQUIRE(snapshot.node_count() == 0);

    GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), 64));
    REQUIRE(pool.bind(snapshot));
    REQUIRE(pool.bound_to(snapshot));
}

TEST_CASE("A pool routes another snapshot through the unbound path and unbinds on reset or copy",
          "[format][graph][executor][routing]") {
    constexpr int kFrames = 32;
    MixedModeGraph graph;
    GraphRuntimeSnapshot first;
    REQUIRE(make_snapshot(first, graph.nodes, graph.conns, graph.bindings));

    // Same node ids and port counts, different wiring: G1 and G2 now run in
    // series, G3 reads G2 and the graph input, and the feedback loops on G4.
    MixedModeGraph rewired;
    rewired.conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
        GraphRuntimeConnectionSpec{1, 0, 4, 1},
        GraphRuntimeConnectionSpec{4, 0, 5, 0},
        GraphRuntimeConnectionSpec{5, 0, 5, 0, /*feedback=*/true},
        GraphRuntimeConnectionSpec{5, 0, 6, 0},
        GraphRuntimeConnectionSpec{6, 0, 7, 0},
    };
    GraphRuntimeSnapshot second;
    REQUIRE(make_snapshot(second, rewired.nodes, rewired.conns, rewired.bindings));
    REQUIRE(first.id() != second.id());

    const auto slots = std::max(first.buffer_slot_count(), second.buffer_slot_count());
    GraphRuntimeBufferPool stale;
    REQUIRE(stale.reset(slots, kFrames));
    REQUIRE(stale.bind(first));
    CHECK_FALSE(stale.bound_to(second));
    GraphRuntimeBufferPool fresh;
    REQUIRE(fresh.reset(slots, kFrames));

    GraphRuntimeExecutor exec;
    float energy = 0.0f;
    for (int blk = 0; blk < 4; ++blk) {
        CAPTURE(blk);
        const std::vector<std::vector<float>> in{
            test_signal(kFrames, 0.4f + 0.1f * static_cast<float>(blk))};
        RoutedHarness via_stale(kSr, kFrames, in, 1);
        RoutedHarness via_fresh(kSr, kFrames, in, 1);
        REQUIRE(via_stale.run(exec, second, stale).ok());
        REQUIRE(via_fresh.run(exec, second, fresh).ok());
        for (int i = 0; i < kFrames; ++i) {
            const auto a = via_stale.outs[0][static_cast<std::size_t>(i)];
            REQUIRE(std::bit_cast<std::uint32_t>(a) ==
                    std::bit_cast<std::uint32_t>(via_fresh.outs[0][static_cast<std::size_t>(i)]));
            energy += std::abs(a);
        }
    }
    CHECK(energy > 1.0f);

    // A copy holds its own storage, so it starts unbound; the original stays
    // bound until it is reset.
    REQUIRE(stale.bind(first));
    const GraphRuntimeBufferPool copy = stale;
    CHECK_FALSE(copy.bound_to(first));
    CHECK(stale.bound_to(first));
    REQUIRE(stale.reset(slots, kFrames));
    CHECK_FALSE(stale.bound_to(first));

    // Resetting the snapshot gives it a new identity, even with the same contents.
    REQUIRE(stale.bind(first));
    REQUIRE(make_snapshot(first, graph.nodes, graph.conns, graph.bindings));
    CHECK_FALSE(stale.bound_to(first));
}

TEST_CASE("A sole output writes its own channels and every other output channel is silent",
          "[format][graph][executor][routing]") {
    // in -> G -> out(1 port) on a two-channel output bus that starts full of
    // garbage. Channel 0 is the sole output's, channel 1 nobody's. When G is a
    // required node with no processor the block fails before the output runs,
    // and both channels must come back silent.
    constexpr int kFrames = 16;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    const auto x = test_signal(kFrames, 0.5f);
    for (const bool fails : {false, true}) {
        CAPTURE(fails);
        GainState gain{0.5f};
        const std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            fails ? GraphRuntimeNodeBinding{2, nullptr, nullptr, true}
                  : GraphRuntimeNodeBinding{2, routing_gain, &gain, true},
            GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
        };
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
        REQUIRE(snapshot.sole_audio_output());
        REQUIRE(snapshot.sole_audio_output_ports() == 1);
        for (const bool bind : {false, true}) {
            CAPTURE(bind);
            auto pool = make_pool(snapshot, kFrames);
            if (bind)
                REQUIRE(pool.bind(snapshot));
            RoutedHarness h(kSr, kFrames, {x}, 2);
            for (auto& channel : h.outs)
                std::fill(channel.begin(), channel.end(), 123.0f);
            GraphRuntimeExecutor exec;
            const auto result = h.run(exec, snapshot, pool);
            CHECK(result.ok() == !fails);
            for (int i = 0; i < kFrames; ++i) {
                const auto at = static_cast<std::size_t>(i);
                REQUIRE(h.outs[0][at] == (fails ? 0.0f : x[at] * 0.5f));
                REQUIRE(h.outs[1][at] == 0.0f);
            }
        }
    }
}

namespace {

// A gain that records the first input channel pointer it was handed.
struct InputPointerProbe {
    float gain = 1.0f;
    const float* seen = nullptr;
};

bool probe_gain(pulp::format::ProcessBlock&,
                const pulp::format::GraphRuntimeNodeProcessContext& ctx, void* user_data) noexcept {
    auto* probe = static_cast<InputPointerProbe*>(user_data);
    probe->seen = ctx.node_inputs.channel_ptr(0);
    auto out = ctx.node_outputs;
    for (std::size_t i = 0; i < out.num_samples(); ++i)
        out.channel_ptr(0)[i] = ctx.node_inputs.channel_ptr(0)[i] * probe->gain;
    return true;
}

} // namespace

TEST_CASE("Read-only readers of the graph input read the host input bus in place",
          "[format][graph][executor][routing]") {
    // in -> G1 (one port) and in -> G2 (port 0; port 1 unconnected, so G2 still
    // gathers); G1 + G2 -> out. With a bound pool and read-only readers, both
    // read the host's input channel itself. A reader that may write its input,
    // a host processing in place, or an unbound pool all go through the
    // AudioInput slot. The output is the same bits every way, and after the
    // block the bound views point back at the slot.
    constexpr int kFrames = 32;
    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::Processor, 2, 1},
        GraphRuntimeNodeSpec{4, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{1, 0, 3, 0},
        GraphRuntimeConnectionSpec{2, 0, 4, 0},
        GraphRuntimeConnectionSpec{3, 0, 4, 0},
    };
    const auto x = test_signal(kFrames, 0.7f);
    std::vector<float> expected(kFrames);
    for (int i = 0; i < kFrames; ++i)
        expected[static_cast<std::size_t>(i)] =
            x[static_cast<std::size_t>(i)] * 0.5f + x[static_cast<std::size_t>(i)] * -0.25f;

    struct Case {
        bool bind;
        bool read_only;
        bool in_place_host;
        bool expect_alias;
    };
    for (const Case c : {Case{true, true, false, true}, Case{true, false, false, false},
                         Case{true, true, true, false}, Case{false, true, false, false}}) {
        CAPTURE(c.bind, c.read_only, c.in_place_host);
        InputPointerProbe g1{0.5f}, g2{-0.25f};
        std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            GraphRuntimeNodeBinding{2, probe_gain, &g1, true},
            GraphRuntimeNodeBinding{3, probe_gain, &g2, true},
            GraphRuntimeNodeBinding{4, nullptr, nullptr, false},
        };
        bindings[1].reads_inputs_only = c.read_only;
        bindings[2].reads_inputs_only = c.read_only;
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, nodes, conns, bindings));
        auto pool = make_pool(snapshot, kFrames);
        if (c.bind)
            REQUIRE(pool.bind(snapshot));

        std::vector<float> host = x;
        std::vector<float> separate_out(kFrames, 0.0f);
        float* out_channel = c.in_place_host ? host.data() : separate_out.data();
        const float* in_channels[] = {host.data()};
        float* out_channels[] = {out_channel};
        pulp::format::BusBufferSet buses;
        REQUIRE(buses.add_input("main", {in_channels, 1, kFrames}, pulp::format::BusRole::Main));
        REQUIRE(buses.add_output("main", {out_channels, 1, kFrames}, pulp::format::BusRole::Main));
        pulp::format::ProcessBlock block;
        block.sample_rate = kSr;
        block.frame_count = kFrames;
        block.buses = &buses;
        GraphRuntimeExecutor exec;
        REQUIRE(exec.process_routed(block, snapshot, pool).ok());

        CHECK((g1.seen == host.data()) == c.expect_alias);
        CHECK((g2.seen == host.data()) == c.expect_alias);
        for (int i = 0; i < kFrames; ++i) {
            const auto at = static_cast<std::size_t>(i);
            REQUIRE(std::bit_cast<std::uint32_t>(out_channel[at]) ==
                    std::bit_cast<std::uint32_t>(expected[at]));
        }
        if (c.bind) {
            const auto& plan = snapshot.plan();
            for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                if (plan.nodes[i].id != 2 && plan.nodes[i].id != 3)
                    continue;
                CHECK(pool.bound_node(i).inputs[0] != host.data());
            }
        }
    }

    // A sole reader reads its source in place whether or not it writes its
    // input, so only the read-only declaration keeps a writer off the host
    // input.
    const std::array chain_nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::AudioInput, 0, 1},
        GraphRuntimeNodeSpec{2, GraphRuntimeNodeKind::Processor, 1, 1},
        GraphRuntimeNodeSpec{3, GraphRuntimeNodeKind::AudioOutput, 1, 0},
    };
    const std::array chain_conns = {
        GraphRuntimeConnectionSpec{1, 0, 2, 0},
        GraphRuntimeConnectionSpec{2, 0, 3, 0},
    };
    for (const bool read_only : {true, false}) {
        CAPTURE(read_only);
        InputPointerProbe sole{2.0f};
        std::array bindings = {
            GraphRuntimeNodeBinding{1, nullptr, nullptr, false},
            GraphRuntimeNodeBinding{2, probe_gain, &sole, true},
            GraphRuntimeNodeBinding{3, nullptr, nullptr, false},
        };
        bindings[1].reads_inputs_only = read_only;
        GraphRuntimeSnapshot snapshot;
        REQUIRE(make_snapshot(snapshot, chain_nodes, chain_conns, bindings));
        auto pool = make_pool(snapshot, kFrames);
        REQUIRE(pool.bind(snapshot));
        RoutedHarness h(kSr, kFrames, {x}, 1);
        GraphRuntimeExecutor exec;
        REQUIRE(h.run(exec, snapshot, pool).ok());
        CHECK((sole.seen == h.ins[0].data()) == read_only);
        for (int i = 0; i < kFrames; ++i) {
            const auto at = static_cast<std::size_t>(i);
            REQUIRE(h.outs[0][at] == x[at] * 2.0f);
        }
    }
}
