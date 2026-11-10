# G-4/G-5 qualification boundary audit — 2026-11-10

**Disposition: HOLD / evidence only. No new runtime or public API was added.**

This audit was performed in a fresh classified worktree at exact
`origin/main` / `250387e23a483c6c65562111707923a57369aa0b`.
It reconciles the G-4 and G-5 status claims with the implementation that is
actually present at that head. It does not promote either lane to COMPLETE and
does not duplicate the existing graph-event implementation or voice work.

## Verified current implementation

### G-4

The current source contains the bounded authoring surfaces described by the
status files:

- `SubgraphTemplate` owns named nodes, feed-forward edges, exposed ports,
  finite parameter declarations, and recursively flattened child templates
  (`core/host/include/pulp/host/signal_graph_subgraph.hpp:62-69`).
- `instantiate_subgraph` and `connect_subgraph_ports` validate declarations
  before mutation and retain instance-local identity/index and state
  (`core/host/include/pulp/host/signal_graph_subgraph.hpp:71-127`).
- `VoiceBankState` persists only the manifest identity and finite per-slot
  parameter values; the public contract explicitly excludes active notes,
  generations, pedals, epochs, queues, and release tails
  (`core/host/include/pulp/host/voice_template.hpp:79-110`).
- Source tests cover eight-slot expansion, identity stability, transactional
  restore, nested subgraphs, port composition, and planted rejection controls
  (`test/test_voice_template.cpp:82-253`,
  `test/test_signal_graph_subgraph.cpp:62-357`,
  `test/test_voice_identity.cpp:20-94`).

Those are control-side authoring, identity, and persistence guarantees. There
is still no reviewed binding from a G-4 voice instance to a graph `Processor`
voice callback, no note/generation/tail lifecycle, and no positive rendered
A1 voice-bank consumer with an audio/tail oracle.

### G-5

The graph-authored event callback wiring is present on this current head. The
merged implementation provides callback-local borrowed writer/reader handles in
`CustomNodeEventBlock` (`core/host/include/pulp/host/custom_node_events.hpp:56-77`).
The routed executor opens the producer or consumer phase and passes the handle
(`core/host/src/signal_graph_executor_routing.cpp:206-240`), and the reference
walk does the same (`core/host/src/signal_graph_reference_walk.cpp:600-630`).
The admission path rejects unsupported roles, missing forward audio paths,
feedback, fan-out, and transport conflicts before publication.

The source tests verify a producer/consumer callback, equal output through both
routed and reference execution, disconnect cleanup, role/path refusals, and
parallel-routing refusal (`test/test_trigger_graph.cpp:133-288`). The bounded
lane tests also verify partition invariance, epoch/stale/late/future refusal,
per-sample attempt/accept caps, one-block look-ahead, reset retirement, abort
recovery, and deterministic producer/sequence ordering
(`test/test_g5_host_event_contract.cpp:431-639`).

These are strong in-tree contract and wiring controls. They do not establish
all roadmap gates: the current receipt set has no clean external installed-SDK
consumer for the graph-authored callback route at this exact head, no mutation
proof covering both backends and the public consumer, and no measured
allocation/lock/I/O/deadline matrix. The one-block lane also intentionally
refuses targets beyond its bounded horizon; it is not a general scheduler or
voice/event lifecycle.

## Explicit remaining gates

| Requirement | Current disposition | Evidence still required |
| --- | --- | --- |
| G-4 nested authoring and stable identity | Verified bounded slice | Exact-head installed consumer or equivalent public fixture if the roadmap requires an external seam |
| G-4 Processor voice binding | **HOLD** | Reviewed declaration/instance binding, stale-generation controls, and prepared graph publication evidence |
| G-5 graph-authored producer/consumer wiring | Verified in-tree bounded slice | Exact-head public installed consumer exercising the same route |
| G-5 backend parity | **HOLD** | Routed and reference byte-identical event tuples and audio under split partitions, including reset/queued boundary |
| G-5 realtime qualification | **HOLD** | 64/128/256/4096-frame callback probes with allocation, lock, wait, I/O, deadline, and p99 evidence |
| A1 flagship | **HOLD** | Fixed eight-voice external consumer with note lifecycle, steal policy, MPE/pedal ordering, tails, persistence, and positive audio oracle |

The existing G-5 follow-up contract remains the correct successor boundary:
exactly one producer and consumer, a closed role declaration, a fixed pending
ring, explicit epoch/sample coordinates, and no second scheduler. The existing
G-6 HOLD remains authoritative for the dependency order: admit the G-5 sidecar
and a reviewed G-4 Processor voice binding before implementing graph polyphony.

## Validation boundary

This audit did not claim a build or runtime pass. No build directory was present
in the fresh worktree, so focused test execution, installed-SDK packaging, and
realtime probes are **NOT CHECKED** here. Existing source-test names and line
references above are implementation evidence only; they are not substituted for
an exact-head executable receipt.

The safe next action is a single reviewed G-4 Processor voice-binding slice or a
single exact-head G-5 public-consumer slice, followed by the A1 oracle. Adding a
second scheduler, a general callback registry, or a standalone allocator wrapper
before those dependencies are admitted would duplicate existing seams and is
out of scope for this HOLD.
