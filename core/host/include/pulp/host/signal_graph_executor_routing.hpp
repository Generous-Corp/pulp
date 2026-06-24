#pragma once

#include <pulp/format/graph_runtime_executor.hpp>

#include <memory>

namespace pulp::host {

class SignalGraph;

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

// True iff `graph` is prepared and contains ONLY the node/connection kinds the
// executor reproduces bit-exactly today:
//   - nodes: AudioInput, AudioOutput, Gain (Gain is fixed 2-in/2-out, so it
//     always fully writes its outputs — required for parity under slot reuse);
//   - connections: plain audio, feedforward or feedback (no MIDI, automation,
//     audio-rate-modulation, or sidechain edges).
// Plugin/Custom/MIDI nodes and the special edge kinds keep the legacy walk;
// they become eligible once the executor grows the matching capabilities
// (per-connection delay compensation, sidechain, MIDI, automation).
bool signal_graph_executor_eligible(const SignalGraph& graph);

// Translate `graph` into `out`. Returns false (and leaves out.valid == false)
// when the graph is not prepared or not eligible. On success `out` can be driven
// with process_routed() and produces output bit-identical to SignalGraph's own
// process() walk for the eligible subset.
bool build_signal_graph_executor_routing(const SignalGraph& graph,
                                          SignalGraphExecutorRouting& out);

} // namespace pulp::host
