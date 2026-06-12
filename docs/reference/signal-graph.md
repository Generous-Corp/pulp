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
Serializer coverage pins this as a compatibility guarantee: unresolved custom
nodes survive save-load-save-load cycles with their identity, port shape,
connections, and opaque state preserved, while registered types resolve only by
the exact saved `(type_id, version)` and matching shape.

## Connections

`connect(from, from_port, to, to_port)` adds an audio edge and returns
false if it would create a cycle (`would_create_cycle()` exposes the same
check without mutating). Disconnect by `NodeId`, port, or by full edge.

Four connection variants cover the non-audio-passthrough cases:

- `connect_midi(from, to)` routes `MidiBuffer` events between node-scoped
  MIDI in/out buffers (ports are ignored). Participates in cycle
  detection the same way audio edges do. `inject_midi(node, buf)` loads
  a `MidiInput`'s output before a block; `extract_midi(node, &out)`
  drains a `MidiOutput`'s input after a block.
- `connect_feedback(from, port, to, port)` closes a cycle with an
  explicit one-block delay — the destination reads the source's previous
  block's output. Invisible to the topological sort and PDC so the
  runtime stays DAG-ordered.
- `connect_automation(from, port, plugin, param, lo, hi)` samples a source
  audio port at the start and end of the block and delivers sparse
  `ParameterEvent`s into `PluginSlot::process()`.
- `connect_audio_rate_modulation(from, port, plugin, param, lo, hi)` declares
  a dense per-sample modulation edge. It is accepted only when the graph edge
  can be represented as a valid `state::ModulationLane` with `GraphNode` scope:
  the destination parameter must be continuous, writable, automatable,
  modulatable, and `HostParamInfo::rate == AudioRate`. Accepted edges emit one
  `ParameterEvent` per sample and participate in latency alignment.
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

## Prepare Limits

`SignalGraph::GraphLimits` bounds the graph shape accepted by `prepare()`.
Hosts can call `set_limits()` before preparing generated, scripted, or
user-built graphs to cap node count, connection count, total declared ports,
maximum block size, and deterministic work units. Work units are a stable
shape/block-size score for importers, not hardware cycle counts: the estimate
combines node count, connection count, declared ports multiplied by block size,
dense audio-rate modulation lanes, and sparse automation edges. Exceeding a
limit fails `prepare()` before plugin prepare or compiled-snapshot allocation,
leaving the graph silent until a valid prepare succeeds.

Generated or scripted graph importers should call
`validate_generated_graph(max_block_size)` after applying importer-specific
limits and before calling `prepare()`. The validation result is a pure preflight
check: it reports the first rejected budget as a stable reason plus actual and
limit counts, and it does not clear or replace an already prepared snapshot.
`prepare()` uses the same validation internally, but remains the mutating
lifecycle step that drops any live snapshot before rebuilding a new one.
Importers can call `estimate_generated_graph_work_units(max_block_size)` first
when they need to present or tune a CPU/work budget before enforcing
`GraphLimits::max_estimated_work_units`.

`.pulpgraph` import also validates generated node shapes before materializing
the graph. Built-in node types must declare the shape their runtime actually
uses: audio inputs have only outputs, audio outputs have only inputs, gain nodes
are the built-in stereo utility, and MIDI source/sink nodes use one MIDI edge.
Plugin and custom nodes may declare their own non-negative port counts. Invalid
shapes, negative port counts, malformed state blobs, stale connection IDs,
invalid ports, and non-feedback cycles fail closed or are skipped before
`prepare()` publishes a runtime snapshot.

Generated/scripted graph import, serialization, `set_limits()`, `prepare()`,
`release()`, custom-node registration, and custom state load/save APIs are
control-thread only. They are not audio-thread APIs. Mutating graph shape or
custom state invalidates the live snapshot; `process()` returns silence until a
subsequent successful `prepare()` publishes a new immutable snapshot. Generated
or scripted runtimes must express audio-thread work only through the prepared
snapshot's `process()` callbacks and preallocated event/audio buffers.

After a successful `prepare()`, `SignalGraph::prepared_stats()` reports the
compiled graph's node/order/connection/port counts, prepared maximum block
size, and fixed audio, automation, and delay-line buffer bytes. The snapshot is
cleared when topology or limits change, when `release()` runs, or when
`prepare()` fails. Treat it as the host-facing budget/accounting view of the
prepared runtime, not as an audio-thread routing API.

Hosts that run optional graph-side work after prepare, such as preview
rendering, analysis refresh, or noncritical diagnostics, can pass a
`runtime::RuntimeBudgetFrame` to
`SignalGraph::evaluate_optional_runtime_budget()`. The graph derives a stable
cost from prepared stats, applies the shared runtime budget policy, and returns
a report with the decision, estimated cost, prepared state, and updated frame
counters. `Run` means the optional work can proceed; `Defer`, `Shed`, and
`Bypass` are the explicit fallback states. Core graph audio processing remains
on the prepared snapshot path and is not skipped by this optional-work helper.
The cost model is deterministic: node count, connection count, declared ports
times prepared block size, and fixed prepared buffer bytes. It is a portable
budget benchmark, not a hardware CPU-time estimate.
Unsupported behavior is explicit: this helper does not skip graph audio nodes,
change routing, or virtualize plugins. It only lets host-side optional work
choose a degraded path before that work starts.

## Latency & PDC

Every `PluginSlot` reports `latency_samples()`. During `prepare()` the
graph walks the topology, computes each node's input / output latency,
and allocates per-connection delay lines so parallel branches converge
aligned. Query results with `SignalGraph::latency_samples()` (graph-wide
total) and `node_latency_samples(id)` (alignment at a specific node).
Feedback edges (`connect_feedback`) don't contribute to PDC — the
one-block delay absorbs their alignment.
Audio-rate modulation edges do contribute to PDC: when a parameter is driven
from a lower-latency branch than the destination plugin's audio input, the
graph delays the dense parameter-event stream by the same amount as an audio
connection.

## Parameters

`set_node_parameter(node, id, value)` forwards a normalized value to the
plugin via `PluginSlot::set_parameter()`; `get_node_parameter(node, id)`
reads back. `connect_automation()` delivers two sparse control points per
block for control-rate movement. `connect_audio_rate_modulation()` delivers
sample-by-sample events for parameters explicitly marked audio-rate and
projects accepted edges through the typed modulation-lane contract.

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
