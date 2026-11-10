# G-6 remaining runtime lane — exact-head HOLD (2026-11-09)

## Disposition

**HOLD / NO-CHANGE.** This lane starts from Pulp `origin/main` at
`a3df0dc6e3dca64b601e67f0cd30e3d0dfd51f67` in a fresh classified worktree.
It adds no graph, voice, scheduler, Processor, or audio runtime code. It does
not modify `docs/graph-program-status/G-6.md`, duplicate the admitted G-5
event lane, or claim A1/G-6 completion.

The smallest safe G-6 successor is still a reviewed G-4 Processor voice
binding followed by one fixed eight-voice public consumer and its audio/tail
qualification. The current public surfaces do not supply that binding, so an
allocator wrapper or graph voice API in this lane would invent a second
contract.

## Exact-head dependency readback

### G-4 authoring and persistence remain control-side

`core/host/include/pulp/host/voice_template.hpp:1-5` describes the schema as
control-side and explicitly says it does not create graph nodes, schedule
events, or implement voice runtime. `VoiceBankState` at lines 77-83 persists
only finite per-slot parameters and explicitly cannot represent active notes,
generations, pedals, epochs, queues, or release tails. The live G-4 status
(`docs/graph-program-status/G-4.md:5-7,78-86`) keeps graph voice runtime and
the A1 audio/tail oracle at HOLD.

The existing positive and planted-negative controls are useful prerequisites,
but they do not cross that boundary:

* `test/test_voice_template.cpp:82-96` proves eight isolated bindings and a
  stable manifest digest.
* `test/test_voice_template.cpp:112-186` proves transactional persistence,
  the 4096-record bound, and unchanged state on malformed/over-limit input.
* `test/test_voice_template.cpp:188-230` refuses aliases, shared bindings,
  type/rate/shape mismatches, forged identity, and invalid migration.

None of these tests instantiate a graph Processor or bind a slot identity to a
voice allocator.

### G-5 event transport is admitted, but it is not a voice lifecycle

The exact head includes the bounded `GraphAuthoredEventLane` and callback-local
`GraphEventWriter`/`GraphEventReader` in
`core/host/include/pulp/host/graph_event_contract.hpp:426-845`. Prepared graphs
retain one lane (`core/host/src/signal_graph_prepare.cpp:452-456`), and the
routed executor supplies the producer writer or consumer reader only for the
declared edge (`core/host/src/signal_graph_executor_routing.cpp:213-239`).
This closes the former G-5 wiring gap; it does not declare a voice slot,
allocator owner, sustain state, or audio tail.

The current G-5 controls are explicit and remain bounded:

* `test/test_g5_host_event_contract.cpp:431-444` proves partition-invariant
  event tuples and stable sequence ordering.
* `test/test_g5_host_event_contract.cpp:446-474` plants stale-epoch, future,
  late, overflow, and attempt-limit refusals.
* `test/test_g5_host_event_contract.cpp:497-531` proves one-block lookahead
  delivery and pending-ring drain; lines 533-551 prove horizon refusal and
  reset retirement.
* `test/test_g5_host_event_contract.cpp:553-569` proves the borrowed writer /
  reader workflow; lines 571-638 cover per-sample budgets, phase refusal, and
  recovery.

These records carry bounded payloads, not note ownership. They do not supply
CC64/CC120/CC121/CC123 pedal semantics, stale-generation voice dispatch, or a
finite audio tail oracle.

### Existing MPE allocator evidence remains a helper characterization

`core/midi/include/pulp/midi/mpe_synth_voice.hpp` provides a preallocated
`MpeVoiceAllocator` with four steal policies and generation-aware expression
dispatch. Existing tests cover note routing, retrigger retirement, deferred
note-off, per-note expression isolation, and allocation-free dispatch
(`test/test_mpe_synth_voice.cpp:147-337`). The installed consumer receipt
reported by G-6 (`PASS checks=160`) characterizes this helper only; it does not
bind it to a graph slot, graph event reader, or Processor output.

## Missing admission gates

The following controls are required before a G-6 runtime implementation is
authorized and are absent at this exact head:

1. **Reviewed Processor voice binding:** canonical G-4 slot identity must map
   to one prepared, isolated Processor voice namespace without runtime-node or
   insertion-order identity.
2. **Voice/event lifecycle:** graph-delivered note-on/off and MPE expression
   must preserve generation checks, same-pitch retriggers, and all four steal
   policies across block partitions.
3. **Pedal/reset semantics:** CC64 threshold and deferred release, CC120/121/
   123 scoped cleanup, epoch reset, and stale-target refusal need positive and
   planted-negative controls.
4. **Audio and tail oracle:** one fixed eight-voice consumer must render finite,
   nonzero output, isolate voices, and prove release-tail drain and silence
   under split blocks.
5. **Realtime and public-consumer gates:** the exact head needs governed
   callback probes plus a clean installed-SDK consumer. A source-level
   preallocation assertion or the existing allocator receipt cannot qualify
   callback deadlines or platform parity.

Until these gates are admitted, adding a second scheduler, an allocator bridge,
or a graph voice wrapper would duplicate G-5 or invent an unreviewed G-4
binding. The next safe action is to review and admit the G-4 Processor voice
binding contract, then build the fixed-voice consumer and its mutation-backed
audio/tail receipt in a separate lane.

## Validation boundary

This is an evidence-only hold. The fresh worktree was clean at the exact base;
no build directory or runtime process was present, so no build command was
run. The positive and negative controls above are existing committed tests and
receipts; they are not reclassified as G-6 runtime qualification. No PR is
armed, enqueued, or merged by this lane.
