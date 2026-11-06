// Bounded Release benchmark for graph planning, scratch assignment, and execution.
//
// The executable is deliberately standalone: it uses one fixed source buffer,
// preallocated routed scratch, identical warmup and timing loops, and a
// deterministic output oracle for every topology/block-size pair. It is intended
// for paired baseline/candidate runs, not as a CTest timing gate.

#include "harness/rt_allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <span>
#include <string_view>
#include <vector>

namespace {

using pulp::format::BusBufferSet;
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

constexpr double kSampleRate = 48000.0;
constexpr int kWarmupBlocks = 500;
constexpr int kTimedBlocks = 100000;
constexpr int kSamples = 31;

struct PassState {
    float gain = 1.0f;
};

bool pass_through(ProcessBlock&, const GraphRuntimeNodeProcessContext& context,
                  void* user_data) noexcept {
    const auto gain = static_cast<const PassState*>(user_data)->gain;
    const auto& input = context.node_inputs;
    auto output = context.node_outputs;
    const auto channels = std::min(input.num_channels(), output.num_channels());
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto* source = input.channel_ptr(channel);
        auto* destination = output.channel_ptr(channel);
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame)
            destination[frame] = source[frame] * gain;
    }
    return true;
}

enum class Shape { Passthrough, Chain, Fanout };

std::string_view shape_name(Shape shape) {
    switch (shape) {
    case Shape::Passthrough:
        return "passthrough";
    case Shape::Chain:
        return "chain";
    case Shape::Fanout:
        return "fan-out";
    }
    return "unknown";
}

struct Case {
    int scale = 1;
    GraphRuntimeSnapshot snapshot;
    GraphRuntimeBufferPool pool;
    std::vector<PassState> states;
    std::vector<std::vector<float>> input;
    std::vector<std::vector<float>> output;
    std::vector<const float*> input_ptrs;
    std::vector<float*> output_ptrs;
    BusBufferSet buses;
    ProcessBlock block;
    std::vector<float> expected;
};

std::vector<float> fixed_input(int frames) {
    std::vector<float> values(static_cast<std::size_t>(frames));
    for (int frame = 0; frame < frames; ++frame) {
        // Fixed finite values keep the oracle bit-exact while exercising more
        // than a constant/zero input pattern.
        const auto word = static_cast<std::uint32_t>(frame * 2654435761u) & 0xFFFFu;
        values[static_cast<std::size_t>(frame)] =
            (static_cast<float>(word) / 32768.0f - 1.0f) * 0.73f;
    }
    return values;
}

bool build_case(Shape shape, int frames, int scale, Case& result, std::string& error) {
    result.scale = scale;
    std::vector<GraphRuntimeNodeSpec> nodes;
    std::vector<GraphRuntimeConnectionSpec> connections;
    const auto add_node = [&](pulp::graph::NodeId id, GraphRuntimeNodeKind kind,
                              std::uint32_t inputs, std::uint32_t outputs) {
        nodes.push_back(GraphRuntimeNodeSpec{id, kind, inputs, outputs});
    };
    const auto connect = [&](pulp::graph::NodeId source, pulp::graph::NodeId destination) {
        GraphRuntimeConnectionSpec connection;
        connection.source_node = source;
        connection.dest_node = destination;
        connections.push_back(connection);
    };

    add_node(1, GraphRuntimeNodeKind::AudioInput, 0, 1);
    if (shape == Shape::Passthrough) {
        add_node(2, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(3, GraphRuntimeNodeKind::AudioOutput, 1, 0);
        connect(1, 2);
        connect(2, 3);
        result.states.emplace_back(PassState{});
        result.expected = fixed_input(frames);
    } else if (shape == Shape::Chain) {
        const int count = std::max(1, scale);
        for (int i = 0; i < count; ++i)
            add_node(static_cast<pulp::graph::NodeId>(2 + i), GraphRuntimeNodeKind::Processor, 1,
                     1);
        add_node(static_cast<pulp::graph::NodeId>(2 + count), GraphRuntimeNodeKind::AudioOutput, 1,
                 0);
        connect(1, 2);
        for (int i = 0; i + 1 < count; ++i)
            connect(static_cast<pulp::graph::NodeId>(2 + i),
                    static_cast<pulp::graph::NodeId>(3 + i));
        connect(static_cast<pulp::graph::NodeId>(1 + count),
                static_cast<pulp::graph::NodeId>(2 + count));
        result.states.assign(static_cast<std::size_t>(count), PassState{});
        result.expected = fixed_input(frames);
    } else {
        const int branches = std::max(2, scale);
        for (int i = 0; i < branches; ++i)
            add_node(static_cast<pulp::graph::NodeId>(2 + i), GraphRuntimeNodeKind::Processor, 1,
                     1);
        add_node(static_cast<pulp::graph::NodeId>(2 + branches), GraphRuntimeNodeKind::AudioOutput,
                 1, 0);
        for (int i = 0; i < branches; ++i) {
            connect(1, static_cast<pulp::graph::NodeId>(2 + i));
            connect(static_cast<pulp::graph::NodeId>(2 + i),
                    static_cast<pulp::graph::NodeId>(2 + branches));
        }
        result.states.assign(static_cast<std::size_t>(branches), PassState{});
        result.expected = fixed_input(frames);
        for (auto& value : result.expected)
            value *= static_cast<float>(branches);
    }

    std::vector<GraphRuntimeNodeBinding> bindings;
    bindings.reserve(nodes.size());
    std::size_t state_index = 0;
    for (const auto& node : nodes) {
        if (node.kind == GraphRuntimeNodeKind::Processor) {
            bindings.push_back(GraphRuntimeNodeBinding{node.id, pass_through,
                                                       &result.states[state_index++], true});
        } else {
            bindings.push_back(GraphRuntimeNodeBinding{node.id, nullptr, nullptr, false});
        }
    }

    auto plan = pulp::graph::build_graph_runtime_plan(nodes, connections);
    if (!plan.ok() || !result.snapshot.reset(std::move(plan.plan), bindings)) {
        error = "failed to build graph runtime snapshot";
        return false;
    }
    if (!result.pool.reset(result.snapshot.buffer_slot_count(),
                           static_cast<std::uint32_t>(frames))) {
        error = "failed to allocate graph runtime pool";
        return false;
    }

    result.input.emplace_back(fixed_input(frames));
    result.output.emplace_back(static_cast<std::size_t>(frames), 0.0f);
    result.input_ptrs.push_back(result.input[0].data());
    result.output_ptrs.push_back(result.output[0].data());
    pulp::audio::BufferView<const float> input_view(
        result.input_ptrs.data(), result.input_ptrs.size(), static_cast<std::uint32_t>(frames));
    pulp::audio::BufferView<float> output_view(result.output_ptrs.data(), result.output_ptrs.size(),
                                               static_cast<std::uint32_t>(frames));
    if (!result.buses.add_input("main", input_view, BusRole::Main) ||
        !result.buses.add_output("main", output_view, BusRole::Main)) {
        error = "failed to create process buses";
        return false;
    }
    result.block.sample_rate = kSampleRate;
    result.block.frame_count = static_cast<std::uint32_t>(frames);
    result.block.buses = &result.buses;
    if (!result.block.validate()) {
        error = "invalid process block";
        return false;
    }
    return true;
}

bool run_one(Case& test, GraphRuntimeExecutor& executor) {
    return executor.process_routed(test.block, test.snapshot, test.pool).ok();
}

// Fan-in reduction may differ by a few ulps as branch count grows; the
// bounded tolerance checks the semantic result, while repeated-run checks below
// still require bitwise stability.
bool oracle_matches(const Case& test) {
    for (std::size_t frame = 0; frame < test.expected.size(); ++frame) {
        const auto actual_bits = std::bit_cast<std::uint32_t>(test.output[0][frame]);
        const auto expected_bits = std::bit_cast<std::uint32_t>(test.expected[frame]);
        if (actual_bits != expected_bits &&
            std::fabs(test.output[0][frame] - test.expected[frame]) > 1.0e-4f)
            return false;
    }
    return true;
}

double timer_overhead_ns() {
    std::array<double, kSamples> samples{};
    for (auto& sample : samples) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kTimedBlocks; ++i) {
            // Keep the empty loop observable without doing work comparable to
            // graph processing.
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
        const auto end = std::chrono::steady_clock::now();
        sample = std::chrono::duration<double, std::nano>(end - start).count() /
                 static_cast<double>(kTimedBlocks);
    }
    std::sort(samples.begin(), samples.end());
    return samples[kSamples / 2];
}

struct TimedSample {
    double latency_ns = std::numeric_limits<double>::quiet_NaN();
    std::size_t allocations = 0;
};

TimedSample timed_sample(Case& test, GraphRuntimeExecutor& executor, double overhead_ns) {
    pulp::test::RtAllocationProbe probe;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTimedBlocks; ++i) {
        if (!run_one(test, executor))
            return {};
    }
    const auto end = std::chrono::steady_clock::now();
    const auto total_ns = std::chrono::duration<double, std::nano>(end - start).count();
    return {std::max(0.0, total_ns / static_cast<double>(kTimedBlocks) - overhead_ns),
            probe.allocation_count()};
}

int run() {
    const auto overhead_ns = timer_overhead_ns();
    std::cout << "graph_r2_measurement_v1\n";
    std::cout << "graph_direct_copy_benchmark_v1 timer_overhead_ns=" << std::fixed
              << std::setprecision(3) << overhead_ns << '\n';
    for (const auto shape : {Shape::Passthrough, Shape::Chain, Shape::Fanout}) {
        for (const int scale : {1, 4, 16, 64}) {
            if ((shape == Shape::Passthrough && scale != 1) ||
                (shape == Shape::Fanout && scale == 1))
                continue;
            for (const int frames : {16, 32, 64, 128, 512}) {
                Case test;
                std::string error;
                std::array<double, kSamples> activation_samples{};
                for (auto& sample : activation_samples) {
                    Case probe;
                    const auto build_start = std::chrono::steady_clock::now();
                    if (!build_case(shape, frames, scale, probe, error)) {
                        std::cerr << "error shape=" << shape_name(shape) << " block=" << frames
                                  << " " << error << '\n';
                        return EXIT_FAILURE;
                    }
                    const auto build_end = std::chrono::steady_clock::now();
                    sample =
                        std::chrono::duration<double, std::nano>(build_end - build_start).count();
                }
                if (!build_case(shape, frames, scale, test, error)) {
                    std::cerr << "error shape=" << shape_name(shape) << " block=" << frames << " "
                              << error << '\n';
                    return EXIT_FAILURE;
                }
                std::sort(activation_samples.begin(), activation_samples.end());
                const auto activation_p50_ns = activation_samples[kSamples / 2];
                const auto activation_p99_ns = activation_samples[(kSamples * 99 + 99) / 100 - 1];
                const auto activation_max_ns = activation_samples.back();
                std::cout << "shape=" << shape_name(shape) << " scale=" << scale
                          << " block=" << frames << " activation_p50_ns=" << std::fixed
                          << std::setprecision(3) << activation_p50_ns
                          << " activation_p99_sample_ns=" << activation_p99_ns
                          << " activation_max_ns=" << activation_max_ns
                          << " scratch_slots=" << test.snapshot.buffer_slot_count() << '\n';
                GraphRuntimeExecutor executor;
                for (int i = 0; i < kWarmupBlocks; ++i) {
                    if (!run_one(test, executor)) {
                        std::cerr << "error during warmup shape=" << shape_name(shape)
                                  << " block=" << frames << '\n';
                        return EXIT_FAILURE;
                    }
                }
                if (!oracle_matches(test)) {
                    std::cerr << "oracle_failed shape=" << shape_name(shape) << " block=" << frames
                              << '\n';
                    return EXIT_FAILURE;
                }
                std::vector<std::uint32_t> baseline_bits;
                baseline_bits.reserve(test.output[0].size());
                for (const float value : test.output[0])
                    baseline_bits.push_back(std::bit_cast<std::uint32_t>(value));
                std::size_t process_allocations = 0;
                {
                    pulp::test::RtAllocationProbe probe;
                    if (!run_one(test, executor)) {
                        std::cerr << "process_failed shape=" << shape_name(shape)
                                  << " block=" << frames << '\n';
                        return EXIT_FAILURE;
                    }
                    process_allocations = probe.allocation_count();
                }
                if (process_allocations != 0) {
                    std::cerr << "process_allocations shape=" << shape_name(shape)
                              << " scale=" << scale << " block=" << frames
                              << " count=" << process_allocations << '\n';
                    return EXIT_FAILURE;
                }
                if (!oracle_matches(test)) {
                    std::cerr << "oracle_failed_after_probe shape=" << shape_name(shape)
                              << " block=" << frames << '\n';
                    return EXIT_FAILURE;
                }
                for (std::size_t index = 0; index < baseline_bits.size(); ++index)
                    if (std::bit_cast<std::uint32_t>(test.output[0][index]) !=
                        baseline_bits[index]) {
                        std::cerr << "repeat_mismatch shape=" << shape_name(shape)
                                  << " block=" << frames << '\n';
                        return EXIT_FAILURE;
                    }
                std::array<double, kSamples> samples{};
                std::size_t timed_allocations = 0;
                for (auto& sample : samples) {
                    const auto timed = timed_sample(test, executor, overhead_ns);
                    sample = timed.latency_ns;
                    timed_allocations += timed.allocations;
                    if (!std::isfinite(sample) || timed.allocations != 0) {
                        std::cerr << "timed_allocations shape=" << shape_name(shape)
                                  << " scale=" << scale << " block=" << frames
                                  << " count=" << timed.allocations << '\n';
                        std::cerr << "timing_failed shape=" << shape_name(shape)
                                  << " block=" << frames << '\n';
                        return EXIT_FAILURE;
                    }
                    if (!oracle_matches(test)) {
                        std::cerr << "oracle_failed_after_timing shape=" << shape_name(shape)
                                  << " block=" << frames << '\n';
                        return EXIT_FAILURE;
                    }
                }
                std::sort(samples.begin(), samples.end());
                const auto p50_ns = samples[kSamples / 2];
                const auto p99_ns = samples[(kSamples * 99 + 99) / 100 - 1];
                const auto max_ns = samples.back();
                const auto jitter_ns = max_ns - samples.front();
                std::cout << "shape=" << shape_name(shape) << " scale=" << scale
                          << " block=" << frames << " warmup=" << kWarmupBlocks
                          << " iterations=" << kTimedBlocks << " samples=" << kSamples
                          << " p50_ns=" << std::fixed << std::setprecision(3) << p50_ns
                          << " p99_sample_ns=" << p99_ns << " max_ns=" << max_ns
                          << " jitter_ns=" << jitter_ns << " observed_wcet_proxy_ns=" << max_ns
                          << " oracle=pass"
                          << " process_allocations=" << process_allocations
                          << " timed_allocations=" << timed_allocations
                          << " timed_allocation_samples=" << kSamples << " output_bits=0x"
                          << std::hex << std::bit_cast<std::uint32_t>(test.output[0][0]) << std::dec
                          << '\n';
            }
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return run();
}
