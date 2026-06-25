#pragma once

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/graph_types.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <span>

namespace pulp::host {

class SignalGraph;
struct GraphNode;
struct Connection;

// A SignalGraph translated into the canonical GraphRuntimeExecutor's routing
// form — an immutable snapshot plus a pre-sized scratch pool, ready to drive via
// GraphRuntimeExecutor::process_routed().
//
// Lifetime: built from a PREPARED, eligible graph; valid only until that graph
// re-prepares. The Gain bindings read the live compiled snapshot's gain atomics
// (so set_node_gain() without a re-prepare is honored on the routed path), and
// `snapshot_keepalive` pins that snapshot so the atomics stay valid. Rebuild the
// routing after any recompile.
struct SignalGraphExecutorRouting {
    format::GraphRuntimeSnapshot snapshot;
    format::GraphRuntimeBufferPool pool;
    std::shared_ptr<const void> snapshot_keepalive;  // pins gain-atomic lifetime
    bool valid = false;
};

// True iff `nodes`/`connections` are in the subset the executor reproduces
// bit-exactly: nodes only AudioInput/AudioOutput/Gain (Gain is fixed 2-in/2-out,
// so it always fully writes its outputs — required for parity under slot reuse);
// connections only plain audio (feedforward or feedback; no MIDI, automation,
// audio-rate-modulation, or sidechain). No prepared check.
bool signal_graph_topology_executor_eligible(std::span<const GraphNode> nodes,
                                              std::span<const Connection> connections);

// As above, plus requiring the graph be prepared (a live snapshot exists).
bool signal_graph_executor_eligible(const SignalGraph& graph);

// Build the executor snapshot (plan + bindings) for an eligible topology,
// resolving each Gain node's live atomic via `gain_for`. Shared by the live-graph
// translation and by SignalGraph::compile_ (which embeds the snapshot in the
// compiled graph, resolving atomics from its own runtime). Returns false (and
// leaves `out` empty) if the topology is not eligible, a Gain atomic is
// missing, or the canonical graph-runtime plan/snapshot cannot be built under
// its own limits. Does NOT size a pool or set a keepalive — the caller owns
// those.
bool build_executor_snapshot(std::span<const GraphNode> nodes,
                             std::span<const Connection> connections,
                             const std::function<std::atomic<float>*(NodeId)>& gain_for,
                             format::GraphRuntimeSnapshot& out);

// Translate a prepared, eligible `graph` into `out` (snapshot + sized pool +
// keepalive). Returns false (out.valid == false) when ineligible/unprepared. On
// success `out` drives process_routed() with output bit-identical to
// SignalGraph::process()'s own walk for the eligible subset.
bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out);

} // namespace pulp::host
