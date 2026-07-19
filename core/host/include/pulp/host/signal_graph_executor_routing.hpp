#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/graph_types.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace pulp::host {

class SignalGraph;
struct GraphNode;
struct Connection;

// The single, authoritative classification of a host Connection into a runtime
// lane. EVERY surface that needs to know whether a Connection is audio / MIDI /
// automation — the executor-routing gather, the legacy reference-walk edge
// bucketer, and the latency/PDC passes — routes through classify() so the three
// can never drift apart. `kind` is the lane discriminator carried onto the
// runtime connection structs; `feedback` is the orthogonal back-edge flag;
// `audio_rate` distinguishes a dense audio-rate modulation edge from a sparse
// two-point automation edge (both have kind == Automation). A sidechain edge is
// deliberately NOT special-cased: it arrives as a plain-audio Connection with
// the destination's sidechain input port already resolved, so it classifies as
// Audio.
struct ConnectionClass {
    graph::GraphRuntimeConnectionKind kind = graph::GraphRuntimeConnectionKind::Audio;
    bool feedback = false;
    bool audio_rate = false;
};

ConnectionClass classify(const Connection& c);

// True iff this connection carries latency-aligned audio that participates in
// plug-in delay compensation: a plain feedforward audio edge or a dense
// audio-rate modulation edge (its source is sampled per block-position and
// time-aligned like audio). Feedback back-edges, MIDI edges, and sparse two-
// point automation edges carry no latency-aligned audio. Single-sourced from
// classify() so the latency/PDC passes can't drift from the lane bucketing.
bool connection_affects_latency(const Connection& c);

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
    NodeId node_id = 0;
    PluginSlot* slot = nullptr;
    PluginRoutingScratch* scratch = nullptr;
    std::unique_ptr<PluginRoutingScratch> owned_scratch;
    using AppendParameterEventsFn =
        std::uint64_t (*)(void*, state::ParameterEventQueue&) noexcept;
    void* parameter_events_user_data = nullptr;
    AppendParameterEventsFn append_parameter_events = nullptr;
    std::atomic<std::uint64_t>* parameter_events_sequence_seen = nullptr;
    std::uint64_t parameter_events_pending_sequence = 0;
    // Cached, prepare-stable copy of the node's transport-sensitivity capability
    // (GraphNode::transport_sensitive, resolved once at compile from
    // PluginSlot::wants_transport()). The binding reads THIS, never a live
    // slot->wants_transport() per block, so the routed forwarding and the
    // anticipation partition resolve from the same value and can never disagree.
    bool wants_transport = false;
};

struct ParameterEventInjectionBinding {
    void* user_data = nullptr;
    PluginBindingContext::AppendParameterEventsFn append = nullptr;
    std::atomic<std::uint64_t>* sequence_seen = nullptr;
};

void reset_plugin_parameter_event_sequences(
    std::span<PluginBindingContext> contexts) noexcept;
void commit_plugin_parameter_event_sequences(
    std::span<PluginBindingContext> contexts) noexcept;

// The custom-node process callback. Declared here (identical to the alias in
// signal_graph.hpp) so this header carries no dependency on signal_graph.hpp,
// which itself includes this one — the two are mutually consistent and an
// identical alias redeclaration is well-formed when both are visible.
using CustomNodeProcessFn = std::function<void(audio::BufferView<float>& output,
                                              const audio::BufferView<const float>& input,
                                              int num_samples)>;

// Transport-aware custom callback (identical to the alias in signal_graph.hpp;
// see the note above on the redeclaration being well-formed). Carries the host
// transport for a transport-sensitive custom node.
using CustomNodeTransportProcessFn =
    std::function<void(audio::BufferView<float>& output,
                       const audio::BufferView<const float>& input,
                       int num_samples,
                       const format::ProcessContext& transport)>;

// Per-Custom-node binding context (the binding's user_data). Stored in a
// caller-owned vector reserved to the custom-node count so its elements keep
// stable addresses for the snapshot's lifetime (mirrors PluginBindingContext).
// `process` is a COPY of the resolved CustomNodeProcessFn: for a stateful custom
// node the resolved fn is a lambda that captured the node's instance shared_ptr
// by value, so this copy carries its own keepalive on that instance — exactly as
// a PluginBindingContext's slot keeps its plugin alive through the snapshot. An
// empty `process` means the node was unresolved (unregistered type / shape
// mismatch); the binding then reproduces SignalGraph's pass-through-or-zero.
struct CustomBindingContext {
    CustomNodeProcessFn process;
    // Transport-aware callback for a transport-sensitive custom node. When set
    // (and a transport is available for the block) the binding invokes this
    // instead of `process`; empty == transport-unaware node. A COPY of the
    // resolved callback (its own keepalive on any captured instance), mirroring
    // `process`. Its non-empty state mirrors the node's resolved
    // GraphNode::transport_sensitive, so binding and partition stay consistent.
    CustomNodeTransportProcessFn process_transport;
};

// A SignalGraph translated into the canonical GraphRuntimeExecutor's routing
// form — an immutable snapshot plus a pre-sized scratch pool, ready to drive via
// GraphRuntimeExecutor::process_routed().
//
// Lifetime: built from a PREPARED, eligible graph; valid only until that graph
// re-prepares. The Gain bindings read the live compiled snapshot's gain atomics
// (so set_node_gain() without a re-prepare is honored on the routed path), the
// Plugin bindings invoke that snapshot's live PluginSlots, the Custom bindings
// invoke a COPY of that snapshot's resolved process callbacks (each copy its own
// instance keepalive), and `snapshot_keepalive` pins that snapshot so all stay
// valid. `plugin_ctx` and `custom_ctx` (reserved, stable storage that the
// respective bindings' user_data points into) and `plugin_scratch` (the shared
// MIDI/param scratch) are owned here. Rebuild the routing after any recompile.
struct SignalGraphExecutorRouting {
    format::GraphRuntimeSnapshot snapshot;
    format::GraphRuntimeBufferPool pool;
    std::vector<PluginBindingContext> plugin_ctx;  // stable user_data storage
    std::vector<CustomBindingContext> custom_ctx;  // stable user_data storage
    PluginRoutingScratch plugin_scratch;
    std::shared_ptr<const void> snapshot_keepalive;  // pins gain-atomic + slot lifetime
    bool valid = false;
};

// True iff `nodes`/`connections` are in the subset the executor reproduces
// bit-exactly:
//   - nodes only AudioInput / AudioOutput / Gain / Plugin / MidiInput /
//     MidiOutput / Custom. Gain is the fixed 2-in/2-out built-in utility and
//     always fully writes. A Plugin node with a live slot invokes it; its output
//     region is pinned persistent so a plugin that does not fully write matches
//     SignalGraph's persistent per-node buffer, and a reported latency is fine
//     because its delay compensation is replicated on the routed path. A Plugin
//     node with no live slot (unresolved/placeholder) routes as
//     pass-through-or-zero, exactly as SignalGraph's walk does for a slot-less
//     plugin node. A Custom node is audio-only (no MIDI/automation/latency);
//     its binding invokes the resolved process callback, or — for an unresolved
//     type / shape mismatch — pass-through-or-zero, so it matches the walk either
//     way;
//   - connections may be audio (feedforward, feedback, or sidechain — a sidechain
//     edge routes as plain audio into a higher input port of the destination
//     plugin), MIDI event edges, sparse automation, or dense audio-rate
//     modulation. Any per-node automated-parameter count routes — the gather's
//     per-node accumulators are sized off-RT to the actual count (no fixed cap).
// No prepared check.
bool signal_graph_topology_executor_eligible(std::span<const GraphNode> nodes,
                                              std::span<const Connection> connections);

enum class ExecutorTopologyValidationCode : std::uint8_t {
    Accepted,
    TopologyIneligible,
    NodeLimitExceeded,
    ConnectionLimitExceeded,
    PerNodePortLimitExceeded,
    TotalPortLimitExceeded,
    PlanRejected,
};

struct ExecutorTopologyValidation {
    ExecutorTopologyValidationCode code = ExecutorTopologyValidationCode::Accepted;
    std::uint64_t actual = 0;
    std::uint64_t limit = 0;
    std::size_t index = 0;
    NodeId node = 0;
    constexpr explicit operator bool() const noexcept {
        return code == ExecutorTopologyValidationCode::Accepted;
    }
};

ExecutorTopologyValidation validate_signal_graph_executor_topology(
    std::span<const GraphNode> nodes, std::span<const Connection> connections,
    graph::GraphRuntimeLimits limits = {});

// As above, plus requiring the graph be prepared (a live snapshot exists).
bool signal_graph_executor_eligible(const SignalGraph& graph);

// The per-node value resolvers build_executor_snapshot binds a plan against.
// One named field per resolver, so two same-shaped resolvers cannot be swapped
// by argument position and a caller that needs only some of them leaves the rest
// default-empty instead of counting placeholder arguments.
//
// Every field is optional: an empty resolver means "this caller supplies no such
// value", and the build falls back exactly as documented on
// build_executor_snapshot below.
struct ExecutorSnapshotBinders {
    // A Gain node's live atomic. Empty (or a null result) fails the build for a
    // graph that has Gain nodes.
    std::function<std::atomic<float>*(NodeId)> gain_for;
    // A Plugin node's live slot. A null result still routes: the binding
    // reproduces SignalGraph's pass-through-or-zero, matching the walk.
    std::function<PluginSlot*(NodeId)> plugin_for;
    // A per-node persistent CPU-load measurer the executor wraps each node's work
    // in (node_loads() telemetry parity with the legacy walk). Null result = that
    // node is not measured; empty = no node is measured (e.g. a baked Processor
    // that exposes no per-node loads).
    std::function<audio::AudioProcessLoadMeasurer*(NodeId)> load_for;
    // A Custom node's live process callback (a COPY is stored in the binding
    // context as its own keepalive). A null result (unregistered type / shape
    // mismatch) still routes as pass-through-or-zero, matching the walk.
    std::function<const CustomNodeProcessFn*(NodeId)> custom_for;
    // A Custom node's transport-aware callback (null = none). Populated into the
    // binding context alongside `custom_for`; a node with a non-null result is
    // transport-sensitive (consistent with GraphNode::transport_sensitive
    // resolved at compile).
    std::function<const CustomNodeTransportProcessFn*(NodeId)> custom_transport_for;
    // Cached plugin-metadata accessors so a swap-time build makes NO live
    // PluginSlot metadata call. Empty → fall back to `plugin_for`'s live slot
    // (baked / anticipation callers, which are not on the swap path).
    std::function<int(NodeId)> plugin_latency_for;
    std::function<const std::vector<HostParamInfo>*(NodeId)> plugin_params_for;
    // Optional per-Plugin-node ingress binding for the control-to-audio
    // parameter-event mailbox. Empty means this snapshot has no injected
    // parameter events (for example baked/external routing callers).
    std::function<ParameterEventInjectionBinding(NodeId)> parameter_events_for;
};

// Build the executor snapshot (plan + bindings) for an eligible topology,
// resolving each node's live values through `binders`. Plugin binding contexts
// are appended to `plugin_ctx` (cleared first, then reserved to the plugin count
// so the bindings' user_data stays valid) and reference `scratch`. Shared by the
// live-graph translation and by SignalGraph::compile_ (which embeds the snapshot
// in the compiled graph, resolving from its own runtime/plugins). Returns false
// (and leaves `out` empty) if the topology is not eligible, a Gain atomic is
// missing, or the canonical plan/snapshot cannot be built under its limits. Does
// NOT size a pool or set a keepalive — the caller owns those.
// `parallel_safe` builds the snapshot's buffer assignment without slot reuse so
// it can drive GraphRuntimeExecutor::process_parallel (concurrent same-level
// nodes never alias a recycled slot); default false = the compact serial layout.
// `custom_ctx`, when non-null, is the caller-owned storage for Custom-node
// binding contexts (cleared, then reserved to the custom-node count so the
// bindings' user_data stays valid). When it is null and the topology contains a
// Custom node, the build fails closed (returns false) so the caller falls back to
// the legacy walk — callers whose subset never contains Custom nodes (e.g. a
// baked Processor or the anticipation interior) may omit it.
bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const ExecutorSnapshotBinders& binders,
                             std::vector<PluginBindingContext>& plugin_ctx,
                             PluginRoutingScratch& scratch,
                             format::GraphRuntimeSnapshot& out,
                             bool parallel_safe = false,
                             std::vector<CustomBindingContext>* custom_ctx = nullptr);

// Positional-resolver forwarder onto the binder-struct overload above, kept for
// call sites that have not migrated yet. Each argument maps to the
// same-named ExecutorSnapshotBinders field and the semantics are identical.
// Prefer the ExecutorSnapshotBinders overload for new code: two same-shaped
// resolvers cannot be silently swapped there.
bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const std::function<std::atomic<float>*(NodeId)>& gain_for,
                             const std::function<PluginSlot*(NodeId)>& plugin_for,
                             std::vector<PluginBindingContext>& plugin_ctx,
                             PluginRoutingScratch& scratch,
                             format::GraphRuntimeSnapshot& out,
                             bool parallel_safe = false,
                             const std::function<audio::AudioProcessLoadMeasurer*(NodeId)>&
                                 load_for = {},
                             std::vector<CustomBindingContext>* custom_ctx = nullptr,
                             const std::function<const CustomNodeProcessFn*(NodeId)>&
                                 custom_for = {},
                             // Resolves a Custom node's transport-aware callback
                             // (null = none). Populated into the binding context
                             // alongside `custom_for`; a node with a non-null
                             // result is transport-sensitive (consistent with
                             // GraphNode::transport_sensitive resolved at compile).
                             const std::function<const CustomNodeTransportProcessFn*(NodeId)>&
                                 custom_transport_for = {},
                             // 2.2b (H2): cached plugin-metadata accessors so a
                             // swap-time build makes NO live PluginSlot metadata
                             // call. Empty → fall back to plugin_for's live slot
                             // (baked / anticipation callers, not on the swap path).
                             const std::function<int(NodeId)>& plugin_latency_for = {},
                             const std::function<const std::vector<HostParamInfo>*(NodeId)>&
                                 plugin_params_for = {},
                             const std::function<ParameterEventInjectionBinding(NodeId)>&
                                 parameter_events_for = {});

// Translate a prepared, eligible `graph` into `out` (snapshot + sized pool +
// keepalive). Returns false (out.valid == false) when ineligible/unprepared. On
// success `out` drives process_routed() with output bit-identical to
// SignalGraph::process()'s own walk for the eligible subset.
bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out);

} // namespace pulp::host
