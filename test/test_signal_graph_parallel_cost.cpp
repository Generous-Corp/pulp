// Production-seam coverage for SignalGraph's parallel executor cost gate.
// Executor-level tests prove the threshold branch; this file proves the host
// API forwards the threshold into the live SignalGraph::process() path.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using pulp::host::SignalGraph;
using pulp::host::signal_graph_executor_eligible;

constexpr double kSr = 48000.0;
constexpr int kFrames = 128;

std::vector<float> ramp(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

std::vector<std::vector<float>> process_block(
    SignalGraph& g,
    const std::vector<std::vector<float>>& in,
    std::size_t out_channels) {
    std::vector<std::vector<float>> ins = in;
    std::vector<std::vector<float>> outs(
        out_channels, std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f));
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    for (auto& c : ins) in_ptrs.push_back(c.data());
    for (auto& c : outs) out_ptrs.push_back(c.data());
    pulp::audio::BufferView<const float> in_view(
        in_ptrs.data(), in_ptrs.size(), static_cast<std::uint32_t>(kFrames));
    pulp::audio::BufferView<float> out_view(
        out_ptrs.data(), out_ptrs.size(), static_cast<std::uint32_t>(kFrames));
    g.process(out_view, in_view, kFrames);
    return outs;
}

void expect_equal(const std::vector<std::vector<float>>& a,
                  const std::vector<std::vector<float>>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t ch = 0; ch < a.size(); ++ch) {
        REQUIRE(a[ch].size() == b[ch].size());
        for (std::size_t i = 0; i < a[ch].size(); ++i) {
            REQUIRE(a[ch][i] == b[ch][i]);
        }
    }
}

// in(2ch) -> N parallel gains -> out(2ch). A wide level so the parallel executor
// can either dispatch across workers or stay serial behind the threshold gate.
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

TEST_CASE("SignalGraph::process parallel path honors the executor cost gate",
          "[host][graph][executor][routing][parallel][cost]") {
    SignalGraph legacy, gated, open;
    build_wide_signal_graph(legacy, 16, /*parallel=*/false);
    build_wide_signal_graph(gated, 16, /*parallel=*/true);
    build_wide_signal_graph(open, 16, /*parallel=*/true);
    REQUIRE(signal_graph_executor_eligible(gated));
    REQUIRE(signal_graph_executor_eligible(open));

    // build_wide_signal_graph has a 16-node, stereo Gain level: static weight
    // 16 * max(2, 2) = 32, and 32 * 128 frames = 4096 channel-samples.
    CHECK(gated.parallel_min_work_units() == 0);
    gated.set_parallel_min_work_units(5000);
    CHECK(gated.parallel_min_work_units() == 5000);
    open.set_parallel_min_work_units(0);

    const std::vector<std::vector<float>> input{ramp(kFrames, 0.67f),
                                                ramp(kFrames, 0.43f)};
    const auto expected = process_block(legacy, input, 2);
    const auto gated_out = process_block(gated, input, 2);
    const auto open_out = process_block(open, input, 2);
    expect_equal(expected, gated_out);
    expect_equal(expected, open_out);

    const auto gated_stats = gated.routing_executor_stats();
    CHECK(gated_stats.parallel_levels_dispatched == 0);
    CHECK(gated_stats.serial_levels_run >= 1);

    const auto open_stats = open.routing_executor_stats();
    CHECK(open_stats.parallel_levels_dispatched >= 1);
}
