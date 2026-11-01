# SignalGraph capability matrix

This matrix describes what a developer can use today, where the capability is
exposed, and what evidence is still required. It intentionally separates graph
engine semantics from authoring and debugging surfaces.

## Choose a starting point

| Your question | Read these rows first |
|---|---|
| Can the runtime express this graph? | DAG composition, limits, prepared edits, feedback and latency/PDC |
| Can I build and edit it safely? | Prepared topology edits, graph fixture discovery, serialization and bake |
| Can I render and compare it? | Offline graph rendering, timeline offline rendering, graph inspection and fingerprints |
| Can I host external processing? | Hosted plugin lifecycle, multi-bus routing, process isolation |
| Can an agent operate it repeatably? | Agent/Forge generation, limits, fixture discovery, inspection and fingerprints |

Use the [unified workflow guide](../guides/signal-graph-workflow.md) for the
step-by-step path. This page answers “is it supported and how certain are we?”;
the guide answers “what do I do next?”

| Capability | Library/runtime | Existing developer surface | Current status | Next acceptance gate |
|---|---|---|---|---|
| DAG composition and stable node identity | `SignalGraph`, built-in/custom nodes | C++ API, graph reference | Supported | Deterministic serialization and replay fixture |
| Limits and generated-graph preflight | `GraphLimits`, `validate_generated_graph`, work-unit estimate | C++ API, serializer/import tests | Supported | Keep limit reason/actual/limit in receipts |
| Prepared topology edits | `PreparedTopologyEdit` | C++ API, host bindings | Supported with interruption policy | Latency-changing, feedback, rollback, and continuity fixtures |
| MIDI, sparse automation, audio-rate modulation | Graph edges and typed event/modulation lanes | C++ API, parameter/modulation docs | Supported in bounded lanes | Capacity, sample-accurate, and hosted-format parity |
| Feedback and latency/PDC | Explicit feedback delay and prepared PDC | C++ API, graph tests | Supported with declared limits | Deterministic replay and latency-change continuity |
| Serial/parallel execution | Canonical and levelized executors | C++ controls and diagnostics | Supported; optimization remains measured | Benchmark before any scheduler change |
| Offline graph rendering | `OfflineSignalGraphHost` | C++ API/tests | Supported for no-transport partition parity | Keep transport/tail on timeline renderer |
| Timeline offline rendering | Timeline renderer with transport/pre-roll/tails | C++ API/CLI surfaces | Supported at higher-level timeline | Preserve direct graph-host boundary |
| Graph serialization and bake | `GraphSerializer`, `pulp bake` | C++ API and CLI | Supported with node restrictions | Receipt semantic form and artifact lineage |
| Hosted plugin lifecycle | `PluginSlot` and plugin nodes in a graph | Format adapters, X9 fixture | Partial; arbitrary artifact breadth is gated | Pinned CLAP/VST3 lifecycle matrix |
| Multi-bus routed ProcessBuffers | Direct rich buses supported; routed named secondary delivery incomplete | Existing tests and bounded fixture | Confirmed routed seam | Named-name preservation, bus mapping, Aux output phase |
| Graph inspection and fingerprints | Authoring receipt and diagnostics exist; semantic and plan diagnostics are separate | Library diagnostics; no unified graph CLI/MCP | Partial/awkward | Deterministic internal receipts before any public contract |
| Graph fixture discovery and structural validation | Kit manifest/fixture verification | `pulp kit` commands | Partial; structural only | Pair structural verification with executable fixture receipt |
| Unified graph control workflow | Control registry covers adjacent state/render/diagnostic operations | CLI/MCP/control docs | Partial/awkward | Canonical discover→validate→prepare→mutate→render→diagnose guide |
| Graph topology CLI/MCP mutation | No generic graph operation | Existing typed control authority only | Deliberately deferred | Add only after repeated manual integration is measured |
| Process isolation for plugin crashes | Scanner containment exists; graph-native isolation is separate | Hosting/diagnostic surfaces | Unverified/artifact-gated | Pinned crash/hang artifact and isolated-process receipt |
| Agent/Forge graph generation | Limits, serializer, prepared edits, receipts are reusable | Kit, graph, control, audio-quality tooling | Semantically possible; workflow exposure partial | X9-like deterministic fixture with rejection and lineage artifacts |

## Reading the status

**Supported** means a current source/test receipt covers the named boundary. It
does not imply every format, platform, or graph shape. **Partial/awkward** means
the engine or adjacent pieces exist but the unified authoring or debugging path
is incomplete. **Confirmed seam** means a current deterministic fixture proves a
specific missing behavior. **Unverified/artifact-gated** means the required
external identity or platform evidence is absent. These states must not be
collapsed into a broad parity claim.

The matrix is subordinate to the durable audio-graph planning ledger. When a
receipt changes a row, update `graph-update.md` with the exact source head,
commands, positive and negative controls, confidence, and next action.
