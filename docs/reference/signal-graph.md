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
  a dense per-sample modulation edge. It is accepted only when the graph edge
  can be represented as a valid `state::ModulationLane` with `GraphNode` scope:
  the destination parameter must be continuous, writable, automatable,
  modulatable, and `HostParamInfo::rate == AudioRate`. Accepted edges emit one
  `ParameterEvent` per sample and participate in latency alignment. While the
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
snapshot's `process()` callbacks and preallocated event/audio buffers. Large
prepared graph fixtures are expected to process without allocating after the
snapshot has been warmed.

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

## Executor Routing

`SignalGraph::set_canonical_executor_routing_enabled(true)` requests the
serial `GraphRuntimeExecutor` path for eligible prepared graphs.
`SignalGraph::set_parallel_routing_enabled(true)` requests the levelized
parallel executor path for the same eligible subset and falls back to serial
executor routing or the legacy walk when the parallel snapshot cannot run.

Parallel routing defaults to dispatching every eligible multi-node level.
Hosts can call `set_parallel_min_work_units(channel_samples)` to keep levels
below that static work-weight x block-size threshold serial, avoiding worker
fork/join overhead on small graphs. `routing_executor_stats()` exposes
diagnostic counters such as parallel levels dispatched and serial levels run so
tests and tools can verify the route actually taken. These flags and counters
are telemetry/control knobs for the prepared execution path; they do not change
graph topology or the generated/import validation budgets above.

`SignalGraph::set_anticipation_enabled(true)` opts a prepared graph into
anticipative rendering on the canonical executor path. When the graph has an
eligible latent interior, `prepare()` builds an `AnticipationLane` and a routed
skip mask. A host-side producer calls `pump_anticipation(max_blocks)` from one
background thread to render that interior ahead of the audio deadline; the audio
thread's `process()` consumes a pre-rendered boundary block, pre-fills the routed
executor slots, and runs the rest of the graph with the interior masked so its
plugin state advances only on the producer. Anticipation is default-off and must
be enabled before `prepare()`. If the lane underruns or the block size no longer
matches, the splice writes silence for that pre-rendered boundary rather than
falling back to a live interior render, which would double-advance plugin state.
Hosts must stop and join the producer thread before any graph mutation or
`prepare()` call.

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
parameters explicitly marked audio-rate and projects accepted edges through the
typed modulation-lane contract.

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
- **Live controls and MIDI handoff** (`set_node_gain`, `inject_midi`,
  `extract_midi`) are covered by the combined TSan-oriented graph race test
  `"[host][graph][threading][race][tsan][midi]"`.
- **Load** (`PluginSlot::load`) runs on a worker thread; the returned
  slot is handed to the graph only after it's fully loaded.

## Baking a graph to a shippable artifact

A live `SignalGraph` is editable and carries per-node overhead. Once a graph is
final you can **bake** it — freeze the topology into a single optimized
`Processor` that drives the routed executor directly (bit-identical output to the
live graph, no graph bookkeeping).

There are two trust boundaries:

- **In-process bake — trusted.** `bake(graph)` returns a `BakedGraphProcessor`
  for a prepared, lowerable graph. Nothing is serialized; this is the fast path
  for "compile my graph to max performance at release." Non-lowerable graphs
  (a hosted `Plugin` node, a MIDI/automation/sidechain edge, a non-opted-in
  `Custom` type) are refused loudly with a `LowerRejectReason`.

- **On-disk `.pulpbake` — untrusted, signature-gated.** To ship or hot-update a
  baked graph as a *file*, sign it and verify on load:

  ```cpp
  // Write path (publisher):
  auto plan = bake_to_plan(graph);                       // std::optional<BakedPlan>
  auto bytes = write_baked_signed(*plan, signer_priv64); // Ed25519-signed .pulpbake

  // Load path (consumer):
  BakedTrust trust; trust.trusted_public_keys.push_back(publisher_pub32);
  LowerResult r = load_baked(bytes, trust, host_custom_types);
  if (r.accepted) use(std::move(r.processor));
  ```

  `load_baked` verifies the Ed25519 signature over a domain-separated message
  (tag + versions + plan length + plan hash) **before** it parses any plan byte,
  then bounded-parses the plan under fixed caps, rebuilds it into a graph, and
  runs it back through `bake()` — so the file's implicit claim is never trusted
  and the full lowerability proof re-runs on the reconstructed topology. There is
  **no unsigned load path**; revoke a publisher by dropping its key from `trust`.

### The `lowerable` author contract

A `Custom` node type is bakeable only if it opts in with `lowerable = true` and
its process is a pure function of its input block (no transport, no external
state a frozen topology can't capture). Custom process code is **never** stored
in the artifact — a `Custom` record carries only `{type_id, version, state}` and
the code is re-resolved from the host's registered types at load, exactly like a
node pack. v1 of the on-disk codec supports **stateless** custom nodes; a record
carrying opaque state is refused (`StatefulCustomNotYetLoadable`).

## See also

- [Hosting guide](../guides/hosting.md) — end-to-end example.
- [`pulp::host::PluginSlot`](../../core/host/include/pulp/host/plugin_slot.hpp)
- [`pulp::host::SignalGraph`](../../core/host/include/pulp/host/signal_graph.hpp)
- [`pulp::host::bake` / `load_baked`](../../core/host/include/pulp/host/baked_graph_processor.hpp)
