#include <pulp/host/signal_graph_executor_routing.hpp>

#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pulp::host {
namespace {

namespace fmt = pulp::format;
namespace gr = pulp::graph;

// Gain binding: out = in * *gain, reading the LIVE compiled snapshot's atomic so
// set_node_gain() is honored without a re-prepare. Mirrors SignalGraph's Gain
// node exactly (same scalar multiply, same min(in,out) channel count).
bool gain_binding(fmt::ProcessBlock&,
                  const fmt::GraphRuntimeNodeProcessContext& ctx,
                  void* user_data) noexcept {
    const auto* atomic = static_cast<const std::atomic<float>*>(user_data);
    const float g = atomic ? atomic->load(std::memory_order_relaxed) : 1.0f;
    const auto& in = ctx.node_inputs;
    auto out = ctx.node_outputs;
    const std::size_t chs = std::min(in.num_channels(), out.num_channels());
    const std::size_t frames = out.num_samples();
    for (std::size_t c = 0; c < chs; ++c) {
        const float* ip = in.channel_ptr(c);
        float* op = out.channel_ptr(c);
        for (std::size_t i = 0; i < frames; ++i) op[i] = ip[i] * g;
    }
    return true;
}

gr::GraphRuntimeNodeKind map_kind(NodeType type) noexcept {
    switch (type) {
        case NodeType::AudioInput: return gr::GraphRuntimeNodeKind::AudioInput;
        case NodeType::AudioOutput: return gr::GraphRuntimeNodeKind::AudioOutput;
        default: return gr::GraphRuntimeNodeKind::Processor;  // Gain
    }
}

bool node_type_eligible(NodeType type) noexcept {
    return type == NodeType::AudioInput || type == NodeType::AudioOutput ||
           type == NodeType::Gain;
}

bool connection_eligible(const Connection& c) noexcept {
    // Plain audio only; feedback is fine (the executor handles one-block
    // feedback). MIDI / automation / audio-rate-mod / sidechain keep the
    // legacy walk.
    return !c.midi && !c.automation && !c.audio_rate_modulation && !c.sidechain;
}

} // namespace

bool signal_graph_executor_eligible(const SignalGraph& graph) {
    if (!graph.is_prepared()) return false;
    for (const auto& node : graph.nodes()) {
        if (!node_type_eligible(node.type)) return false;
    }
    for (const auto& c : graph.connections()) {
        if (!connection_eligible(c)) return false;
    }
    return true;
}

bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out) {
    out = SignalGraphExecutorRouting{};
    if (!signal_graph_executor_eligible(graph)) return false;

    const auto& nodes = graph.nodes();
    const auto& connections = graph.connections();

    // Node specs in nodes() order; the plan resolves connections by NodeId, so
    // bindings just need to match plan.nodes order (== spec order).
    std::vector<gr::GraphRuntimeNodeSpec> node_specs;
    node_specs.reserve(nodes.size());
    for (const auto& node : nodes) {
        node_specs.push_back(gr::GraphRuntimeNodeSpec{
            node.id,
            map_kind(node.type),
            static_cast<std::uint32_t>(std::max(0, node.num_input_ports)),
            static_cast<std::uint32_t>(std::max(0, node.num_output_ports)),
        });
    }

    // Connection specs in connections() order, so a dest port's inbound edges
    // are summed in the SAME order SignalGraph accumulates them (float add is
    // non-associative). Feedback flag carried through.
    std::vector<gr::GraphRuntimeConnectionSpec> conn_specs;
    conn_specs.reserve(connections.size());
    for (const auto& c : connections) {
        conn_specs.push_back(gr::GraphRuntimeConnectionSpec{
            c.source_node, c.source_port, c.dest_node, c.dest_port,
            c.feedback, /*event=*/false,
        });
    }

    auto plan = gr::build_graph_runtime_plan(node_specs, conn_specs);
    if (!plan.ok()) return false;

    // Bindings in plan.nodes order (build_graph_runtime_plan preserves spec
    // order). AudioInput/AudioOutput are executor-handled (null binding);
    // Gain binds the live gain atomic.
    std::vector<fmt::GraphRuntimeNodeBinding> bindings;
    bindings.reserve(plan.plan.nodes.size());
    for (const auto& pnode : plan.plan.nodes) {
        const NodeId id = pnode.id;
        // Find the source node (small N; nodes() is the authoritative list).
        const GraphNode* src = nullptr;
        for (const auto& n : nodes) {
            if (n.id == id) { src = &n; break; }
        }
        if (src == nullptr) return false;
        if (src->type == NodeType::Gain) {
            std::atomic<float>* atomic = graph.live_gain_atomic(id);
            // The contract is "bit-identical or return false": a Gain node with
            // no live atomic would silently fall back to unity gain and diverge.
            if (atomic == nullptr) return false;
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, gain_binding, atomic, /*required=*/true});
        } else {  // AudioInput / AudioOutput — executor kind dispatch
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, nullptr, nullptr, /*required=*/false});
        }
    }

    if (!out.snapshot.reset(std::move(plan.plan), bindings)) return false;

    const int max_block = graph.prepared_max_block_size();
    if (max_block <= 0) return false;
    if (!out.pool.reset(out.snapshot.buffer_slot_count(),
                        static_cast<std::uint32_t>(max_block))) {
        return false;
    }

    out.snapshot_keepalive = graph.live_snapshot_handle();
    out.valid = true;
    return true;
}

} // namespace pulp::host
