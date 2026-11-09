# G-5 graph-authored event follow-up contract — 2026-11-09

**Disposition: PLANNING ONLY — implementation is held; G-5 remains PARTIAL.**

This packet is a follow-up contract for the next G-5 slice.  It does not alter
`docs/graph-program-status/G-5.md`, add a runtime API, or claim that a graph
authored producer/consumer route exists.  The source authority for this audit
is Pulp `origin/main` at
`dab7cef9ddd3ae0b276ca0e932fa9db3af4051ba`.  The host-ingress successor is
already on that head (`c6ffe31023d76bfbf57937bc4648b76fae3b9aa7`, with the
window-size repair `94a5cf534a9f`); this packet does not duplicate that seam.

The proposed first implementation is one named, acyclic graph-authored event
edge between one producer Custom node and one consumer Custom node.  It uses
the graph's existing serial sample clock and fixed prepared storage.  It does
not add a host scheduler, a worker queue, a second executor, plugin voice
delivery, fan-out, feedback, or an open-ended callback registry.

## Source and receipt readback

The following current-head facts were verified in the fresh worktree before
writing this packet:

| Surface | Current evidence and boundary |
| --- | --- |
| `core/host/include/pulp/host/graph_event_contract.hpp` | `HostEventIngress<256>` projects absolute host records into preallocated MIDI/UMP/MPE sidecars. `GraphAuthoredEventLane` has explicit producer/consumer phases, 16 attempts/8 accepted records per sample, epoch checks, and fixed storage, but returns `Future` for every target after the current sample and has no retained cross-block queue. |
| `core/host/include/pulp/host/trigger_graph.hpp` and `core/host/src/trigger_graph.cpp` | The admitted typed pair is a built-in trigger producer/envelope. `TriggerContext::emit_at_sample()` refuses future targets. `prepare_trigger_plan()` requires one pair, one edge, serial canonical routing, and no anticipation/parallel mode; hosted plugins remain refused. |
| `core/host/include/pulp/host/custom_node_events.hpp` | `CustomNodeEventBlock` carries MIDI input/output and a `TriggerContext*`, but the normal routed and reference custom-event bindings construct it without a trigger context. The built-in trigger binding replaces the producer callback and removes the envelope node, so it is not a general graph-authored callback route. |
| `core/host/src/signal_graph_executor_routing.cpp` and `core/host/src/signal_graph_reference_walk.cpp` | Both paths resolve `process_events`, but their event blocks currently carry only MIDI. The typed trigger path is admitted only through the canonical serial executor; an equivalent graph-authored route is not proven on the reference walk. |
| `core/host/src/trigger_graph.cpp` host ingress | `begin_host_event_block()`/`publish_host_event()`/`project_host_events_()` bind host absolute records to the active epoch and existing MIDI mailbox. The adapter is bounded and fail-closed; it is a host ingress path, not graph-authored scheduling. |
| Existing receipts | `docs/evidence/g5-host-event-projection-20261106.md` (source fix `d17bdfe425ad`) and `docs/evidence/g5-host-event-installed-consumer.md` (consumer source `407069a15b28830c02b869ec8b686b5dcd317acc`) are historical contract evidence. They explicitly leave graph-authored callback routing, queued cross-block semantics, backend parity, host scheduling, plugin parity, and hard-realtime qualification open. Refreshing them at `dab7cef9` is a later evidence task. |

The source audit also found three constraints that the next design must retain:

1. `HostEventIngress::less()` orders equal-time host records by producer then
   producer sequence, while `GraphAuthoredEventLane` currently has only one
   producer and orders by its minted sequence.  The follow-up must define one
   tie key for both lanes; insertion order is not an identity.
2. `GraphAuthoredEventLane` has a fixed trace arena but no pending future-event
   storage.  Allowing `Future` without a bounded queue would either retime an
   event or silently lose it; neither is admissible.
3. A `CustomNodeEventProcessFn` is block-shaped.  Giving an arbitrary callback
   an implicit mutable sample cursor would make partition behaviour depend on
   callback internals.  The proposed writer therefore takes an explicit
   absolute sample target and the reader exposes sorted block records.

## Smallest contract and API direction

The implementation PR may choose names that fit existing vocabulary, but it
must preserve this shape and refusal behaviour.

### Authoring and admission

Add one control-side, opt-in event edge declaration (or extend the existing
typed edge declaration) with exactly these roles:

* one `producer` Custom node with a resolved `process_events` callback;
* one `consumer` Custom node with a resolved `process_events` callback;
* one event kind and one source/destination port (port zero in the first
  slice); and
* a feed-forward graph path with no event fan-out, event feedback, or cycle.

The declaration is a graph property, not a callback registration mechanism.
Unbound Custom nodes keep their existing MIDI-only `CustomNodeEventBlock` and
receive a null graph-event handle.  A node without the required callback,
with a callback that is also transport-sensitive, or with a plugin/hosted
binding is refused before candidate publication.  `NodeId` is the runtime
producer identity; persistence or authored-name identity is deferred to the
G-4 identity work and must not be inferred from insertion order.

The public callback surface should remain one block callback with an explicit
opaque writer/reader pair, conceptually:

```text
GraphEventWriter::emit_at(epoch, absolute_sample, payload) -> GraphEventResult
GraphEventReader::events() -> span<const GraphAuthoredEvent>
```

The writer mints the producer-local sequence.  A caller cannot supply a
sequence, forge an epoch, or retain the writer after the callback.  The reader
is valid only during the consumer callback and returns records for that block,
sorted by `(absolute_sample, producer_id, producer_sequence)`.  This is an API
direction for review, not a request to add these declarations in this packet.

### Event coordinates and queued cross-block policy

The graph owns one monotonic unsigned 64-bit sample cursor per activation
epoch.  A render owns `[S, S + N)` with `1 <= N <= 4096`; checked addition
refuses cursor exhaustion before any output, queue, or state mutation.

The first queued contract uses one fixed look-ahead horizon of `MaxFrames` and
one fixed pending ring charged to the existing 2 MiB trigger budget:

| Target | Result and delivery |
| --- | --- |
| `S <= target < S + N` | Accepted into the current block. The consumer sees it in `events()` at its absolute sample. |
| `S + N <= target < S + N + MaxFrames` | Accepted as queued. It is retained in the pending ring and delivered exactly once in the first later block whose window contains it. A target exactly at `S + N` belongs to the following block. |
| `target < S` or an already-consumed target in the active block | `Late`; no queue, reader, output, or counter state is changed except the refusal counter. |
| `target >= S + N + MaxFrames` | `Future`/out-of-horizon; no queue or output mutation. The caller may retry with a later block. |
| epoch differs or the writer is unbound | `StaleEpoch`/`Unbound`; no mutation. |

The pending ring is bounded before publication.  The implementation must report
`Overflow` when it is full and retain no partial prefix.  Per canonical sample,
the existing 16-attempt/8-accepted limit remains unchanged; attempts 1–16
consume sequence identity even when invalid or overflowing, and the latched
17th attempt does not.  Counters for attempts, accepted, overflow, late,
future, and diagnostics are saturating and reset only with the epoch.

At block start, a queued record is selected by the same total order as a
current-block record.  A host seek or discontinuity that leaves a queued
record behind the new `S` retires that record as `Late` and reports it; it is
never retimed to offset zero.  Pending records never migrate across reset or
prepared-generation replacement.

### Reset, swap, and lifecycle

Reset and validated replacement are control-boundary operations between
completed renders.  They increment the epoch, clear the pending ring and
per-sample counters, reset producer/consumer state, and invalidate borrowed
writer/reader handles.  A reset or replacement while a callback or ingress
projection is active returns `Busy` without mutation.  Epoch exhaustion returns
`ClockExhausted` without clearing the current generation.  The old generation
must remain renderable until the new candidate is atomically published, as in
the existing trigger-plan lifecycle.

The host absolute-event adapter remains separate: its caller supplies an
epoch/sample record before `process_checked()`, and the existing projection
maps it into MIDI/UMP/MPE.  The graph-authored writer is audio-callback local;
it does not read wall-clock time, host timestamps, beat time, or a control
thread queue.  Any such projection needs a separately named adapter contract.

## Ownership and dependencies

| Owner | Required responsibility | Dependency or boundary |
| --- | --- | --- |
| Graph/host runtime owner | Implement the single edge declaration, fixed pending ring, epoch/reset lifecycle, and writer/reader binding in the prepared snapshot. | Reuse `GraphAuthoredEventLane`, trigger resource accounting, and the existing serial graph clock. Do not add a second scheduler. |
| Routed executor owner | Pass the writer to the producer callback and the sorted reader to the consumer callback with fixed, snapshot-owned storage. | `signal_graph_executor_routing.cpp` must preserve callback lifetime and refuse unsupported binding modes before dispatch. |
| Reference-walk owner | Run the same producer/consumer callbacks and event ordering in the legacy walk, or return the same explicit `Mode` refusal before either backend renders. | A canonical-only green test is not backend parity. `CustomNodeEventBlock` currently omits graph-event handles on this path. |
| DSP/instrument owner | Supply one public producer/consumer fixture with finite payload semantics, exact sample tuples, reset behaviour, and a downstream modulation or envelope observation. | Do not use a plugin or MPE voice allocator in the first event slice. G-6 consumes this contract only after its own voice/epoch contract is admitted. |
| Maintainer/public-surface owner | Keep the role declaration closed and versioned, preserve aggregate/source compatibility where possible, refresh the capability inventory, and produce an installed-SDK consumer. | No public callback registry, arbitrary function pointer escape, private-header dependency, or insertion-order identity. |
| Realtime/CI owner | Run allocation/lock/I/O mutation probes, RTSan where available, bounded deadline runs, and both-backend receipts at the exact head. | A declared `CustomNodeRtContract`, local Release build, or green focused unit test is not hard-realtime proof. |
| G-4/G-6/G-7/G-9 owners | Keep identity, voice lifecycle, probes, and hosted-plugin isolation as named dependencies. | G-4 authored identity and G-6 per-note delivery remain downstream. G-7 probes are optional evidence helpers. G-9/plugin isolation is explicitly out of scope. |

## Evidence gates for a future implementation PR

All gates must be present at one exact source head; historical receipts do not
count for a changed public header or binding.

1. **In-tree positive:** one graph-authored producer/consumer pair renders a
   known finite event-to-envelope or modulation path. The receipt records
   `(epoch, absolute_sample, producer_id, sequence, payload)` tuples and the
   audio oracle for one block and split partitions `1, 32, 64, 127, 256`.
2. **Queued semantics:** the same fixture emits a target at the next block
   boundary, a target inside the one-block horizon, and an out-of-horizon
   target. It proves exactly-once delivery, no early delivery, queue overflow,
   late-on-seek refusal, reset clearing, and epoch exhaustion. A block-size
   change must not retime or duplicate an event.
3. **Tie and refusal negatives:** planted controls reverse the tie comparator,
   accept a stale epoch, accept a consumed/late target, deliver a future target
   early, bypass the 16/8 per-sample cap, and clear the pending ring without
   advancing the epoch. Each mutation must fail a focused oracle and pass again
   after exact source restoration. `confirm_failure.sh` is the required
   mutation harness; a prose-only negative is insufficient.
4. **Public consumer:** a clean external project builds against the freshly
   installed Release SDK using only public headers. It executes the same event
   trace through both the routed serial executor and the reference walk (or
   records the same explicit refusal from both). A private-header compile
   attempt must fail, and a wrong-output or shifted-sample mutation must return
   nonzero.
5. **Backend parity:** the two backends produce byte-identical event tuples and
   equal audio within the fixture's declared numeric tolerance, including the
   queued boundary and reset. Parallel and anticipation modes remain refused;
   no parity claim may be inferred from the canonical path alone.
6. **Realtime:** the installed callback is exercised at 64/128/256/4096-frame
   blocks with no allocations, locks, waits, filesystem/network I/O, or
   unbounded retries. Allocation, mutex, and write mutations must be caught by
   the runtime probe/RTSan where supported. Record callback deadline and
   p99/jitter data separately from functional correctness; do not promote a
   single local timing run to hard-realtime qualification.
7. **Receipt provenance:** record source SHA, origin/main base SHA, public SDK
   prefix and sorted manifest hashes, binary hash, build profile, backend mode,
   block partitions, focused assertion/test counts, every planted-negative exit
   code, and all unavailable lanes as `NOT CHECKED`, `N/A`, or `BLOCKED`.

## Adversarial review

**DSP review.** Explicit absolute targets prevent a callback from smuggling a
hidden block-relative clock. Total ordering by sample, producer identity, and
sequence is deterministic under ties. The review rejects retiming, implicit
interpolation, non-finite payloads, denormal-dependent state, and feedback
edges in this slice. Partition invariance must compare both event tuples and
audio, not only a final checksum.

**Instrument review.** A queued trigger is not a note lifecycle. MPE note
ownership, pedal deferral, stale-generation handling, and per-note reset stay
with G-6. The first G-5 consumer must use a bounded envelope/modulation
observation; it must not claim voice delivery or plugin MIDI parity merely
because the host-ingress MPE sidecar reaches an in-process MIDI lane.

**Maintainer review.** An open-ended `std::function` event registry, a second
scheduler, or a callback-visible mutable global would expand the public ABI and
break the capability/installed-consumer boundary. The role declaration must be
closed, prepare-stable, and versioned. Public-header changes require a refreshed
capability inventory and exact installed-consumer receipt. This packet adds no
roadmap status line and must not be used to mark G-5 ready or complete.

## Kill criteria and do-not-build boundaries

Stop and return to contract review if any of the following is required:

* more than one producer or consumer, fan-out, event feedback, or plugin
  callback in the first slice;
* an unbounded queue, a queue whose capacity depends on host block history, or
  a refusal that retimes/drops an event without a counted reason;
* a second scheduler, worker thread, wall-clock timer, or host-thread callback
  used to make graph-authored delivery happen;
* backend-specific event semantics, a canonical-only implementation, or a
  test that cannot run the public consumer against the installed SDK;
* a hard-realtime claim based only on a Release build, allocation-free source
  inspection, one callback timing sample, or a declared RT contract;
* plugin/MPE voice parity, G-6 pedal/allocator behaviour, serialization, or
  authored identity being inferred from this event lane;
* source edits that duplicate the existing host-ingress successor seam or
  modify `docs/graph-program-status/G-5.md` to hide its PARTIAL boundary.

Until every evidence gate is satisfied, the safe disposition is **PARTIAL /
implementation HOLD**.  This packet is the smallest reviewable direction for
the next source slice; it is not runtime admission.
