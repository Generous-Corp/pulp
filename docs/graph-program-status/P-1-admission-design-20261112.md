# P-1 bounded cross-block event projection: admission design

Status: **CONDITIONAL HOLD / design packet only**  
Source baseline: `origin/main` `a6874494141c4a86e8a1b4b3cce8801357694ef9`  
Authority: `docs/reference/2026-11-12-post-roadmap-audio-graph-capability-gaps.md:211-244`

This packet is an admission decision, not implementation authorization. It
does not add a runtime API, queue, scheduler, graph edge, or public consumer.
P0 admission does not by itself authorize P1: the authority requires one
installed consumer that proves four typed families across two block boundaries,
including voice intent.

## Verified current seams

* `pulp::host::HostEventIngress<256>` in
  `core/host/include/pulp/host/graph_event_contract.hpp:30-65,66-123`
  owns epoch-local absolute samples, producer/sequence identity, late,
  stale-epoch, invalid-sequence, overflow, and bounded-capacity refusals.
  `project()` is transactional and allocation-free
  (`graph_event_contract.hpp:125-191`); future records remain staged until a
  later block.
* `pulp::host::GraphAuthoredEventLane` is in the same header after the host
  ingress contract. Its fixed arena and producer/consumer phases are already
  the graph-authored trigger seam; it must not be wrapped in a second generic
  event bus.
* `pulp::state::ParameterEventQueue` has fixed capacity 1024 and
  block-relative `sample_offset` values (`core/state/include/pulp/state/parameter_event_queue.hpp:34-39,82-125`).
  It has stable ordering and overflow telemetry, but no epoch or cross-block
  absolute coordinate. `ModulationEventQueue` is a separate 1024-entry typed
  queue (`parameter_event_queue.hpp:41-74`).
* `pulp::host::TriggerContext::emit_at_sample` exposes absolute-sample trigger
  publication and bounded refusal results (`core/host/include/pulp/host/trigger_graph.hpp:19-59`),
  but its construction is private and public custom nodes do not receive it,
  so it was refused as a consumer seam. The graph-authored trigger lane is
  `CustomNodeType::process_events` with `SignalGraph::connect_graph_event`,
  `GraphEventWriter::emit_at`, and `GraphEventReader::events`
  (`core/host/include/pulp/host/custom_node_events.hpp`,
  `graph_event_contract.hpp`).
* `SignalGraph::ExecutionSnapshot` exposes block-local MIDI/UMP/MPE injection
  and parameter injection (`core/host/include/pulp/host/signal_graph_execution_snapshot.hpp:33-49`).
  The existing mailbox expects block-relative events and is not evidence of
  cross-block scheduling by itself.
* The installed G-5 consumer already demonstrates host ingress, graph-authored
  ordering, and MIDI/UMP/MPE projection (`examples/g5-host-event-consumer/main.cpp:544-600`;
  `examples/g5-host-event-consumer/README.md`). It does not cover parameter,
  trigger, or voice-intent as one cross-block consumer.
* G-4/G-6 provide value-owned voice identity, binding, and fixed eight-slot
  control-side state, but the status authority explicitly keeps graph voice
  runtime and lifecycle qualification open (`docs/graph-program-status/G-4.md:5-7,78-86`;
  `docs/graph-program-status/G-6.md:1-30,126-143`).

* The parameter mailbox row is **UNAVAILABLE** for a plain
  `add_processor_node()` `Processor`, which receives no parameter events
  without the private exact-parameter claim path. It is checked instead
  through a `PluginSlot` node added with `add_plugin_node`. See
  `docs/evidence/p1-event-admission-consumer-20261112.md:93-364` and the
  receipt's control disposition at
  `docs/evidence/p1-event-admission-consumer-20261112/receipt.json:8-156`.
* Public `TriggerContext` delivery is **UNAVAILABLE** and was refused as a
  seam: ordinary public custom-node callbacks do not receive a trigger
  context, whose construction is private. The trigger row is satisfied
  through the graph-authored trigger lane instead.

## Admission contract

The smallest defensible P1 experiment is a single installed-SDK consumer that
coordinates existing typed sidecars. It must preserve each sidecar's own
capacity and refusal policy while projecting them through one test-owned epoch
and absolute sample origin. The consumer may use a control-side adapter to
translate absolute positions to block-relative offsets; the audio callback may
only consume preallocated records and must not allocate or mutate topology.

Required positive controls, each spanning at least two blocks and at least two
partition schedules (for example 1/32/64/127/256), are:

1. MIDI/UMP/MPE note and expression events through the existing ingress and
   `inject_midi` path.
2. A parameter event through `ParameterEventQueue` and the existing parameter
   mailbox into a `PluginSlot` node, with stable equal-offset ordering and
   overflow telemetry.
3. A trigger emitted at an absolute sample through the graph-authored trigger
   lane (`GraphEventWriter::emit_at` to a `connect_graph_event` consumer),
   including current-block and bounded-future delivery. Public
   `TriggerContext` was refused as a seam.
4. A voice-intent record represented by an already reviewed G-4/G-6 binding
   identity and a fixed-slot control-side record. It must prove note-on,
   note-off or release intent, slot/generation identity, and stale-generation
   refusal without claiming a graph allocator or a new audio scheduler.

The receipt must also include reset/epoch cancellation, late-event refusal,
capacity overflow, deterministic tie ordering, partition-invariant output, and
a sequence mutation that fails while unaffected records remain explainable.
The existing `examples/g5-event-consumer` Python oracle may remain the offline
trace checker, but its known-invalid zero-test `unittest discover` invocation
must not be used as evidence (`docs/reference/2026-11-12-post-roadmap-audio-graph-capability-gaps.md:232-244`).

## Voice-intent decision

**Reuse existing identity and binding records; do not add a new public voice
queue in P1.** `VoiceIdentityKey`, `VoiceBankManifest`, and the reviewed
fixed-slot binding are suitable control-side identity evidence. A P1 fixture
may carry a test-owned value record containing slot, generation, absolute
sample, and intent kind, then prove that stale generations are refused before
dispatch. It must not pretend that this record is a production graph voice
runtime.

If the required consumer cannot exercise voice intent through the existing
binding and ingress seams, the correct outcome is **HOLD**, followed by a
separate P2 admission for graph-owned voice lifecycle. Do not introduce a
universal event envelope, second scheduler, dynamic callback graph mutation,
or an unreviewed `VoiceIntentQueue` merely to satisfy the P1 fixture.

## Acceptance and stop conditions

Admission is allowed only after an installed public consumer and source-bound
receipt demonstrate all four families, two-block projection, ordering, reset,
late/overflow refusal, partition invariance, and mutation failure. The receipt
must identify which evidence is production public API and which is test-owned
adaptation. Missing installed SDK support modules, absent voice-intent seam,
or any callback allocation/topology mutation leaves this packet **CONDITIONAL
HOLD**. No runtime implementation should begin until a reviewer accepts this
packet and names the exact consumer worktree and file claim.

## Recommended next action

Keep P-1 **CONDITIONAL HOLD**. The existing
`examples/p1-event-admission-consumer` is the authoritative partial fixture.
It now proves each family across blocks from an installed SDK: MIDI ingress,
the graph-authored trigger lane, the parameter mailbox into a `PluginSlot`
node, and voice intent through `VoiceBankBinding`. A combined control also
coordinates the four families through one test-owned epoch and absolute sample
origin on one block grid across five schedules, with an explainable sequence
mutation. UMP/MPE sidecars in this fixture, mailbox overflow telemetry,
all-family reset, measured callback allocation freedom, executor parity, and
voice intent as a graph node remain NOT_CHECKED. The evidence is recorded at
`docs/evidence/p1-event-admission-consumer-20261112.md:93-364`, with the exact
qualification result in
`docs/evidence/p1-event-admission-consumer-20261112/receipt.json:8-156`; no
runtime/API change or second queue is authorized by this packet.
