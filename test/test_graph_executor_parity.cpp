// Serial-parity baseline: prove the canonical core/graph executor
// (pulp::format::GraphRuntimeExecutor) can drive a gain node to output that is
// BIT-IDENTICAL to the host graph (pulp::host::SignalGraph) for the same input.
//
// This is the regression baseline for converging SignalGraph onto the executor
// seam, and it pins a key architectural fact: the executor's shared-block path
// does NOT route inter-node audio — it walks nodes in topological order, drains
// the command queue, and hands every binding the SAME shared ProcessBlock.
// Audio routing is the binding's responsibility (or the routing process() path).
// So the parity here is "executor + a gain binding over the shared block" vs
// "SignalGraph input->gain->output": both compute out = in * g with the same
// float multiply, hence bit-identical. Inter-node routing is exercised
// separately in test_graph_executor_routing.cpp.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/format/process_block.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>
#include <pulp/graph/graph_runtime_queue.hpp>
#include <pulp/host/signal_graph.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using pulp::format::BusBufferSet;
using pulp::format::BusDirection;
using pulp::format::BusRole;
using pulp::format::GraphRuntimeExecutor;
using pulp::format::GraphRuntimeNodeBinding;
using pulp::format::GraphRuntimeNodeProcessContext;
using pulp::format::GraphRuntimeSnapshot;
using pulp::format::ProcessBlock;
using pulp::graph::GraphRuntimeNodeKind;
using pulp::graph::GraphRuntimeNodeSpec;
using pulp::graph::GraphRuntimeQueues;

struct GainState {
    float gain = 1.0f;
};

// Executor node binding: scalar gain over the shared block's main I/O buses.
// out[c][i] = in[c][i] * gain — identical float op to SignalGraph's Gain node.
bool gain_binding(ProcessBlock& block,
                  const GraphRuntimeNodeProcessContext&,
                  void* user_data) noexcept {
    const float gain = static_cast<const GainState*>(user_data)->gain;
    if (block.buses == nullptr) return false;
    auto* in_bus = block.buses->first(BusDirection::Input, BusRole::Main);
    auto* out_bus = block.buses->first(BusDirection::Output, BusRole::Main);
    if (in_bus == nullptr || out_bus == nullptr) return false;

    const auto& in = in_bus->input;
    auto& out = out_bus->output;
    const std::size_t chs = std::min(in.num_channels(), out.num_channels());
    const auto frames = block.frame_count;
    for (std::size_t c = 0; c < chs; ++c) {
        const float* ip = in.channel_ptr(c);
        float* op = out.channel_ptr(c);
        for (std::uint32_t i = 0; i < frames; ++i) {
            op[i] = ip[i] * gain;
        }
    }
    return true;
}

// Deterministic non-trivial test signal so a wrong gain shows up bit-wise.
std::vector<float> test_signal(int n, float seed) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        v[static_cast<std::size_t>(i)] =
            (static_cast<float>((i * 2654435761u) & 0xFFFF) / 32768.0f - 1.0f) * seed;
    }
    return v;
}

// Run input->gain->output through SignalGraph; return interleaved-by-channel
// output (channel 0 then channel 1), each `frames` long.
std::vector<float> run_signal_graph(float gain, double sr, int frames,
                                    const std::vector<float>& l,
                                    const std::vector<float>& r) {
    pulp::host::SignalGraph g;
    const auto in = g.add_input_node(2, "In");
    const auto gn = g.add_gain_node("Gain");
    const auto out = g.add_output_node(2, "Out");
    // SignalGraph uses one port per channel (port 0 = L, port 1 = R), so a
    // stereo path needs both port pairs wired through the 2-port gain node.
    REQUIRE(g.connect(in, 0, gn, 0));
    REQUIRE(g.connect(in, 1, gn, 1));
    REQUIRE(g.connect(gn, 0, out, 0));
    REQUIRE(g.connect(gn, 1, out, 1));
    REQUIRE(g.set_node_gain(gn, gain));
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

// Run the same gain through the GraphRuntimeExecutor; same output layout.
std::vector<float> run_executor(float gain, double sr, int frames,
                                const std::vector<float>& l,
                                const std::vector<float>& r) {
    std::vector<float> li = l, ri = r;
    std::vector<float> lo(static_cast<std::size_t>(frames), 0.0f);
    std::vector<float> ro(static_cast<std::size_t>(frames), 0.0f);
    std::array<const float*, 2> in_ch{li.data(), ri.data()};
    std::array<float*, 2> out_ch{lo.data(), ro.data()};
    // BufferView is non-owning and aliases in_ch/out_ch (which alias li/ri/
    // lo/ro). All outlive the executor.process() call below; if this binding
    // setup ever moves out of this function those locals would dangle.
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

    const std::array nodes = {
        GraphRuntimeNodeSpec{1, GraphRuntimeNodeKind::Processor, 2, 2},
    };
    auto plan = pulp::graph::build_graph_runtime_plan(
        nodes, std::span<const pulp::graph::GraphRuntimeConnectionSpec>{});
    REQUIRE(plan.ok());

    GainState state{gain};
    const std::array bindings = {
        GraphRuntimeNodeBinding{1, gain_binding, &state, true},
    };
    GraphRuntimeSnapshot snapshot;
    REQUIRE(snapshot.reset(std::move(plan.plan), bindings));

    GraphRuntimeQueues<4, 4, 4> queues;
    GraphRuntimeExecutor executor;
    const auto result = executor.process(block, snapshot, queues);
    REQUIRE(result.ok());
    REQUIRE(result.nodes_processed == 1);

    std::vector<float> out;
    out.insert(out.end(), lo.begin(), lo.end());
    out.insert(out.end(), ro.begin(), ro.end());
    return out;
}

}  // namespace

// Scope: this is OUTPUT parity for a gain (a single executor gain binding vs
// SignalGraph's input->gain->output), NOT topology/graph parity — the executor
// graph is one node and routes no inter-node audio. Topological ordering and
// command drain are covered separately by the existing GraphRuntimeExecutor
// tests ("visits snapshot nodes in plan order", "drains commands ..."). What
// this adds is the SignalGraph<->executor output-parity baseline.
TEST_CASE("GraphRuntimeExecutor gain binding output-parity vs SignalGraph in->gain->out",
          "[host][graph][executor][parity]") {
    constexpr double kSr = 48000.0;
    for (int frames : {1, 64, 256, 1024}) {
        for (float gain : {1.0f, 0.5f, 0.25f, 2.0f, 0.0f}) {
            CAPTURE(frames);
            CAPTURE(gain);
            const auto l = test_signal(frames, 0.8f);
            const auto r = test_signal(frames, 0.6f);

            const auto sg = run_signal_graph(gain, kSr, frames, l, r);
            const auto ex = run_executor(gain, kSr, frames, l, r);

            REQUIRE(sg.size() == ex.size());
            for (std::size_t i = 0; i < sg.size(); ++i) {
                // Bit-exact: both paths are out = in * gain, same float multiply.
                REQUIRE(sg[i] == ex[i]);
            }
        }
    }
}
