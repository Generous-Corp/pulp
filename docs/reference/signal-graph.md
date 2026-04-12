# Signal Graph Reference

`pulp::host::SignalGraph` is the DAG engine that connects `PluginSlot`
instances (and built-in nodes: input, output, gain, MIDI in/out) into a
routable audio topology.

## Nodes

| NodeType       | Role                                      | Ports |
|----------------|-------------------------------------------|-------|
| `Input`        | Source from the host's input bus          | 0 in → N out |
| `Output`       | Sink to the host's output bus             | N in → 0 out |
| `Gain`         | Scalar gain, no allocation                | 1 in → 1 out |
| `Plugin`       | Wraps a `PluginSlot`                      | N in → N out |
| `MidiInput`    | Source for MIDI events                    | 0 in → 1 out |
| `MidiOutput`   | Sink for MIDI events                      | 1 in → 0 out |

Each node has a stable `NodeId` that survives connection edits. The graph
owns the nodes; removing a node invalidates its id.

## Connections

`connect(from, from_port, to, to_port)` adds an edge and returns false if
it would create a cycle (`would_create_cycle()` exposes the same check
without mutating). Disconnect by `NodeId`, port, or by full edge.

The DAG constraint is enforced up front — feedback loops must be expressed
with an explicit delay node (see [hosting guide](../guides/hosting.md)).

## Processing order

`processing_order()` returns the topological sort. The audio callback
walks this vector once per block:

1. Input nodes copy from the host buffer to their output port.
2. Plugin nodes call `PluginSlot::process()` with their gathered inputs.
3. Gain nodes multiply in place.
4. MIDI nodes route events the same way audio flows.
5. Output nodes copy to the host buffer.

Topological sort is stable: for a given set of nodes and connections, the
resulting order is deterministic, so routing is reproducible across runs.

## Latency & PDC

Every `PluginSlot` reports `latency_samples()`. The graph sums latency
along each path to each output and inserts delay-line nodes on the
shorter paths so signals stay aligned. Latency is recomputed whenever the
topology changes.

## Thread model

- **Build & edit** (add / connect / remove) runs on the UI thread.
- **Process** runs on the audio thread over the snapshotted processing
  order. The snapshot is swapped under a lock-free publish so edits never
  tear the running order.
- **Load** (`PluginSlot::load`) runs on a worker thread; the returned
  slot is handed to the graph only after it's fully loaded.

## See also

- [Hosting guide](../guides/hosting.md) — end-to-end example.
- [`pulp::host::PluginSlot`](../../core/host/include/pulp/host/plugin_slot.hpp)
- [`pulp::host::SignalGraph`](../../core/host/include/pulp/host/signal_graph.hpp)
