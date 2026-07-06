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
