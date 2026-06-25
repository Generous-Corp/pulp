#include <pulp/host/signal_graph_executor_routing.hpp>

#include <pulp/host/signal_graph.hpp>

#include <pulp/format/process_block.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace pulp::host {
namespace {

namespace fmt = pulp::format;
namespace gr = pulp::graph;

// Gain binding: out = in * *gain, reading the live compiled snapshot's atomic so
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

// Plugin binding: invoke the node's PluginSlot exactly as SignalGraph's Plugin
// node does — a single Main input/output bus built over the node's gathered
// inputs and assigned outputs, with empty MIDI-in and an empty parameter-event
// queue (the eligible subset has no MIDI / automation / sidechain edges), and a
// scratch MIDI-out that is cleared and discarded. The node's output slots are
// pinned persistent (see build_executor_snapshot), so a plugin that does not
// fully overwrite its output carries the same stale tail across blocks that
// SignalGraph's persistent per-node buffer does. The ProcessBuffers and bus
// views are stack-built each call (string_view names, no allocation), matching
// SignalGraph; both paths reach the slot through PluginSlot::process's default
// main-in/main-out projection, so the audio output is bit-identical.
bool plugin_binding(fmt::ProcessBlock&,
                    const fmt::GraphRuntimeNodeProcessContext& ctx,
                    void* user_data) noexcept {
    auto* pctx = static_cast<PluginBindingContext*>(user_data);
    if (pctx == nullptr || pctx->slot == nullptr || pctx->scratch == nullptr) {
        return false;  // eligibility guarantees a live slot; a null here is a bug
    }
    const auto in_c = ctx.node_inputs;
    auto out_view = ctx.node_outputs;
    const int num_samples = static_cast<int>(out_view.num_samples());

    std::array<fmt::ProcessBusBufferView<const float>, 1> input_buses{{
        {
            .info = {
                .name = "Plugin Node In",
                .index = 0,
                .direction = fmt::BusDirection::Input,
                .role = fmt::BusRole::Main,
                .declared_channels = static_cast<int>(in_c.num_channels()),
                .optional = in_c.num_channels() == 0,
                .active = in_c.num_channels() > 0,
            },
            .buffer = in_c,
        },
    }};
    std::array<fmt::ProcessBusBufferView<float>, 1> output_buses{{
        {
            .info = {
                .name = "Plugin Node Out",
                .index = 0,
                .direction = fmt::BusDirection::Output,
                .role = fmt::BusRole::Main,
                .declared_channels = static_cast<int>(out_view.num_channels()),
                .optional = false,
                .active = out_view.num_channels() > 0,
            },
            .buffer = out_view,
        },
    }};
    fmt::ProcessBuffers buffers{
        fmt::ProcessBusBufferSet<const float>{std::span(input_buses)},
        fmt::ProcessBusBufferSet<float>{std::span(output_buses)},
    };

    pctx->scratch->midi_out.clear();
    pctx->slot->process(buffers, pctx->scratch->midi_in, pctx->scratch->midi_out,
                        pctx->scratch->param_events, num_samples);
    return true;
}

gr::GraphRuntimeNodeKind map_kind(NodeType type) noexcept {
    switch (type) {
        case NodeType::AudioInput: return gr::GraphRuntimeNodeKind::AudioInput;
        case NodeType::AudioOutput: return gr::GraphRuntimeNodeKind::AudioOutput;
        default: return gr::GraphRuntimeNodeKind::Processor;  // Gain / Plugin
    }
}

bool node_eligible(const GraphNode& node) noexcept {
    switch (node.type) {
        case NodeType::AudioInput:
        case NodeType::AudioOutput:
        case NodeType::Gain:
            return true;
        case NodeType::Plugin:
            // A Plugin node only routes bit-exactly when its slot is live (the
            // binding invokes it; a missing/placeholder slot would take
            // SignalGraph's pass-through-or-zero branch instead). A reported
            // latency is fine: the executor's gather applies the same
            // per-connection delay compensation as the legacy walk, derived from
            // the node's latency_samples carried into the plan.
            return node.plugin != nullptr;
        default:
            return false;
    }
}

bool connection_eligible(const Connection& c) noexcept {
    // Plain audio only; feedback is fine (the executor handles one-block
    // feedback). MIDI / automation / audio-rate-mod / sidechain keep the
    // legacy walk.
    return !c.midi && !c.automation && !c.audio_rate_modulation && !c.sidechain;
}

} // namespace

bool signal_graph_topology_executor_eligible(std::span<const GraphNode> nodes,
                                             std::span<const Connection> connections) {
    for (const auto& node : nodes) {
        if (!node_eligible(node)) return false;
    }
    for (const auto& c : connections) {
        if (!connection_eligible(c)) return false;
    }
    return true;
}

bool signal_graph_executor_eligible(const SignalGraph& graph) {
    if (!graph.is_prepared()) return false;
    return signal_graph_topology_executor_eligible(graph.nodes(), graph.connections());
}

bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const std::function<std::atomic<float>*(NodeId)>& gain_for,
                             const std::function<PluginSlot*(NodeId)>& plugin_for,
                             std::vector<PluginBindingContext>& plugin_ctx,
                             PluginRoutingScratch& scratch,
                             fmt::GraphRuntimeSnapshot& out) {
    out.clear();
    plugin_ctx.clear();
    if (!signal_graph_topology_executor_eligible(nodes, connections)) return false;

    // Reserve plugin contexts to the exact count so push_back never reallocates;
    // the bindings' user_data points at &plugin_ctx[k] and must stay valid.
    std::size_t plugin_count = 0;
    for (const auto& node : nodes) {
        if (node.type == NodeType::Plugin) ++plugin_count;
    }
    plugin_ctx.reserve(plugin_count);

    // Node specs in nodes() order; the plan resolves connections by NodeId, so
    // bindings just need to match plan.nodes order (== spec order). Plugin output
    // regions are pinned persistent so a non-full-writing plugin keeps its stale
    // tail across blocks, matching SignalGraph's persistent per-node buffer.
    std::vector<gr::GraphRuntimeNodeSpec> node_specs;
    node_specs.reserve(nodes.size());
    for (const auto& node : nodes) {
        gr::GraphRuntimeNodeSpec spec{
            node.id,
            map_kind(node.type),
            static_cast<std::uint32_t>(std::max(0, node.num_input_ports)),
            static_cast<std::uint32_t>(std::max(0, node.num_output_ports)),
        };
        spec.persistent_output = node.type == NodeType::Plugin;
        // Carry the node's reported latency so the buffer assignment can derive
        // per-connection delay compensation. Resolve it from the SAME slot the
        // legacy walk's latency pass uses (plugin_for == the compiled snapshot's
        // plugins) so the propagated delays match exactly.
        if (node.type == NodeType::Plugin) {
            PluginSlot* slot = plugin_for ? plugin_for(node.id) : nullptr;
            if (slot != nullptr) {
                spec.latency_samples =
                    static_cast<std::uint32_t>(std::max(0, slot->latency_samples()));
            }
        }
        node_specs.push_back(spec);
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
    // order). AudioInput/AudioOutput are executor-handled (null binding); Gain
    // binds its live atomic; Plugin binds a context referencing its live slot
    // and the shared scratch.
    std::vector<fmt::GraphRuntimeNodeBinding> bindings;
    bindings.reserve(plan.plan.nodes.size());
    for (const auto& pnode : plan.plan.nodes) {
        const NodeId id = pnode.id;
        const GraphNode* src = nullptr;
        for (const auto& n : nodes) {
            if (n.id == id) { src = &n; break; }
        }
        if (src == nullptr) return false;
        if (src->type == NodeType::Gain) {
            std::atomic<float>* atomic = gain_for ? gain_for(id) : nullptr;
            // Contract is "bit-identical or fail": a Gain with no live atomic
            // would silently fall back to unity gain and diverge.
            if (atomic == nullptr) return false;
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, gain_binding, atomic, /*required=*/true});
        } else if (src->type == NodeType::Plugin) {
            PluginSlot* slot = plugin_for ? plugin_for(id) : nullptr;
            // Eligibility required a live slot; a missing one here means the
            // resolver and the topology disagree — fail closed to the walk.
            if (slot == nullptr) return false;
            plugin_ctx.push_back(PluginBindingContext{slot, &scratch});
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, plugin_binding, &plugin_ctx.back(), /*required=*/true});
        } else {  // AudioInput / AudioOutput — executor kind dispatch
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, nullptr, nullptr, /*required=*/false});
        }
    }

    return out.reset(std::move(plan.plan), bindings);
}

bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out) {
    // Reset the consumable state directly — SignalGraphExecutorRouting is not
    // assignable (its MIDI/param scratch is move-only). build_executor_snapshot
    // clears `snapshot` and `plugin_ctx`; here we drop the keepalive and mark
    // invalid so a failure can't leave a stale-but-valid routing behind.
    out.valid = false;
    out.snapshot_keepalive.reset();
    if (!graph.is_prepared()) return false;

    if (!build_executor_snapshot(
            graph.nodes(), graph.connections(),
            [&graph](NodeId id) { return graph.live_gain_atomic(id); },
            [&graph](NodeId id) { return graph.live_plugin_slot(id); },
            out.plugin_ctx, out.plugin_scratch, out.snapshot)) {
        return false;
    }

    const int max_block = graph.prepared_max_block_size();
    if (max_block <= 0) return false;
    if (!out.pool.reset(out.snapshot.buffer_slot_count(),
                        static_cast<std::uint32_t>(max_block),
                        out.snapshot.buffer_assignment().connection_delay_samples)) {
        return false;
    }

    out.snapshot_keepalive = graph.live_snapshot_handle();
    out.valid = true;
    return true;
}

} // namespace pulp::host
