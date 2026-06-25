// Shared plumbing for graph inter-node routing parity tests.
//
// Both the fixed-topology routing tests and the randomized differential parity
// tests build a GraphRuntimeSnapshot, size a buffer pool, drive a mono/stereo
// block through the routed executor, and read per-channel output. This header
// owns that plumbing so each test file is just "topology + bindings +
// assertion" and the executor-facing setup lives in exactly one place.

#ifndef PULP_TEST_HARNESS_GRAPH_ROUTING_HARNESS_HPP
#define PULP_TEST_HARNESS_GRAPH_ROUTING_HARNESS_HPP

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::test::graph_routing {

// Per-node gain carried as a node binding's user_data.
struct GainState {
    float gain = 1.0f;
};

// Bus-agnostic gain binding: read the gathered per-node input view, write the
// per-node output view. Summing inbound edges into the input is the executor's
// job, so this stays pure DSP.
inline bool routing_gain(pulp::format::ProcessBlock&,
                         const pulp::format::GraphRuntimeNodeProcessContext& ctx,
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

// Deterministic pseudo-signal so cases are reproducible across runs/seeds.
inline std::vector<float> test_signal(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

// Build an immutable snapshot from node/connection specs + bindings. Returns
// false if the plan is rejected (e.g. an unbroken cycle).
inline bool make_snapshot(pulp::format::GraphRuntimeSnapshot& snapshot,
                          std::span<const pulp::graph::GraphRuntimeNodeSpec> nodes,
                          std::span<const pulp::graph::GraphRuntimeConnectionSpec> conns,
                          std::span<const pulp::format::GraphRuntimeNodeBinding> bindings,
                          bool parallel_safe = false) {
    auto plan = pulp::graph::build_graph_runtime_plan(nodes, conns);
    if (!plan.ok()) return false;
    return snapshot.reset(std::move(plan.plan), bindings, parallel_safe);
}

inline pulp::format::GraphRuntimeBufferPool make_pool(
    const pulp::format::GraphRuntimeSnapshot& snapshot, int frames) {
    pulp::format::GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), static_cast<std::uint32_t>(frames)));
    return pool;
}

// Owns the per-call input/output channel storage + buses + block for one routed
// executor call. `inputs` is one buffer per main-input channel; `run()` drives
// the executor and `outs` exposes per-channel output.
struct RoutedHarness {
    std::vector<std::vector<float>> ins;
    std::vector<std::vector<float>> outs;
    std::vector<const float*> in_ptrs;
    std::vector<float*> out_ptrs;
    pulp::format::BusBufferSet buses;
    pulp::format::ProcessBlock block;
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
        REQUIRE(buses.add_input("main", in_view, pulp::format::BusRole::Main));
        REQUIRE(buses.add_output("main", out_view, pulp::format::BusRole::Main));
        block.sample_rate = sr;
        block.frame_count = static_cast<std::uint32_t>(frames);
        block.buses = &buses;
        REQUIRE(block.validate());
    }

    pulp::format::GraphRuntimeExecutorResult run(
        pulp::format::GraphRuntimeExecutor& exec,
        const pulp::format::GraphRuntimeSnapshot& snapshot,
        pulp::format::GraphRuntimeBufferPool& pool) {
        return exec.process_routed(block, snapshot, pool);
    }
};

} // namespace pulp::test::graph_routing

#endif // PULP_TEST_HARNESS_GRAPH_ROUTING_HARNESS_HPP
