#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/graph_types.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host {

class SignalGraph;
struct GraphNode;
struct Connection;

// Fallback scratch a routed Plugin-node binding hands to PluginSlot::process
// when the routed call does not need per-node MIDI or automation buffers.
// MIDI/automation graphs use the executor-owned per-node scratch exposed through
// GraphRuntimeNodeProcessContext. Serial snapshots may share one fallback
// scratch; parallel snapshots bind each Plugin node to owned fallback scratch
// because PluginSlot::process receives a mutable MIDI output buffer.
struct PluginRoutingScratch {
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    state::ParameterEventQueue param_events;
};

// Per-Plugin-node binding context (the binding's user_data). Stored in a
// caller-owned vector reserved to the plugin-node count so its elements keep
// stable addresses for the snapshot's lifetime. `scratch` points either at the
// serial snapshot's shared fallback scratch or at `owned_scratch` for a parallel
// snapshot. Keep owned scratch heap-indirected so `scratch` survives moving this
// context into `plugin_ctx`.
struct PluginBindingContext {
    PluginSlot* slot = nullptr;
    PluginRoutingScratch* scratch = nullptr;
    std::unique_ptr<PluginRoutingScratch> owned_scratch;
};

// A SignalGraph translated into the canonical GraphRuntimeExecutor's routing
// form — an immutable snapshot plus a pre-sized scratch pool, ready to drive via
// GraphRuntimeExecutor::process_routed().
//
// Lifetime: built from a PREPARED, eligible graph; valid only until that graph
// re-prepares. The Gain bindings read the live compiled snapshot's gain atomics
// (so set_node_gain() without a re-prepare is honored on the routed path), the
// Plugin bindings invoke that snapshot's live PluginSlots, and
// `snapshot_keepalive` pins that snapshot so both stay valid. `plugin_ctx`
// (reserved, stable storage that the Plugin bindings' user_data points into) and
// `plugin_scratch` (the shared MIDI/param scratch) are owned here. Rebuild the
// routing after any recompile.
struct SignalGraphExecutorRouting {
    format::GraphRuntimeSnapshot snapshot;
    format::GraphRuntimeBufferPool pool;
    std::vector<PluginBindingContext> plugin_ctx;  // stable user_data storage
    PluginRoutingScratch plugin_scratch;
    std::shared_ptr<const void> snapshot_keepalive;  // pins gain-atomic + slot lifetime
    bool valid = false;
};

// True iff `nodes`/`connections` are in the subset the executor reproduces
// bit-exactly:
//   - nodes only AudioInput / AudioOutput / Gain / live Plugin / MidiInput /
//     MidiOutput. Gain is the fixed 2-in/2-out built-in utility and always
//     fully writes. Every Plugin node
//     must carry a live slot; its output region is pinned persistent so a plugin
//     that does not fully write matches SignalGraph's persistent per-node buffer;
//     a reported latency is fine because its delay compensation is replicated on
//     the routed path;
//   - connections may be audio (feedforward, feedback, or sidechain — a sidechain
//     edge routes as plain audio into a higher input port of the destination
//     plugin), MIDI event edges, sparse automation, or dense audio-rate
//     modulation. Sparse and dense automation each fail closed when one node
//     exceeds GraphRuntimeAutomationScratch::kMaxParamsPerNode distinct params.
// No prepared check.
bool signal_graph_topology_executor_eligible(std::span<const GraphNode> nodes,
                                              std::span<const Connection> connections);

// As above, plus requiring the graph be prepared (a live snapshot exists).
bool signal_graph_executor_eligible(const SignalGraph& graph);

// Build the executor snapshot (plan + bindings) for an eligible topology,
// resolving each Gain node's live atomic via `gain_for` and each Plugin node's
// live slot via `plugin_for`. Plugin binding contexts are appended to
// `plugin_ctx` (cleared first, then reserved to the plugin count so the bindings'
// user_data stays valid) and reference `scratch`. Shared by the live-graph
// translation and by SignalGraph::compile_ (which embeds the snapshot in the
// compiled graph, resolving from its own runtime/plugins). Returns false (and
// leaves `out` empty) if the topology is not eligible, a Gain atomic or Plugin
// slot is missing, or the canonical plan/snapshot cannot be built under its
// limits. Does NOT size a pool or set a keepalive — the caller owns those.
// `parallel_safe` builds the snapshot's buffer assignment without slot reuse so
// it can drive GraphRuntimeExecutor::process_parallel (concurrent same-level
// nodes never alias a recycled slot); default false = the compact serial layout.
// `load_for`, when provided, resolves a per-node persistent CPU-load measurer
// that the executor wraps each node's work in (for node_loads() telemetry parity
// with the legacy walk). Null result = that node is not measured; an empty
// function = no node is measured (e.g. a baked Processor that exposes no
// per-node loads).
bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const std::function<std::atomic<float>*(NodeId)>& gain_for,
                             const std::function<PluginSlot*(NodeId)>& plugin_for,
                             std::vector<PluginBindingContext>& plugin_ctx,
                             PluginRoutingScratch& scratch,
                             format::GraphRuntimeSnapshot& out,
                             bool parallel_safe = false,
                             const std::function<audio::AudioProcessLoadMeasurer*(NodeId)>&
                                 load_for = {});

// Translate a prepared, eligible `graph` into `out` (snapshot + sized pool +
// keepalive). Returns false (out.valid == false) when ineligible/unprepared. On
// success `out` drives process_routed() with output bit-identical to
// SignalGraph::process()'s own walk for the eligible subset.
bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out);

} // namespace pulp::host
