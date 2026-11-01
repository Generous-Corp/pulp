# SignalGraph capability matrix

This matrix separates four questions that are easy to conflate:

- **Runtime:** what the engine can execute or represent.
- **Authoring:** what a developer can construct and edit through the C++ model.
- **Tooling:** what the CLI, kits, and diagnostics expose today.
- **Evidence:** the fixture or receipt that supports the status claim.

A capability is only as broad as its narrowest lane. A runtime primitive does
not imply a public authoring workflow, and a kit manifest does not prove audio
execution. Use the [unified workflow guide](../guides/signal-graph-workflow.md)
for the step-by-step sequence.

## Choose a starting point

| Your question | Read these rows first |
|---|---|
| Can the runtime express this graph? | DAG composition, limits, prepared edits, feedback and latency/PDC |
| Can I author and edit it safely? | Prepared topology edits, serialization, and the graph fixture workflow |
| Can I render and compare it? | Offline graph rendering, timeline offline rendering, and the evidence rows |
| Can I host external processing? | Hosted plugin lifecycle, multi-bus routing, and process isolation |
| Can an agent operate it repeatably? | Fixture discovery, limits, receipts, inspection, and fingerprints |

## Runtime, authoring, tooling, and evidence

| Capability | Runtime | Authoring | Tooling | Evidence | Status / next gate |
|---|---|---|---|---|---|
| DAG composition and stable node identity | `SignalGraph` with built-in and custom nodes | C++ graph API and [reference](signal-graph.md) | Serializer and graph tests | [sample-region graph](../../examples/sample-region-allpass/allpass_graph.hpp) | Supported; add deterministic replay receipt |
| Limits and generated-graph preflight | `GraphLimits`, validation, work-unit estimate | Apply limits before `prepare()` | Serializer/import tests | [sample-region compatibility graphs](../../test/fixtures/sample-region-compat/graphs/) | Supported; retain rejection reason, actual, and limit in receipts |
| Prepared topology edits | Immutable prepared snapshots and publication generations | `PreparedTopologyEdit` transaction | Host bindings and lifecycle tests | [lifecycle reports](../reports/signal-graph-authoring-d1-tsan-20261015.md) | Supported with interruption policy; add latency, feedback, rollback, and continuity fixtures |
| MIDI, sparse automation, and audio-rate modulation | Typed event and modulation lanes | Graph edges and bounded lane configuration | Parameter/modulation docs and tests | [graph control fixture](../../test/fixtures/control_dspx04_graph_product_fixture.cpp) | Supported in bounded lanes; add capacity and sample-accurate parity receipts |
| Feedback and latency/PDC | Explicit feedback delay and prepared compensation | Feedback edges and latency declarations | Graph tests and prepared stats | [SignalGraph reference](signal-graph.md#latency--pdc) | Supported with declared limits; add deterministic replay and latency-change continuity |
| Serial and parallel execution | Canonical and levelized executors | Topology determines eligible scheduling | Runtime counters and diagnostics | [lifecycle receipt](../reports/signal-graph-lifecycle-a3-receipt-20261002.md) | Supported; benchmark before scheduler changes |
| Offline graph rendering | `OfflineSignalGraphHost` with no transport | Prepare a graph, then render fixed partitions | C++ tests | [sample-region example](../../examples/sample-region-allpass/) | Supported for no-transport partition parity; use timeline renderer for transport and tails |
| Timeline offline rendering | Timeline renderer supplies transport, pre-roll, PDC, and tails | Timeline project, clips, notes, and automation | Timeline CLI and [SDK guide](../guides/timeline-sdk.md) | [timeline phase example](../examples/timeline-phase1.md) and [timeline corpus](../../test/fixtures/timeline/corpus.index) | Supported at the higher level; preserve the direct graph-host boundary |
| Graph serialization and bake | `GraphSerializer` and signed bake artifacts | Author `.pulpgraph` and lowerable node sets | `pulp bake` and verification | [sample-region graph](../../test/fixtures/sample-region-compat/graphs/legacy-v2.pulpgraph) | Supported with node restrictions; retain semantic form and artifact lineage |
| Hosted plugin lifecycle | `PluginSlot` and plugin nodes | Wrap a configured `PluginSlot` in a graph node; `Processor` nodes use the owned processor-node path | Format adapters and bounded hosting fixtures | [hosting guide](../guides/hosting.md) | Partial; require a pinned format lifecycle matrix |
| Multi-bus routed `ProcessBuffers` | Direct rich buses; routed named secondary delivery is bounded | Declare and connect supported buses | Existing tests and routed fixture | [capability rows](capabilities.md) | Confirmed seam; add name preservation, mapping, and auxiliary-output receipts |
| Graph inspection and fingerprints | Authoring receipts and runtime diagnostics | Capture semantic and prepared generations separately | No unified graph CLI/MCP workflow | [reference diagnostics](signal-graph.md) | Partial/awkward; establish deterministic internal receipts before a public contract |
| Fixture discovery and structural validation | Kit metadata does not execute a graph | Declare fixture paths and metadata | `pulp kit search/inspect/validate/verify` | [sample-region kit](../../examples/sample-region-allpass/) | Partial; pair structural verification with an executable fixture receipt |
| Unified graph control workflow | Adjacent control operations exist | Discover → validate → prepare → mutate → render → diagnose | CLI/MCP/control docs cover adjacent surfaces | [workflow guide](../guides/signal-graph-workflow.md) | Partial/awkward; keep the guide as the canonical integration path |
| Process isolation for plugin crashes | Scanner containment is separate from graph execution | No graph-native isolation contract | Hosting and diagnostic surfaces | [hosting guide](../guides/hosting.md) | Unverified/artifact-gated; require pinned crash/hang and isolated-process receipts |
| Agent/Forge graph generation | Limits, serializer, edits, and receipts are reusable | Generate a bounded graph fixture | Kits, controls, and audio-quality tooling | [agent acceptance section](../guides/signal-graph-workflow.md#agent-and-forge-acceptance-workflow) | Semantically possible; add one deterministic fixture with positive and negative controls |

## Reading the status

**Supported** means a current source or test receipt covers the named boundary;
it does not imply every format, platform, or graph shape. **Partial/awkward**
means the engine or adjacent pieces exist but the unified authoring or debugging
path is incomplete. **Confirmed seam** means a deterministic fixture proves a
specific bounded behavior. **Unverified/artifact-gated** means the required
external identity or platform evidence is absent. Do not collapse these states
into a broad parity claim.

The matrix is subordinate to the durable audio-graph planning ledger. When a
receipt changes a row, update `graph-update.md` with the exact source head,
commands, positive and negative controls, confidence, and next action.
