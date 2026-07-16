# Choosing a Processing Model

Pulp has **one way to author DSP** and **one way to compose already-built units
into a runtime-editable graph**. They sit at different altitudes and are not
interchangeable. This page is the canonical answer to "should I write a
`Processor` or reach for `SignalGraph`?"

## The two layers

| Layer | What it is | You use it to… |
|-------|------------|----------------|
| **`Processor`** (`pulp::format::Processor`) | The single authoring unit for a plugin/effect/instrument. One `process()` callback per block; internal DSP is composed from `pulp::signal::*` helpers inside that callback. | Write a plugin, effect, instrument, or MIDI effect. **The default for essentially all DSP authoring.** |
| **`SignalGraph`** (`pulp::host::SignalGraph`) | A host-side, runtime-editable routing engine that connects *already-built* units — loaded plugins (`PluginSlot`), I/O, gain, and custom nodes — into a DAG. | Host other plugins, build a rack/mixer, drive a node editor, or load/save a routed topology (`.pulpgraph`). |

A `SignalGraph` **node** wraps a unit (a hosted plugin, or a `CustomNodeType`); it
is not a new way to *write* DSP. Authoring still happens in a `Processor`.

## The decision

> **Is the signal topology fixed when you build the plugin? → `Processor`.
> Is it edited or loaded at run time? → `SignalGraph`.**

"Fixed topology" does **not** mean "simple" or "one-in/one-out." A `Processor` is
still the right tool when its internal DSP is:

- **polyphonic** (synths, samplers — voices are not graph nodes),
- **multi-bus** — sidechain input and descriptor-declared aux / secondary
  output buses via the `ProcessBuffers` surface, not a graph. CLAP and VST3
  route writable secondary outputs through `ProcessBuffers`; AUv3 currently
  exposes the sidechain input but not secondary outputs.
- **sidechained** (a sidechain is an input bus on the `Processor`),
- **internally branching** (parallel/serial chains, mid/side, dry/wet) — compose
  `pulp::signal::*` blocks in `process()`,
- **oversampled or convolution-based** — an implementation detail of one `Processor`.

Reach for `SignalGraph` only when the **routing itself** is dynamic: hosting
external plugins, a user-editable rack/mixer, a node editor, or a topology loaded
from disk. Do not express a fixed internal signal chain as a graph — it is slower
and harder to reason about than a straight `process()`.

### Quick reference

| Task | Model |
|------|-------|
| Reverb, compressor, EQ, filter | `Processor` |
| Synth or sampler (any polyphony) | `Processor` |
| Drum instrument with a sidechain input or aux outputs | `Processor` + `ProcessBuffers` |
| Sidechain compressor | `Processor` + sidechain input bus |
| Parallel/serial FX *inside one plugin* | `Processor` (compose `signal::*`) |
| A host app that loads and routes VST3/AU/CLAP plugins | `SignalGraph` |
| A user-editable effects rack / modular patch | `SignalGraph` |
| A visual node editor | `SignalGraph` (the editor is UI over the graph) |

## What goes inside a node

Deciding you genuinely need a `SignalGraph` raises the next question: what does
each node actually contain? There are three sources, and they are not
interchangeable.

| Node content | Where it comes from | Use it for |
|--------------|---------------------|------------|
| **Built-ins** — `Input`, `Output`, `Gain`, `MidiInput`, `MidiOutput` | The graph itself; no code to write | Plumbing — host I/O, a gain stage, MIDI in and out. |
| **`Plugin` node**, wrapping a `PluginSlot` | A VST3 / AU / CLAP / LV2 binary on disk, discovered by `PluginScanner` and opened by `PluginSlot::load()` | **Third-party components** — units you did not write and cannot recompile. |
| **`Custom` node**, from a registered `CustomNodeType` | Your own C++, registered per graph with a `type_id`, version, and port shape | **Your own graph-native components** — utility and extension nodes. |

The line between the last two rows is the one worth internalizing: a
`CustomNodeType` is a graph utility, **not** a plugin authoring surface. If the
unit is a plugin you intend to ship, author it as a `Processor` and let a graph
host it like any other. Reach for `CustomNodeType` when the node only ever makes
sense *inside* your graph.

A `CustomNodeType` can be stateless (just a `process` callback) or stateful
(supply `create`/`destroy` and the graph owns one instance per node, with
`prepare`/`release`/`reset`/`save_state`/`load_state` on it). Stateful custom
nodes follow the same threading contract as `PluginSlot`: lifecycle calls on the
UI/main thread, `process_instance` on the audio thread and real-time-safe.

### If you are building a node editor

The graph is designed to be the model under an editor, so the pieces an editor
needs are already contracts rather than conventions:

- **`NodeId` is stable** across connection edits, so it is a safe identity anchor
  for the editor's own view state.
- **Unresolved types survive a round trip.** Serialization stores each custom
  node's `type_id` and `version`. Loading a topology whose custom types this
  machine has not registered still produces a placeholder node with the saved
  identity, port shape, connections, and opaque state, and reports the type in
  `LoadResult::missing_custom_node_types` — so a patch does not silently lose
  nodes when it moves between machines.
- **Bound user-built graphs** with `SignalGraph::set_limits()` before `prepare()`,
  and preflight generated or scripted topologies with
  `validate_generated_graph()`. A graph assembled by a user (or a script) is
  untrusted input.
- **Editing while it plays** is supported: connection edits, and hot-swapping the
  plugin inside a node via the `begin_swap_edit()` / `prepare_swap()`
  transaction.
- **`GraphEditorView`** (`pulp::view::widgets`) already renders a `SignalGraph`
  as a draggable, connectable canvas if you want the model and the view together.

### Embedding a hosted plugin's own editor

A `Plugin` node can show the plugin's real GUI rather than a parameter list you
rebuild yourself. `pulp::view::EditorAttachment::create(slot, window)` asks the
slot for its editor and embeds it in a `WindowHost`, returning an RAII handle
that detaches and destroys in the right order.

Two constraints shape what you can build on top of it:

- **A native child always composites ABOVE Pulp's GPU layer.** The OS window
  server draws it over the Skia surface, so a node editor cannot paint wires,
  selection chrome, or overlays *on top of* an embedded plugin GUI. Native
  children suit full-region embeds — a detail pane, an inspector — better than a
  live GUI floating mid-canvas under other widgets. Clipping still works:
  `set_native_child_view_clip` masks a child to its scroll viewport.
- **Coverage is per format and per platform.** CLAP editors embed on macOS
  today. VST3, AU, and LV2 slots report no editor, and Windows/Linux have no
  desktop `WindowHost` implementing the native-child seam, so
  `EditorAttachment::create` returns nullptr there. Null is the honest
  "no editor" answer everywhere — branch on it rather than assuming a view.


## The two layers compose

`Processor` and `SignalGraph` are altitudes, not a fork in the road. A graph that
has stopped being dynamic can be lowered back down: `bake()` freezes a prepared
graph into a `BakedGraphProcessor` — a self-contained `Processor` you can ship
like any other. It drives the frozen plan through the same executor the live
graph uses, so its output matches the live walk for the lowerable subset.

Baking fails loudly rather than silently mis-baking. A hosted `Plugin` node is
refused because it owns opaque external state and the result would not be
self-contained; a `Custom` node lowers only if its type opted in with
`lowerable = true`, matches the node's shape, and is transport-independent. See
[Signal Graph Reference](signal-graph.md#baking-a-graph-to-a-shippable-artifact)
for the `.pulpbake` file format and its signature gate.

## Reserved terminology

To keep the two layers from blurring, these terms each mean exactly one thing.

| Term | Means | Not |
|------|-------|-----|
| **`Processor`** | The DSP authoring unit. | A generic graph node. |
| **`SignalGraph`** | The host-side routing engine (the user-facing graph). | Any fixed internal DSP chain. |
| **node** | A vertex in a `SignalGraph`; wraps a unit. | A `Processor` itself. |
| **`PluginSlot`** | The host wrapper around a loaded plugin. | A `Processor` you authored directly. |
| **`CustomNodeType`** | A graph utility/extension node. | A plugin authoring surface. |
| **bus / output** | A named multi-channel I/O group on a unit. | A `SignalGraph` "output node" — say "output node" explicitly. |

When writing docs or code comments, avoid the bare word "graph" for authoring
work — qualify it as `SignalGraph`. A plugin is always a `Processor`; there is no
such thing as a graph-authored plugin.

## How this is enforced

Two CI guards keep the model from blurring back into two surfaces. Both run in
`gates.sh`, the pre-push hook, and the `Versioning & Skill-Sync` workflow.

- `tools/scripts/processing_model_terms_lint.py` flags the reserved-terminology
  anti-phrases (e.g. "graph plugin") in docs and source. <!-- terms-lint: allow -->

- `tools/scripts/single_backend_guard.py` asserts the structural invariants:
  exactly one routing backend (`GraphRuntimeExecutor`) defines the routed-graph
  execution entry points; `pulp create` offers no scaffold that treats a
  `SignalGraph` as an authoring surface; the public generated-DSP ABI entry
  symbols stay the sanctioned pair
  (the `native_core.h` Processor-level FFI and the `pulp_node_v1` custom-node
  ABI); and the routed-vs-`SignalGraph` differential parity test stays wired
  into the build. Sanctioning a genuinely-new engine, scaffold, or ABI is a
  deliberate allowlist edit in that guard.

## See also

- [Signal Graph Reference](signal-graph.md) — the `SignalGraph` node and edge model.
- [Hosting guide](../guides/hosting.md) — scanning, loading, and routing plugins.
- [Node ABI](node-abi.md) — the source contract for `Processor`, `PluginSlot`, and custom nodes.
- [DSP threading](../guides/dsp-threading.md) — the real-time contract every `Processor` must honor.
