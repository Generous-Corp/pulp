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
| Study a complete example | [Sample granular graphs](sample-granular-graphs.md) | The example's structural and executable receipts |
| Build editable routing or hosted composition | [Choose the graph boundary](#choose-the-graph-boundary) | A prepared graph and one executable render |
| Turn an authored or generated graph into audio | [Load and validate before preparation](#load-and-validate-before-preparation) | Limits, preparation, partition contract, and output receipt |
| Change a running graph | [Prepare and publish a safe edit](#prepare-and-publish-a-safe-edit) | Valid edit acceptance and invalid edit rollback |
| Diagnose or reproduce a graph | [Inspect and diagnose](#inspect-and-diagnose) | Semantic receipt, generation receipt, diagnostics, and audio artifact |
| Automate graph work as an agent | [Agent and Forge acceptance workflow](#agent-and-forge-acceptance-workflow) | One deterministic fixture with positive and negative controls |

Humans can read the sections top to bottom. Agents should use the table as a
dispatch map, then follow the selected section and record the artifacts named
there. The [capability matrix](../reference/signal-graph-capabilities.md) is the
compact status view; this guide is the operational sequence.

## Choose the graph boundary

Start with [Processing models](../reference/processing-models.md). Keep fixed
DSP, voice state, and sample processing in `Processor`. Use
`pulp::host::SignalGraph` when the routing or hosted composition is the thing
the application edits: a rack, mixer, modular patch, plugin host, or node
editor. A `Processor` can own a prepared graph through the existing binding;
that ownership does not change the Processor/SignalGraph boundary.

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
