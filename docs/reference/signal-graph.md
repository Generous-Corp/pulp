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
| `Custom`       | String-keyed extension node               | Registered shape |

Each node has a stable `NodeId` that survives connection edits. The graph
owns the nodes; removing a node invalidates its id.

Custom node types are registered per graph with `CustomNodeType`
(`type_id`, `version`, input ports, output ports, default name, optional
process callback) before calling `add_custom_node(type_id)` or
`add_custom_node(type_id, version)`. Graph serialization stores the custom
`type_id` and `version` so a topology can be reloaded on another machine.
If the target graph has not registered the exact matching version and shape,
the loader still creates a placeholder `Custom` node with the saved identity
and port shape and reports the unresolved type in
`LoadResult::missing_custom_node_types`.

## Connections

`connect(from, from_port, to, to_port)` adds an audio edge and returns
false if it would create a cycle (`would_create_cycle()` exposes the same
check without mutating). Disconnect by `NodeId`, port, or by full edge.

Four connection variants cover the non-audio-passthrough cases:

- `connect_midi(from, to)` routes `MidiBuffer` events between node-scoped
  MIDI in/out buffers (ports are ignored). Participates in cycle
  detection the same way audio edges do. `inject_midi(node, buf)` loads
  a `MidiInput`'s output before a block; `extract_midi(node, &out)`
  reads the latest `MidiOutput` input snapshot after a block. MIDI edges
  preserve the full logical block: short MIDI events, SysEx sidecars, and an
  attached `UmpBuffer`. MPE is derived from those events rather than stored as a
  separate graph-owned sidecar, so route MIDI 1.0 MPE channel messages or MIDI
  2.0 per-note UMP packets through the graph and derive `MpeBuffer` at the
  processor/adapter boundary with `MpeVoiceTracker`.
- `connect_feedback(from, port, to, port)` closes a cycle with an
  explicit one-block delay — the destination reads the source's previous
  block's output. Invisible to the topological sort and PDC so the
  runtime stays DAG-ordered.
- `connect_automation(from, port, plugin, param, lo, hi)` samples a source
  audio port at the start and end of the block and delivers sparse,
  source-block-relative `ParameterEvent`s into `PluginSlot::process()`. Sparse
  graph automation uses two events per automated parameter, so large host
  blocks such as 2048 or 4096 samples do not consume one queue slot per sample.
- `connect_audio_rate_modulation(from, port, plugin, param, lo, hi)` declares
  a dense per-sample modulation edge. It is accepted only for continuous,
  automatable `HostParamInfo::rate == AudioRate` parameters, emits one
  `ParameterEvent` per sample, and participates in latency alignment. While the
  plugin ABI still carries dense modulation through the fixed 1024-slot
  `ParameterEventQueue`, `prepare()` fails closed when
  `audio_rate_params * max_block_size + sparse_params * 2` would exceed that
  capacity. For example, one dense lane is allowed at 1024 samples and rejected
  at 2048/4096 samples until a separate dense modulation view replaces the
  sparse-event transport for those blocks.
- Sidechain is *not* a separate API: connect a secondary source to the
  plugin node's sidechain audio-port indices (e.g. `connect(side, 0, p,
  2)` when the plugin exposes ports 2/3 as its sidechain bus).

## Processing order

`processing_order()` returns the topological sort. The audio callback
walks this vector once per block:

1. Input nodes copy from the host buffer to their output port.
2. Plugin nodes call `PluginSlot::process()` with their gathered inputs.
3. Gain nodes multiply in place.
4. Custom nodes run their registered process callback when the registered
   version and shape match the node; unresolved, shape-mismatched, or
   metadata-only registrations pass audio inputs through to matching outputs.
5. MIDI nodes route events the same way audio flows.
6. Output nodes copy to the host buffer.

Topological sort is stable: for a given set of nodes and connections, the
resulting order is deterministic, so routing is reproducible across runs.

## Latency & PDC

Every `PluginSlot` reports `latency_samples()`. During `prepare()` the
graph walks the topology, computes each node's input / output latency,
and allocates per-connection delay lines so parallel branches converge
aligned. Query results with `SignalGraph::latency_samples()` (graph-wide
total) and `node_latency_samples(id)` (alignment at a specific node).
Feedback edges (`connect_feedback`) don't contribute to PDC — the
one-block delay absorbs their alignment.
Sparse automation edges do not contribute to PDC. They are control-rate
source samples for the current graph block, delivered as two sparse control
points. A plugin can use `ParamCursor` / `for_each_subblock()` to interpolate
or render spans between those points, but the graph does not delay that sparse
event stream to match a destination plugin's input latency. Use audio-rate
modulation when a parameter stream must stay phase-aligned with a delayed
audio path.

Audio-rate modulation edges do contribute to PDC: when a parameter is driven
from a lower-latency branch than the destination plugin's audio input, the
graph delays the dense parameter-event stream by the same amount as an audio
connection.

## Parameters

`set_node_parameter(node, id, value)` forwards a normalized value to the
plugin via `PluginSlot::set_parameter()`; `get_node_parameter(node, id)`
reads back. `connect_automation()` delivers two sparse control points per
block for control-rate movement; processors that need a smooth value between
those points should use their normal parameter-ramp or subblock helpers.
`connect_audio_rate_modulation()` delivers sample-by-sample events for
parameters explicitly marked audio-rate.

## Persistence

`GraphSerializer` writes `.pulpgraph` JSON with a `format_version` field.
Loaders read that field before materializing nodes. Older graph versions can
register `GraphSerializer::register_migration()` steps to upgrade JSON to the
current format; newer or unreadable versions fail closed instead of being
silently interpreted as current data.

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
