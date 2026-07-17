#include <pulp/host/signal_graph_executor_routing.hpp>

#include <pulp/host/signal_graph.hpp>

#include <pulp/format/process_block.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace pulp::host {

// Single classification surface for a host Connection's runtime lane. The lane
// precedence — MIDI before automation before plain audio — mirrors the legacy
// reference walk's bucketing: a MIDI edge is an Event regardless of any other
// flag; otherwise either automation form (sparse `automation` or dense
// `audio_rate_modulation`) is Automation; everything else (including sidechain
// and feedback back-edges) is plain Audio. `feedback` is carried through
// orthogonally, and `audio_rate` records the dense-vs-sparse automation form so
// callers need not re-read the Connection.
ConnectionClass classify(const Connection& c) {
    ConnectionClass cls;
    cls.kind = c.midi ? graph::GraphRuntimeConnectionKind::Event
               : (c.automation || c.audio_rate_modulation)
                   ? graph::GraphRuntimeConnectionKind::Automation
                   : graph::GraphRuntimeConnectionKind::Audio;
    cls.feedback = c.feedback;
    cls.audio_rate = c.audio_rate_modulation;
    return cls;
}

bool connection_affects_latency(const Connection& c) {
    const ConnectionClass cls = classify(c);
    if (cls.feedback) return false;
    return cls.kind == graph::GraphRuntimeConnectionKind::Audio ||
           (cls.kind == graph::GraphRuntimeConnectionKind::Automation && cls.audio_rate);
}

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
// inputs and assigned outputs, with MIDI and parameter-event buffers supplied by
// the routed executor when the graph carries those edges (or the binding's
// fallback scratch for audio-only graphs). The node's output slots are pinned
// persistent (see build_executor_snapshot), so a plugin that does not fully
// overwrite its output carries the same stale tail across blocks that
// SignalGraph's persistent per-node buffer does. The ProcessBuffers and bus views
// are stack-built each call (string_view names, no allocation), matching
// SignalGraph; both paths reach the slot through PluginSlot::process's default
// main-in/main-out projection, so the audio output is bit-identical.
bool plugin_binding(fmt::ProcessBlock& block,
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
    // nodes gather from. Fall back to the binding scratch for audio-only graphs
    // (shared on serial snapshots, per-plugin on parallel snapshots). Clear the
    // output fully before the plugin fills it, matching SignalGraph's per-node
    // clear.
    midi::MidiBuffer& midi_in =
        ctx.node_midi_in != nullptr ? *ctx.node_midi_in : pctx->scratch->midi_in;
    midi::MidiBuffer& midi_out =
        ctx.node_midi_out != nullptr ? *ctx.node_midi_out : pctx->scratch->midi_out;
    midi_out.clear();
    midi_out.clear_sysex();
    if (auto* ump = midi_out.ump()) ump->clear();
    // Use the executor's per-node parameter-automation events when supplied (an
    // automation graph); the binding fallback queue otherwise.
    state::ParameterEventQueue& param_events =
        ctx.node_param_events != nullptr ? *ctx.node_param_events
                                         : pctx->scratch->param_events;
    if (ctx.node_param_events == nullptr) {
        // Audio-only and anticipation snapshots use the binding-owned fallback
        // queue. It persists across blocks, so clear it before one-shot mailbox
        // delivery just as the executor clears its per-node queue in gather.
        param_events.clear();
    }
    pctx->parameter_events_pending_sequence = 0;
    if (pctx->append_parameter_events != nullptr) {
        pctx->parameter_events_pending_sequence = pctx->append_parameter_events(
            pctx->parameter_events_user_data, param_events);
        param_events.sort();
    }
    // A transport-sensitive plugin (wants_transport cached once at compile from
    // PluginSlot::wants_transport(), never re-polled here) gets the live host
    // transport when the block carries one; otherwise — and for every
    // transport-unaware plugin — the transport-less overload runs, byte-for-byte
    // as before. block.transport is the routed ProcessBlock's transport pointer,
    // populated by SignalGraph::process_impl from the host playhead.
    if (pctx->wants_transport && block.transport != nullptr) {
        pctx->slot->process(buffers, midi_in, midi_out, param_events, num_samples,
                            *block.transport);
    } else {
        pctx->slot->process(buffers, midi_in, midi_out, param_events, num_samples);
    }
    return true;
}

// Custom binding: invoke the node's resolved process callback exactly as
// SignalGraph's Custom node does — over the node's mono input/output slots
// (ctx.node_inputs gathered from upstream, ctx.node_outputs the assigned
// scratch). Custom nodes are audio-only (no MIDI/automation/latency), so unlike
// plugin_binding there is no bus/MIDI/parameter marshaling. When the context's
// process callback is empty (an unresolved custom type or a shape mismatch — the
// same condition SignalGraph checks with custom_type_matches_node_shape), this
// reproduces SignalGraph's pass_through_or_zero: copy min(in,out) channels
// straight through, then zero any extra output channels, so the routed output is
// bit-identical to the walk either way. RT-safe: the BufferView copies are
// lightweight views (no allocation), and the std::function copy that may
// allocate happened off the audio thread at snapshot-build time.
bool custom_binding(fmt::ProcessBlock& block,
                    const fmt::GraphRuntimeNodeProcessContext& ctx,
                    void* user_data) noexcept {
    const auto* cctx = static_cast<const CustomBindingContext*>(user_data);
    const auto in = ctx.node_inputs;
    auto out = ctx.node_outputs;
    const std::size_t frames = out.num_samples();
    // A transport-sensitive custom node (process_transport set once at compile)
    // gets the live host transport when the block carries one; otherwise the
    // transport-unaware `process` runs, byte-for-byte as before.
    if (cctx != nullptr && cctx->process_transport && block.transport != nullptr) {
        cctx->process_transport(out, in, static_cast<int>(frames), *block.transport);
        return true;
    }
    if (cctx != nullptr && cctx->process) {
        cctx->process(out, in, static_cast<int>(frames));
        return true;
    }
    const std::size_t in_ch = in.num_channels();
    const std::size_t out_ch = out.num_channels();
    const std::size_t chs = std::min(in_ch, out_ch);
    for (std::size_t c = 0; c < chs; ++c) {
        std::memcpy(out.channel_ptr(c), in.channel_ptr(c), sizeof(float) * frames);
    }
    for (std::size_t c = chs; c < out_ch; ++c) {
        std::memset(out.channel_ptr(c), 0, sizeof(float) * frames);
    }
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
            // Always eligible: a live slot routes through plugin_binding, and a
            // null slot (an unresolved/placeholder node — e.g. GraphSerializer
            // rehydration of a missing plugin) routes through the binding's
            // pass-through-or-zero, which is exactly what SignalGraph's walk does
            // for a slot-less Plugin node — so the routed output matches the walk
            // in either case (mirroring the Custom case below). A reported latency
            // is fine: the executor's gather applies the same per-connection delay
            // compensation as the legacy walk, derived from the node's
            // latency_samples carried into the plan.
            return true;
        case NodeType::Custom:
            // Always eligible: a resolved custom node routes through its process
            // callback, and an unresolved one (unregistered type / shape
            // mismatch) routes through the binding's pass-through-or-zero — which
            // is exactly what SignalGraph's walk does for an unresolved custom
            // node — so the routed output matches the walk in either case.
            return true;
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
    // per node into per-node scratch slices sized off-RT to the actual count (one
    // axis for sparse two-point automation, one for dense audio-rate), so there is
    // no per-node parameter cap to fail closed on — any count routes.
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
                             bool parallel_safe,
                             const std::function<audio::AudioProcessLoadMeasurer*(NodeId)>&
                                 load_for,
                             std::vector<CustomBindingContext>* custom_ctx,
                             const std::function<const CustomNodeProcessFn*(NodeId)>&
                                 custom_for,
                             const std::function<const CustomNodeTransportProcessFn*(NodeId)>&
                                 custom_transport_for,
                             const std::function<int(NodeId)>& plugin_latency_for,
                             const std::function<const std::vector<HostParamInfo>*(NodeId)>&
                                 plugin_params_for,
                             const std::function<ParameterEventInjectionBinding(NodeId)>&
                                 parameter_events_for) {
    return build_executor_snapshot(nodes, connections,
                                   ExecutorSnapshotBinders{
                                       .gain_for = gain_for,
                                       .plugin_for = plugin_for,
                                       .load_for = load_for,
                                       .custom_for = custom_for,
                                       .custom_transport_for = custom_transport_for,
                                       .plugin_latency_for = plugin_latency_for,
                                       .plugin_params_for = plugin_params_for,
                                       .parameter_events_for = parameter_events_for,
                                   },
                                   plugin_ctx, scratch, out, parallel_safe,
                                   custom_ctx);
}

bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const ExecutorSnapshotBinders& binders,
                             std::vector<PluginBindingContext>& plugin_ctx,
                             PluginRoutingScratch& scratch,
                             fmt::GraphRuntimeSnapshot& out,
                             bool parallel_safe,
                             std::vector<CustomBindingContext>* custom_ctx) {
    const auto& gain_for = binders.gain_for;
    const auto& plugin_for = binders.plugin_for;
    const auto& load_for = binders.load_for;
    const auto& custom_for = binders.custom_for;
    const auto& custom_transport_for = binders.custom_transport_for;
    const auto& plugin_latency_for = binders.plugin_latency_for;
    const auto& plugin_params_for = binders.plugin_params_for;
    const auto& parameter_events_for = binders.parameter_events_for;

    out.clear();
    plugin_ctx.clear();
    if (custom_ctx != nullptr) custom_ctx->clear();
    if (!signal_graph_topology_executor_eligible(nodes, connections)) return false;

    // Reserve plugin contexts to the exact count so push_back never reallocates;
    // the bindings' user_data points at &plugin_ctx[k] and must stay valid.
    std::size_t plugin_count = 0;
    std::size_t custom_count = 0;
    for (const auto& node : nodes) {
        if (node.type == NodeType::Plugin) ++plugin_count;
        else if (node.type == NodeType::Custom) ++custom_count;
    }
    plugin_ctx.reserve(plugin_count);
    // Same stable-address contract as plugin_ctx: reserve to the custom-node
    // count so &custom_ctx->back() stays valid for every binding's user_data.
    if (custom_ctx != nullptr) custom_ctx->reserve(custom_count);

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
        // Pin a persistent output region for any node that may NOT fully
        // overwrite its output each block, so a partial writer keeps its own
        // stale tail across blocks instead of a recycled slot's data — matching
        // SignalGraph's per-node persistent output buffer. Hosted plugins and
        // Custom callbacks both qualify (neither is contractually required to
        // write every output sample); the walk reuses an uncleared per-node
        // buffer for both.
        spec.persistent_output =
            node.type == NodeType::Plugin || node.type == NodeType::Custom;
        // Carry the node's reported latency so the buffer assignment can derive
        // per-connection delay compensation. Resolve it from the SAME slot the
        // legacy walk's latency pass uses (plugin_for == the compiled snapshot's
        // plugins) so the propagated delays match exactly.
        if (node.type == NodeType::Plugin) {
            PluginSlot* slot = plugin_for ? plugin_for(node.id) : nullptr;
            if (slot != nullptr) {
                // 2.2b (H2): prefer cached latency; latency_samples() is a live
                // plugin call unsafe concurrent with process() during a swap.
                const int lat = plugin_latency_for ? plugin_latency_for(node.id)
                                                    : slot->latency_samples();
                spec.latency_samples = static_cast<std::uint32_t>(std::max(0, lat));
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
        // Single classification surface: the lane (audio / event / automation),
        // the feedback flag, and the dense-vs-sparse automation form all come
        // from classify(), the same helper the legacy reference walk uses, so the
        // two routing surfaces can never disagree about a Connection's lane.
        const ConnectionClass cls = classify(c);
        gr::GraphRuntimeConnectionSpec spec{
            c.source_node, c.source_port, c.dest_node, c.dest_port,
            cls.feedback, cls.kind,
        };
        // A parameter-automation edge — sparse two-point (`automation`) or dense
        // audio-rate (`audio_rate_modulation`) — carries its mapping. Resolve the
        // destination plugin's parameter bounds OFF the audio thread (so the
        // realtime gather's Add-mix clamp matches the legacy walk's
        // bounds_for_param without a plugin call).
        if (cls.kind == gr::GraphRuntimeConnectionKind::Automation) {
            spec.automation.param_id = c.automation_param_id;
            spec.automation.range_lo = c.automation_range_lo;
            spec.automation.range_hi = c.automation_range_hi;
            spec.automation.smoothing_ms = c.automation_smoothing_ms;
            spec.automation.mix_add = c.automation_mix == AutomationMix::Add;
            spec.automation.audio_rate = cls.audio_rate;
            // Default bounds to the mapped range, then refine to the plugin's
            // declared parameter bounds if resolvable (matching bounds_for_param).
            spec.automation.bounds_lo = c.automation_range_lo;
            spec.automation.bounds_hi = c.automation_range_hi;
            PluginSlot* dest_slot = plugin_for ? plugin_for(c.dest_node) : nullptr;
            if (dest_slot != nullptr) {
                // 2.2b (H2): prefer cached params; parameters() is a live plugin
                // call. Cache returns a stable pointer; fall back to a by-value
                // copy of the live list when no cache accessor is provided.
                const std::vector<HostParamInfo>* cached =
                    plugin_params_for ? plugin_params_for(c.dest_node) : nullptr;
                std::vector<HostParamInfo> live;
                if (cached == nullptr) live = dest_slot->parameters();
                const std::vector<HostParamInfo>& params = cached ? *cached : live;
                for (const auto& p : params) {
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
    // binds its live atomic; Plugin binds a context referencing its live slot and
    // fallback scratch. Serial snapshots reuse the shared scratch; parallel
    // snapshots need per-plugin fallback scratch because same-level Plugin nodes
    // may run concurrently and PluginSlot::process mutates MIDI output.
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
            if (slot == nullptr) {
                // A slot-less Plugin node (unresolved/placeholder — e.g. a
                // GraphSerializer rehydration of a missing plugin) routes as
                // pass-through-or-zero, matching SignalGraph's walk. custom_binding
                // with a null user_data is exactly that: copy min(in,out) channels,
                // zero-fill the rest. No plugin_ctx entry is pushed (the binding
                // needs no context), and required=false because the pass-through
                // covers the missing slot. Latency stays 0 (the spec's latency
                // resolve already guarded on a non-null slot); persistent_output is
                // harmless since pass-through fully writes every output channel.
                bindings.push_back(fmt::GraphRuntimeNodeBinding{
                    id, custom_binding, nullptr, /*required=*/false});
            } else {
                PluginBindingContext ctx;
                ctx.node_id = id;
                ctx.slot = slot;
                // Cache the node's transport-sensitivity from the SAME compile-time
                // GraphNode::transport_sensitive the anticipation analysis reads, so
                // the routed forwarding and the partition never disagree. Never
                // re-poll slot->wants_transport() per block on the audio thread.
                ctx.wants_transport = src->transport_sensitive;
                if (parameter_events_for) {
                    const auto injection = parameter_events_for(id);
                    ctx.parameter_events_user_data = injection.user_data;
                    ctx.append_parameter_events = injection.append;
                    ctx.parameter_events_sequence_seen = injection.sequence_seen;
                }
                if (parallel_safe) {
                    try {
                        ctx.owned_scratch = std::make_unique<PluginRoutingScratch>();
                    } catch (...) {
                        return false;
                    }
                    ctx.scratch = ctx.owned_scratch.get();
                } else {
                    ctx.scratch = &scratch;
                }
                plugin_ctx.push_back(std::move(ctx));
                bindings.push_back(fmt::GraphRuntimeNodeBinding{
                    id, plugin_binding, &plugin_ctx.back(), /*required=*/true});
            }
        } else if (src->type == NodeType::Custom) {
            // No storage for the binding context means this caller's subset is
            // not expected to carry Custom nodes — fail closed to the walk rather
            // than route without a stable user_data slot.
            if (custom_ctx == nullptr) return false;
            // Resolve the live process callback; null = unresolved type / shape
            // mismatch, stored as an empty function so the binding pass-through-
            // or-zeros (matching SignalGraph). The stored copy is the binding's
            // own keepalive on any captured instance (mirrors PluginBindingContext
            // holding its slot). required=false: the pass-through covers a null
            // callback, so a missing resolution never disqualifies the snapshot.
            const CustomNodeProcessFn* fn = custom_for ? custom_for(id) : nullptr;
            // Resolve the transport-aware callback from the SAME compile-time
            // resolution that set GraphNode::transport_sensitive (a non-null
            // result <=> transport_sensitive), so binding and partition agree.
            const CustomNodeTransportProcessFn* tfn =
                custom_transport_for ? custom_transport_for(id) : nullptr;
            custom_ctx->push_back(CustomBindingContext{
                fn ? *fn : CustomNodeProcessFn{},
                tfn ? *tfn : CustomNodeTransportProcessFn{}});
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, custom_binding, &custom_ctx->back(), /*required=*/false});
        } else {  // AudioInput / AudioOutput / MidiInput / MidiOutput — null
            // binding: AudioInput/AudioOutput are executor kind dispatch, and
            // MidiInput/MidiOutput run as no-op nodes (the executor's MIDI gather
            // fills a MidiOutput's input; the host pre-fills a MidiInput's output
            // and drains a MidiOutput's input around process_routed).
            bindings.push_back(fmt::GraphRuntimeNodeBinding{
                id, nullptr, nullptr, /*required=*/false});
        }
        // Wire this node's persistent CPU-load measurer (every node kind, incl.
        // audio/MIDI I/O) so routed execution attributes per-node load exactly
        // like the legacy walk. The pointee lives in the host's insert-only
        // node-load map and outlives the snapshot.
        bindings.back().load = load_for ? load_for(id) : nullptr;
    }

    return out.reset(std::move(plan.plan), bindings, parallel_safe);
}

void reset_plugin_parameter_event_sequences(
    std::span<PluginBindingContext> contexts) noexcept {
    for (auto& context : contexts) {
        context.parameter_events_pending_sequence = 0;
    }
}

void commit_plugin_parameter_event_sequences(
    std::span<PluginBindingContext> contexts) noexcept {
    for (auto& context : contexts) {
        if (context.parameter_events_pending_sequence != 0
            && context.parameter_events_sequence_seen != nullptr) {
            context.parameter_events_sequence_seen->store(
                context.parameter_events_pending_sequence,
                std::memory_order_relaxed);
        }
        context.parameter_events_pending_sequence = 0;
    }
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

    const ExecutorSnapshotBinders binders{
        .gain_for = [&graph](NodeId id) { return graph.live_gain_atomic(id); },
        .plugin_for = [&graph](NodeId id) { return graph.live_plugin_slot(id); },
        .custom_for = [&graph](NodeId id) { return graph.live_custom_processor(id); },
        .custom_transport_for =
            [&graph](NodeId id) { return graph.live_custom_transport_processor(id); },
    };
    if (!build_executor_snapshot(graph.nodes(), graph.connections(), binders,
                                 out.plugin_ctx, out.plugin_scratch, out.snapshot,
                                 /*parallel_safe=*/false, &out.custom_ctx)) {
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
