// Retained Release benchmark for the routed graph input-gather shapes.
//
// The executable is deliberately standalone: it uses one fixed source buffer,
// preallocated routed scratch, identical warmup and timing loops, and a
// bit-exact output oracle for every topology/block-size pair. It is intended
// for paired baseline/candidate runs, not as a CTest timing gate.

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

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
constexpr int kSamples = 7;

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

bool build_case(Shape shape, int frames, Case& result, std::string& error) {
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
        add_node(2, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(3, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(4, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(5, GraphRuntimeNodeKind::AudioOutput, 1, 0);
        connect(1, 2);
        connect(2, 3);
        connect(3, 4);
        connect(4, 5);
        result.states.assign(3, PassState{});
        result.expected = fixed_input(frames);
    } else {
        add_node(2, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(3, GraphRuntimeNodeKind::Processor, 1, 1);
        add_node(4, GraphRuntimeNodeKind::AudioOutput, 1, 0);
        connect(1, 2);
        connect(1, 3);
        connect(2, 4);
        connect(3, 4);
        result.states.assign(2, PassState{});
        result.expected = fixed_input(frames);
        for (auto& value : result.expected)
            value *= 2.0f;
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

bool oracle_matches(const Case& test) {
    for (std::size_t frame = 0; frame < test.expected.size(); ++frame) {
        const auto actual_bits = std::bit_cast<std::uint32_t>(test.output[0][frame]);
        const auto expected_bits = std::bit_cast<std::uint32_t>(test.expected[frame]);
        if (actual_bits != expected_bits)
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

double timed_sample(Case& test, GraphRuntimeExecutor& executor, double overhead_ns) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kTimedBlocks; ++i) {
        if (!run_one(test, executor))
            return std::numeric_limits<double>::quiet_NaN();
    }
    const auto end = std::chrono::steady_clock::now();
    const auto total_ns = std::chrono::duration<double, std::nano>(end - start).count();
    return std::max(0.0, total_ns / static_cast<double>(kTimedBlocks) - overhead_ns);
}

int run() {
    const auto overhead_ns = timer_overhead_ns();
    std::cout << "graph_direct_copy_benchmark_v1 timer_overhead_ns=" << std::fixed
              << std::setprecision(3) << overhead_ns << '\n';
    for (const auto shape : {Shape::Passthrough, Shape::Chain, Shape::Fanout}) {
        for (const int frames : {16, 32, 64, 128, 512}) {
            Case test;
            std::string error;
            if (!build_case(shape, frames, test, error)) {
                std::cerr << "error shape=" << shape_name(shape) << " block=" << frames << " "
                          << error << '\n';
                return EXIT_FAILURE;
            }
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
            std::array<double, kSamples> samples{};
            for (auto& sample : samples) {
                sample = timed_sample(test, executor, overhead_ns);
                if (!std::isfinite(sample)) {
                    std::cerr << "timing_failed shape=" << shape_name(shape) << " block=" << frames
                              << '\n';
                    return EXIT_FAILURE;
                }
                if (!oracle_matches(test)) {
                    std::cerr << "oracle_failed_after_timing shape=" << shape_name(shape)
                              << " block=" << frames << '\n';
                    return EXIT_FAILURE;
                }
            }
            std::sort(samples.begin(), samples.end());
            const auto median_ns = samples[kSamples / 2];
            std::cout << "shape=" << shape_name(shape) << " block=" << frames
                      << " warmup=" << kWarmupBlocks << " iterations=" << kTimedBlocks
                      << " samples=" << kSamples << " corrected_ns=" << std::fixed
                      << std::setprecision(3) << median_ns << " oracle=pass"
                      << " output_bits=0x" << std::hex
                      << std::bit_cast<std::uint32_t>(test.output[0][0]) << std::dec << '\n';
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

int main() {
    return run();
}
