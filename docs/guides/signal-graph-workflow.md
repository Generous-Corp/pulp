# Unified SignalGraph workflow

This guide is the practical path for a developer or coding agent building a
graph-backed instrument, effect, host, or modular application. It connects the
existing graph library, kit fixtures, validation, prepared edits, offline
rendering, diagnostics, and bake tooling. It does not introduce another graph
runtime or a second control authority.

## Start here

| If you are trying to… | Start with… | Finish by proving… |
|---|---|---|
| Build fixed DSP or voice logic | [Processing models](../reference/processing-models.md) and `Processor` | Processor lifecycle and audio output tests |
| Learn the graph vocabulary | [Signal Graph Reference](../reference/signal-graph.md) | A small graph that loads, prepares, and renders |
| Study a complete example | [Sample granular graphs](sample-granular-graphs.md) and the [installed consumer README](../../examples/sample-region-allpass/README.md) | The example's structural and executable receipts |
| Build editable routing or hosted composition | [Choose the graph boundary](#choose-the-graph-boundary) | A prepared graph and one executable render |
| Turn an authored or generated graph into audio | [Load and validate before preparation](#load-and-validate-before-preparation) | Limits, preparation, partition contract, and output receipt |
| Change a running graph | [Prepare and publish a safe edit](#prepare-and-publish-a-safe-edit) | Valid edit acceptance and invalid edit rollback |
| Diagnose or reproduce a graph | [Inspect and diagnose](#inspect-and-diagnose) | Semantic receipt, generation receipt, diagnostics, and audio artifact |
| Automate graph work as an agent | [Agent and Forge acceptance workflow](#agent-and-forge-acceptance-workflow) | One deterministic fixture with positive and negative controls |

Humans can read the sections top to bottom. Agents should use the table as a
dispatch map, then follow the selected section and record the artifacts named
there. The [capability matrix](../reference/signal-graph-capabilities.md) is the
compact status view; this guide is the operational sequence.

## What a graph is, and when to use one

An audio graph is a set of processing nodes connected by typed paths. Audio,
MIDI, automation, modulation, and feedback move along those paths in a defined
order. The graph is useful when the application needs to make the connections
themselves part of the product: a patch editor, instrument routing network,
effect chain, mixer, or hosted processing rack.

| Use this boundary | When it fits | Typical work |
|---|---|---|
| `Processor` | The signal algorithm and its state are fixed inside one component | Oscillators, voices, filters, samplers, meters, and reusable DSP modules |
| `SignalGraph` | The application edits nodes, routing, hosted processing, or graph-level automation | Modular instruments, effect chains, mixers, plugin hosts, and patch editors |
| Timeline/playback | The behavior depends on transport, tempo, arrangement, pre-roll, or tails | Songs, clips, automation lanes, and offline renders with transport |

Pulp graphs currently cover DAG composition, stable node identity, prepared
topology edits, MIDI and bounded modulation lanes, explicit feedback delay,
latency/PDC, serial and parallel execution, graph and timeline offline paths,
serialization, and signed bake artifacts. Hosted plugin breadth, richer bus
projection, process isolation, and some event-format and continuity cases remain
explicitly gated; read the [capability matrix](../reference/signal-graph-capabilities.md)
before treating a row as a universal guarantee.

The practical rule is to keep fixed DSP and voice state in `Processor`, and use
`SignalGraph` where composition is the feature being edited. `ProcessorNode` wraps a configured processor-node instance; the graph retains
its node lifetime while owning routing and execution. A timeline binding likewise
requires its graph and program store to outlive the binding. Choosing the graph boundary does not require a second runtime or a
duplicate control model.

Ask these three questions before writing code:

1. **Is the signal algorithm fixed?** Put it in a `Processor`, including complex
   voices, sidechains, oversampling, and internal serial or parallel stages.
2. **Can a user or host change the routing while the product runs?** Use a
   `SignalGraph`, with prepared edits and an immutable published snapshot.
3. **Does behavior depend on musical time?** Use the timeline model and its
   transport-aware renderer; bind graph work underneath it when needed.

A graph fixture is a routing and lifecycle example, not proof of every host or
format. The [sample-region example](../../examples/sample-region-allpass/) is a
small graph-backed fixture; the [timeline phase example](../examples/timeline-phase1.md)
shows the transport-owned path. Use both when a feature crosses the graph and
timeline boundaries.

## Choose the graph boundary

Start with [Processing models](../reference/processing-models.md). Keep fixed
DSP, voice state, and sample processing in `Processor`. Use
`pulp::host::SignalGraph` when the routing or hosted composition is the thing
the application edits: a rack, mixer, modular patch, plugin host, or node
editor. `ProcessorNode` uses the configured processor-node path while `SignalGraph`
retains the node lifetime and owns routing and execution. Timeline playback uses its existing
binding to lower transport-driven work into graph nodes.

## Discover a fixture or kit

Use the existing kit workflow to discover reviewed graph fixtures and their
declared files:

```sh
pulp kit search graph --root ./fixtures/packages --lane kit --json
pulp kit inspect ./fixtures/packages/my-graph-kit --json
pulp kit validate ./fixtures/packages/my-graph-kit --json
pulp kit verify ./fixtures/packages/my-graph-kit --project . --json
```

`signal-graph-fixture` entries are structurally checked by `pulp kit verify`.
That check proves manifest shape, paths, and declared fixture metadata; it does
not execute a graph. Treat an unexecuted fixture as an input to the next step,
not as audio evidence. `pulp kit apply` remains a reviewed project mutation and
must be used only after inspecting its plan.

### Executable fixture dispatch

The use-case names in the planning material map to checked-in source and test
targets as follows. Run a target through the normal governed build, then run the
matching CTest name from the configured build directory. A source link without a
target is an authoring reference, not a passing runtime receipt.

| Workflow | Start with | What it proves | Current boundary |
|---|---|---|---|
| Voice and polyphony (X1) | [`test_instrument_voice_allocator.cpp`](../../test/test_instrument_voice_allocator.cpp), [`test_voice_runtime_facade.cpp`](../../test/test_voice_runtime_facade.cpp), [`test_mpe_synth_voice.cpp`](../../test/test_mpe_synth_voice.cpp) | voice lifecycle and per-note building blocks | No single complete instrument fixture yet |
| Feedback and modulation (X2) | `pulp-test-host-signal-graph`, `pulp-test-signal-graph-executor-parity` | graph execution, feedback and parity controls | A combined serialized replay receipt remains open |
| Reusable components (X3) | `pulp-test-signal-graph-x9-modular-instrument` (`X3` cases) | two independent component instances in the public-API fixture | General template authoring is still higher-level guidance |
| Routing and mutation (X4) | `pulp-test-signal-graph-prepared-swap-live` and `pulp-test-signal-graph-pdc-swap-continuity` (declared in `test/cmake/sampler_runtime_tests.cmake`) | prepared edits, latency/PDC and continuity policy | Full aux/sidechain application routing remains open |
| Hosted processing (X5) | `pulp-test-external-plugin-graph-fixture` (declared in `test/cmake/timeline_tests.cmake`), plus the conditional `pulp-test-signal-graph-x9-hosted-format-receipt` CTest in `CMakeLists.txt` when the X9 target exists | conditional format availability and graph-host seams | Bus negotiation, graph-visible failure, offline parity and isolation remain gated |
| Generated instrument (X6) | `pulp-test-dsl-processor` | deterministic generated `Processor` path | Graph/template discoverability and full live/baked/offline join remain open |
| Mixed-rate/sample-region work (X7) | [sample granular graphs](sample-granular-graphs.md), `pulp-test-host-signal-graph` | documented rate and causality building blocks | A complete mixed-rate authoring fixture is not yet claimed |
| Inspection and replay (X8) | `pulp-test-live-dsp-telemetry-graph`, `pulp-test-signal-graph-offline-parity` | diagnostics and output parity surfaces | Joined topology/event/fault replay receipt remains open |
| Complete modular instrument (X9) | `pulp-test-signal-graph-x9-modular-instrument` | public-API integration of voices, modulation, state, mutation and offline render | Hosted-format matrix and replay/state/output closure remain conditional |

For a clean-checkout productivity measurement, record the first successful
fixture command, elapsed setup time, concepts required, manual integration
steps, and the first diagnostic artifact. Repeat the same exercise after any
documentation change. This keeps a discoverability improvement distinct from
new runtime capability.

## Load and validate before preparation

For a generated or authored `.pulpgraph` document, keep the JSON bytes and
serializer version as part of the fixture receipt. The library workflow is:

1. Load with `GraphSerializer::from_json` into a control-thread graph.
2. Apply the graph's `GraphLimits` for the target application.
3. Call `validate_generated_graph(max_block_size)` before `prepare()` and record
   its first stable rejection reason, actual value, and limit when it fails.
4. Use `estimate_generated_graph_work_units(max_block_size)` when presenting a
   budget before enforcing the limit.
5. Record the canonical `GraphSerializer::to_json` form and the source fixture
   identity. A canonical semantic form is not a resolved execution-plan
   fingerprint; keep those diagnostics separate.

Validation is a pure preflight. `prepare()` is the lifecycle operation that can
drop an old snapshot and publish a newly compiled one. Both are control-thread
operations. Never use graph import, serialization, limits, or preparation from
the audio callback.

## Prepare and publish a safe edit

For a running graph, prefer the existing `begin_prepared_topology_edit()` path:

1. Start a private candidate from the current prepared generation.
2. Apply additions, removals, connections, automation, feedback, or modulation
   changes to the candidate.
3. Prepare the candidate off the audio thread with the target sample rate and
   block size.
4. Commit only when preparation, limits, lifecycle, and latency policy accept
   the candidate; otherwise retain the current graph and record the refusal.

The transaction is atomic at the graph publication boundary. It does not promise
that every latency-changing or feedback edit is seamless. The documented
contract may intentionally require eager preparation and a bounded interruption.
Do not describe a refused edit as a partial success.

## Render the right offline surface

Use `OfflineSignalGraphHost` to prove that a prepared graph is invariant to
block partitioning. It drives the no-transport `SignalGraph::process()` path;
it does not provide timeline transport or tail injection.

Use the timeline offline renderer when notes, automation, tempo, pre-roll, PDC,
or tails depend on transport. Driving a timeline graph through
`OfflineSignalGraphHost` produces silence by contract because the nodes receive
no transport state.

For every fixture, record the sample rate, maximum block size, partition list,
input stimulus, serialized semantic form, output hash or audio-quality artifact,
and whether the path is graph-host or timeline-host. Compare live/offline output
only when both paths have the same declared transport contract.

## Inspect and diagnose

The existing library diagnostics are the first authority:

- `prepared_stats()` for compiled node, connection, port, work, and buffer facts;
- routing executor counters for serial/parallel path selection;
- graph authoring receipts and generation for publication identity;
- trace, logs, telemetry, and audio-quality artifacts through the canonical
  control registry where an application has an installed control adapter.

The CLI and MCP currently expose adjacent control capabilities, but not a
single SignalGraph topology inspect/validate/diff/render workflow. Do not infer
that capability from a built control component or a kit manifest. The current
limitation is tracked in [SignalGraph capability matrix](../reference/signal-graph-capabilities.md).

## Bake only after validation

When a graph is lowerable and trusted, use the existing bake path:

```sh
pulp bake myrack.pulpgraph -o myrack.pulpbake --sign-key signing.key
pulp bake verify myrack.pulpbake --trust signing.key
```

Bake is an artifact step after graph validation and preparation. It is not a
replacement for live graph editing, hosted-plugin lifecycle testing, or process
isolation. Hosted Plugin nodes and unsupported runtime-owned nodes remain subject
to the documented bake restrictions.

## Agent and Forge acceptance workflow

An agent-authored graph is ready to use only when one deterministic fixture can:

1. be discovered and structurally verified through the kit workflow;
2. load and validate limits before preparation;
3. prepare and render with a declared partition/transport contract;
4. apply one valid prepared mutation and reject one invalid mutation without
   disturbing the published graph;
5. emit a semantic JSON receipt, publication/generation receipt, diagnostics,
   and an audio-quality artifact; and
6. optionally bake and verify the resulting artifact when its node set is
   lowerable.

This is the smallest useful agent-facing contract. It uses existing graph,
control, kit, trace, and audio-quality infrastructure. A typed graph CLI/MCP
operation should be added only if this workflow demonstrates repeated manual
integration cost; any such operation must reuse the canonical control authority,
authorization, receipts, and artifact lineage.

## Capability gaps and status

See the [capability matrix](../reference/signal-graph-capabilities.md) for the
current split between library support, authoring/tooling exposure, evidence
confidence, and next acceptance gate. Update that matrix and the durable planning
ledger whenever a fixture, implementation, or no-change decision changes status.
