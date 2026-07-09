#include <catch2/catch_test_macros.hpp>

#include "pulp/host/signal_graph.hpp"
#include "pulp/audio/live_dsp_telemetry.hpp"
#include "pulp/runtime/scoped_no_alloc.hpp"

#include "harness/rt_allocation_probe.hpp"

#include <array>
#include <string>
#include <vector>

using namespace pulp::host;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

// input(2) -> gain -> output(2). Legacy reference walk (routing left off), so the
// per-node telemetry scopes in run_reference_walk_ are exercised.
void build_in_gain_out(SignalGraph& g, NodeId& in, NodeId& gain, NodeId& out) {
    in = g.add_input_node(2, "In");
    gain = g.add_gain_node("Gain");
    out = g.add_output_node(2, "Out");
    for (int c = 0; c < 2; ++c) {
        REQUIRE(g.connect(in, c, gain, c));
        REQUIRE(g.connect(gain, c, out, c));
    }
    REQUIRE(g.prepare(kSr, kFrames));
}

void render_block(SignalGraph& g, float level = 0.25f) {
    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> in_l(kFrames, level), in_r(kFrames, level);
    std::array<const float*, 2> ic{in_l.data(), in_r.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);
    g.process(ov, iv, kFrames);
}

}  // namespace

TEST_CASE("live-dsp telemetry is disabled by default on the graph", "[host][signal-graph][live-dsp-telemetry]") {
    SignalGraph g;
    NodeId in, gain, out;
    build_in_gain_out(g, in, gain, out);

    REQUIRE_FALSE(g.live_dsp_telemetry_enabled());
    for (int i = 0; i < 8; ++i) render_block(g);

    // Draining with telemetry off records nothing (begin_block returns inactive).
    const auto snap = g.poll_live_dsp_telemetry();
    REQUIRE(snap.blocks_written == 0);
    for (const auto& n : snap.nodes) {
        REQUIRE(n.sample_count == 0);
    }
}

TEST_CASE("live-dsp telemetry records per-node timing on the reference walk", "[host][signal-graph][live-dsp-telemetry]") {
    SignalGraph g;
    NodeId in, gain, out;
    build_in_gain_out(g, in, gain, out);

    g.set_live_dsp_telemetry_enabled(true);
    REQUIRE(g.live_dsp_telemetry_enabled());

    constexpr int kBlocks = 32;
    for (int i = 0; i < kBlocks; ++i) render_block(g);

    const auto snap = g.poll_live_dsp_telemetry();
    REQUIRE(snap.available);
    REQUIRE(snap.enabled);
    REQUIRE(snap.node_count == 3);
    REQUIRE(snap.nodes.size() == 3);
    REQUIRE(snap.blocks_written == static_cast<std::uint64_t>(kBlocks));
    REQUIRE(snap.blocks_drained == static_cast<std::uint64_t>(kBlocks));

    // Every ordered node recorded on each block; kinds/names map from NodeType.
    std::vector<pulp::audio::LiveDspNodeKind> kinds;
    for (const auto& n : snap.nodes) {
        REQUIRE(n.sample_count == static_cast<std::uint64_t>(kBlocks));
        REQUIRE(n.p50_elapsed_ns >= 0);
        REQUIRE(n.p95_elapsed_ns >= n.p50_elapsed_ns);
        REQUIRE(n.jitter_ns == n.p95_elapsed_ns - n.p50_elapsed_ns);
        REQUIRE(std::string(n.name) == pulp::audio::to_string(n.kind));
        kinds.push_back(n.kind);
    }
    // The three node kinds are present (order is topological: in -> gain -> out).
    auto has = [&](pulp::audio::LiveDspNodeKind k) {
        return std::find(kinds.begin(), kinds.end(), k) != kinds.end();
    };
    REQUIRE(has(pulp::audio::LiveDspNodeKind::AudioInput));
    REQUIRE(has(pulp::audio::LiveDspNodeKind::Gain));
    REQUIRE(has(pulp::audio::LiveDspNodeKind::AudioOutput));

    // Graph-level fields are populated and sane for a light 3-node graph.
    REQUIRE(snap.last_frame_count == static_cast<std::uint32_t>(kFrames));
    REQUIRE(snap.last_available_ns > 0);
    REQUIRE(snap.last_graph_load >= 0.0f);
}

TEST_CASE("live-dsp telemetry toggles the live snapshot without a recompile", "[host][signal-graph][live-dsp-telemetry]") {
    SignalGraph g;
    NodeId in, gain, out;
    build_in_gain_out(g, in, gain, out);

    for (int i = 0; i < 4; ++i) render_block(g);
    REQUIRE(g.poll_live_dsp_telemetry().blocks_written == 0);  // off

    g.set_live_dsp_telemetry_enabled(true);
    for (int i = 0; i < 4; ++i) render_block(g);
    REQUIRE(g.poll_live_dsp_telemetry().blocks_written == 4);  // on, no recompile

    g.set_live_dsp_telemetry_enabled(false);
    for (int i = 0; i < 4; ++i) render_block(g);
    // Still 4 written total: the 4 blocks while disabled recorded nothing.
    REQUIRE(g.poll_live_dsp_telemetry().blocks_written == 4);
}

TEST_CASE("live-dsp telemetry does not allocate on the audio path", "[host][signal-graph][live-dsp-telemetry][rt-safety]") {
    SignalGraph g;
    NodeId in, gain, out;
    build_in_gain_out(g, in, gain, out);
    g.set_live_dsp_telemetry_enabled(true);

    // Warm up (first blocks may touch lazy runtime state outside telemetry).
    for (int i = 0; i < 4; ++i) render_block(g);

    std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
    std::array<float*, 2> oc{l.data(), r.data()};
    std::vector<float> in_l(kFrames, 0.25f), in_r(kFrames, 0.25f);
    std::array<const float*, 2> ic{in_l.data(), in_r.data()};
    pulp::audio::BufferView<const float> iv(ic.data(), 2, kFrames);
    pulp::audio::BufferView<float> ov(oc.data(), 2, kFrames);

    {
        pulp::test::RtAllocationProbe probe;
        pulp::runtime::ScopedNoAlloc no_alloc;
        for (int i = 0; i < 64; ++i) g.process(ov, iv, kFrames);
        REQUIRE_FALSE(probe.saw_allocation());
    }
}
