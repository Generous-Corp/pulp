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

    // Use the executor's per-node MIDI buffers when the routed call supplies
    // them (a MIDI graph): node_midi_in carries the events gathered from this
    // plugin's inbound MIDI edges, and node_midi_out is the buffer downstream
    // nodes gather from. Fall back to the empty shared scratch for audio-only
    // graphs (no MIDI routing). Clear the output fully before the plugin fills
    // it, matching SignalGraph's per-node clear.
    midi::MidiBuffer& midi_in =
        ctx.node_midi_in != nullptr ? *ctx.node_midi_in : pctx->scratch->midi_in;
    midi::MidiBuffer& midi_out =
        ctx.node_midi_out != nullptr ? *ctx.node_midi_out : pctx->scratch->midi_out;
    midi_out.clear();
    midi_out.clear_sysex();
    if (auto* ump = midi_out.ump()) ump->clear();
    // Use the executor's per-node parameter-automation events when supplied (an
    // automation graph); the empty shared scratch queue otherwise.
    state::ParameterEventQueue& param_events =
        ctx.node_param_events != nullptr ? *ctx.node_param_events
                                         : pctx->scratch->param_events;
    pctx->slot->process(buffers, midi_in, midi_out, param_events, num_samples);
    return true;
}

gr::GraphRuntimeNodeKind map_kind(NodeType type) noexcept {
    switch (type) {
        case NodeType::AudioInput: return gr::GraphRuntimeNodeKind::AudioInput;
        case NodeType::AudioOutput: return gr::GraphRuntimeNodeKind::AudioOutput;
        case NodeType::MidiInput: return gr::GraphRuntimeNodeKind::MidiInput;
        case NodeType::MidiOutput: return gr::GraphRuntimeNodeKind::MidiOutput;
        default: return gr::GraphRuntimeNodeKind::Processor;  // Gain / Plugin
    }
}

bool node_eligible(const GraphNode& node) noexcept {
    switch (node.type) {
        case NodeType::AudioInput:
        case NodeType::AudioOutput:
        case NodeType::Gain:
        // MidiInput / MidiOutput carry no audio and run as no-op nodes on the
        // routed path; the host bridges their mailboxes around process_routed,
        // and the executor's MIDI gather routes events between them and plugins.
        case NodeType::MidiInput:
        case NodeType::MidiOutput:
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

bool connection_eligible(const Connection& /*c*/) noexcept {
    // Every connection kind is now routed: plain audio (feedforward, feedback,
    // sidechain) with PDC; MIDI event edges; sparse parameter automation
    // (two-point) and dense audio-rate modulation (per-sample) — both built into
    // the plugin's parameter-event queue by the executor's automation gather.
    return true;
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
    // The executor's automation gather accumulates distinct automated parameters
    // per node in fixed on-stack arrays (kMaxParamsPerNode) — one for sparse
    // two-point automation, one for dense audio-rate. A node automating more than
    // that on EITHER axis would silently drop the overflow, so keep such a graph
    // on the legacy walk (fail closed). Count sparse and dense distinct params
    // per node separately.
    const auto over_cap = [&](bool dense) {
        std::vector<std::pair<NodeId, std::vector<std::uint32_t>>> per_dest;
        for (const auto& c : connections) {
            const bool is_dense = c.audio_rate_modulation;
            const bool is_sparse = c.automation && !c.audio_rate_modulation;
            if (dense ? !is_dense : !is_sparse) continue;
            auto it = std::find_if(per_dest.begin(), per_dest.end(),
                                   [&](const auto& e) { return e.first == c.dest_node; });
            if (it == per_dest.end()) {
                per_dest.push_back({c.dest_node, {c.automation_param_id}});
                continue;
            }
            auto& ids = it->second;
            if (std::find(ids.begin(), ids.end(), c.automation_param_id) == ids.end()) {
                ids.push_back(c.automation_param_id);
                if (ids.size() > fmt::GraphRuntimeAutomationScratch::kMaxParamsPerNode) {
                    return true;
                }
            }
        }
        return false;
    };
    if (over_cap(/*dense=*/false) || over_cap(/*dense=*/true)) return false;
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
                             fmt::GraphRuntimeSnapshot& out,
                             bool parallel_safe) {
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

    // Whether this graph carries any MIDI. Only then do nodes declare event
    // ports (one in + one out, port 0 — connect_midi routes the whole per-node
    // MIDI stream on port 0), so an audio-only graph's plan is byte-identical to
    // before MIDI support and never inflates the total-port budget.
    bool has_midi = false;
    for (const auto& node : nodes) {
        if (node.type == NodeType::MidiInput || node.type == NodeType::MidiOutput) {
            has_midi = true;
            break;
        }
    }
    if (!has_midi) {
        for (const auto& c : connections) {
            if (c.midi) { has_midi = true; break; }
        }
    }

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
        if (has_midi) {
            spec.event_input_ports = 1;
            spec.event_output_ports = 1;
        }
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
    // non-associative). Feedback flag carried through; a MIDI edge becomes an
    // event connection the executor routes via its MIDI gather.
    std::vector<gr::GraphRuntimeConnectionSpec> conn_specs;
    conn_specs.reserve(connections.size());
    for (const auto& c : connections) {
        gr::GraphRuntimeConnectionSpec spec{
            c.source_node, c.source_port, c.dest_node, c.dest_port,
            c.feedback, /*event=*/c.midi,
        };
        // A parameter-automation edge — sparse two-point (`automation`) or dense
        // audio-rate (`audio_rate_modulation`) — becomes an automation connection
        // carrying its mapping. Resolve the destination plugin's parameter bounds
        // OFF the audio thread (so the realtime gather's Add-mix clamp matches the
        // legacy walk's bounds_for_param without a plugin call).
        if (c.automation || c.audio_rate_modulation) {
            spec.is_automation = true;
            spec.automation.param_id = c.automation_param_id;
            spec.automation.range_lo = c.automation_range_lo;
            spec.automation.range_hi = c.automation_range_hi;
            spec.automation.smoothing_ms = c.automation_smoothing_ms;
            spec.automation.mix_add = c.automation_mix == AutomationMix::Add;
            spec.automation.audio_rate = c.audio_rate_modulation;
            // Default bounds to the mapped range, then refine to the plugin's
            // declared parameter bounds if resolvable (matching bounds_for_param).
            spec.automation.bounds_lo = c.automation_range_lo;
            spec.automation.bounds_hi = c.automation_range_hi;
            PluginSlot* dest_slot = plugin_for ? plugin_for(c.dest_node) : nullptr;
            if (dest_slot != nullptr) {
                for (const auto& p : dest_slot->parameters()) {
                    if (p.id != c.automation_param_id) continue;
                    spec.automation.bounds_lo = p.min_value;
                    spec.automation.bounds_hi = p.max_value;
                    break;
                }
            }
        }
        conn_specs.push_back(spec);
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
        } else {  // AudioInput / AudioOutput / MidiInput / MidiOutput — null
            // binding: AudioInput/AudioOutput are executor kind dispatch, and
            // MidiInput/MidiOutput run as no-op nodes (the executor's MIDI gather
            // fills a MidiOutput's input; the host pre-fills a MidiInput's output
            // and drains a MidiOutput's input around process_routed).
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, nullptr, nullptr, /*required=*/false});
        }
    }

    return out.reset(std::move(plan.plan), bindings, parallel_safe);
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
