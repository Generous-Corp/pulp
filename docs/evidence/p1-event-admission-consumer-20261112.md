# P1 event admission consumer

Status: **PARTIAL / qualification pending**.

The source fixture is an installed-SDK consumer at
`examples/p1-event-admission-consumer`. It verifies all four typed families
through public seams across at least two blocks and two partition schedules:
MIDI host ingress, the graph-authored event lane for triggers, the parameter
mailbox into a `PluginSlot` node, and voice intent through the installed
`VoiceBankBinding`. Each family runs in its own graph or binding. No runtime
API or queue is added.

The gate remains `NOT_QUALIFIED`: the admission-contract rows listed under
"Remaining rows" below are `NOT_CHECKED`, and the runtime, provider parity,
realtime, and non-Darwin lanes remain `NOT_CHECKED`.

## Exact live-origin recheck — 2026-11-12

This lane was rechecked from a fresh worktree at exact `origin/main`
`e3a6c36e7e3d8efb836659f1bc5ae4dd8a419a5c` (Pulp `1.119.0`). The only
available installed release SDK is `/Users/danielraffel/.pulp/sdk/1.54.0`,
with `sdk-provenance.json` source SHA
`b9c14f9d8332cbb0cbcbec75740331b8ddade4c6` and
`lib/cmake/Pulp/PulpConfig.cmake` SHA-256
`253e3552b00a5845205c7cbf9d23a7f3cc0aca51573df51fe0ea9eb6fe1de794`.
That prefix does not ship `pulp/host/graph_event_contract.hpp`,
`trigger_graph.hpp`, or `voice_identity.hpp`.

The bounded public consumer configure succeeded against that prefix, but the
governed single-worker build failed before compilation with:

```text
fatal error: 'pulp/host/graph_event_contract.hpp' file not found
```

The durable command logs are [live-origin-configure.log](p1-event-admission-consumer-20261112/live-origin-configure.log)
(`5a21785141418bf25c086bbd5a5a13d0870e9e98a05eaf846f7e4db4f426995c`) and
[live-origin-build.log](p1-event-admission-consumer-20261112/live-origin-build.log)
(`0fe0762ad7c4dcc2323e7b052abf1f4bc4b7578f10133c80eb359e1a4e5088e5`).
No CTest or executable run was attempted after the compile refusal.

The source-level seam audit independently remains fail-closed: generic
`inject_parameter_events` publication does not add a routed Processor input
unless the private exact-parameter claim is present
(`core/host/src/signal_graph_prepare.cpp:1122-1127`), and the exact claim is
owned by the private topology-edit path
(`core/host/src/signal_graph_prepared_topology_edit.cpp:2094-2108`).
`TriggerContext` construction is private to the trigger state
(`core/host/include/pulp/host/trigger_graph.hpp:46-58`), while graph voice
projection has no public mailbox or dispatch seam. Therefore the four-family,
two-block P1 gate remains **NOT_QUALIFIED / HOLD**; no runtime/API seam,
universal queue, or voice-intent API is introduced.

## Installed-SDK run — 2026-11-13

The consumer now builds and runs from a complete installed SDK. The prefix is
a clean Release source install of `4c69e183387017c47d048b477218a6d0a4947bcc`
(`sdk-provenance.json`: `kind=source-install`, `build_profile=release`,
`source_git_dirty=false`, `platform=darwin-arm64`). Its installed
`graph_event_contract.hpp` and `signal_graph_runtime.hpp` are byte-identical
to the same files at `origin/main` `5b904b8712667b87d2ecd43f9315c574fc37ec68`;
`core/` and `tools/cmake/` do not differ between the two heads. The digests are
in the receipt's `build.sdk` block.

The first complete run failed on two fixture controls, and both were fixture
defects rather than runtime defects:

* `midi-duplicate-sequence-refusal` published the duplicate after `project()`.
  `HostEventIngress` detects a duplicate producer sequence only among staged
  records, and `project()` removes the records it delivers, so the refusal is
  observable only before projection. The control now publishes the duplicate
  before projecting.
* `trigger-cross-block-samples` expected `trigger_trace()` to hold both blocks.
  That view covers only the most recent block and expires at the next render
  boundary, so the control now copies each block's record after that block.

After the correction the positive run prints
`p1-event-admission PASS checks=48 partitions=4,8 planted_negative=5 consumer_gate=NOT_QUALIFIED`
and CTest passes 1/1. Two input mutations show that the corrected controls
discriminate. Giving the duplicate a unique sequence fails
`midi-duplicate-sequence-refusal`, and moving the second-block impulse fails
`trigger-cross-block-samples`.

The gate stays **NOT_QUALIFIED**. This run closes only the installed-SDK lane
for the MIDI and trigger-graph controls. Parameter-mailbox projection for a
public `Processor` and public `TriggerContext` current/future delivery remain
**UNAVAILABLE** through the public API, and voice projection remains
**NOT_CHECKED**. Those two unavailable rows are the binding gaps that a P1
runtime admission would have to address. They are recorded here as evidence
and do not authorize an API change.

## Graph event lane, PluginSlot and voice-bank controls — 2026-11-13

The trigger, parameter, and voice rows were reworked against seams the
installed SDK already exposes. The prefix is the same clean Release source
install of `4c69e183387017c47d048b477218a6d0a4947bcc`; the installed
`graph_event_contract.hpp`, `signal_graph_runtime.hpp`,
`voice_bank_binding.hpp`, `custom_node_type.hpp`, `custom_node_events.hpp`,
`plugin_slot.hpp`, and `parameter_event_queue.hpp` are byte-identical to
`origin/main` `1042181bec4b33ea3180ced14972a8ab01fd63bf`. Their digests are in
the receipt's `build.sdk` block.

* **Trigger (graph-authored lane).** Public `TriggerContext` was refused as a
  seam, so the trigger row now uses a `CustomNodeType::process_events`
  producer and consumer joined by `SignalGraph::connect_graph_event`. The
  producer emits with `GraphEventWriter::emit_at` and the consumer reads
  `GraphEventReader::events`. One emission at sample 1 is delivered in the
  producer's own block. Eight emissions at sample 4 are staged in block 0 and
  delivered in the later block whose window contains sample 4, in sequence
  order. A ninth emission at sample 4 is refused with `Overflow`, a target past
  the look-ahead horizon with `Future`, and a target behind the block start in
  the next block with `Late`. Schedules 2+2+4 and 3+5 produce identical event
  tuples and audio. A separate graph stages a record for sample 3, processes a
  block with `reset_requested`, and never delivers that record; the next
  emission carries the following epoch. The existing trigger-graph check is
  kept as trigger-graph evidence, not a `TriggerContext` consumer.
* **Parameter (PluginSlot node).** A small `PluginSlot` subclass added with
  `SignalGraph::add_plugin_node` records each `ParameterEventQueue` it
  receives. Over four blocks at block sizes 4 and 8,
  `SignalGraph::inject_parameter_events` delivers only the latest injection
  before a callback, sorted by offset with equal offsets in push order;
  delivers nothing in a block with no injection; and delivers a fresh
  injection once. A queue that overflowed at the source makes the injection
  return `false` while the 1024-event retained prefix is published. The two
  block sizes deliver identical events. Delivery to a plain
  `add_processor_node()` `Processor` is not relied on.
* **Voice intent (VoiceBankBinding).** The test-owned note-generation adapter
  is replaced by `pulp::host::VoiceBankBinding` with `MpeVoiceBankOwner`, over
  32 samples at partitions 4 and 8. A note-on published at sample 2 lands on
  one slot with a nonzero generation. A note-off at sample 10, in a later
  block, puts that slot and generation into release. Through
  `dispatch_deferred`, a wrong generation, a wrong slot, a second release, the
  retired generation after the slot is retired, and the retired generation on
  the slot re-used by a note-on at sample 26 are each refused with
  `StaleGeneration`. The live generation accepts deferred pressure and release,
  and a stale epoch is refused with `StaleEpoch`. Audio, slots, and generations
  match across the two partitions. This is control-side evidence; no graph
  voice allocator or audio scheduler is claimed.

The positive run prints
`p1-event-admission PASS checks=223 partitions=4,8 graph_event_schedules=2+2+4,3+5 graph_events=9 voice_slot_reuse=1 consumer_gate=NOT_QUALIFIED`
and CTest passes 1/1.

Each new control was checked with a planted input mutation, built and run
against the same installed SDK, and restored with `git checkout`:

| Mutation | Result |
|---|---|
| far-future emission retargeted inside the look-ahead horizon | `FAIL rule=graph-event-future-refusal` |
| late emission retargeted to a sample not yet consumed | `FAIL rule=graph-event-late-refusal` |
| ninth same-sample emission moved to a different sample | `FAIL rule=graph-event-overflow-refusal` |
| `reset_requested` cleared on the reset block | `FAIL rule=graph-event-reset-cancels-pending` |
| older parameter queue injected after the latest | `FAIL rule=parameter-mailbox-latest-wins` |
| latest parameter queue re-injected before the quiet block | `FAIL rule=parameter-mailbox-one-shot` |
| equal-offset parameter events pushed in the opposite order | `FAIL rule=parameter-mailbox-equal-offset-order` |
| overflow fill stops at capacity | `FAIL rule=parameter-mailbox-source-overflow-refusal` |
| voice note-on moved past the first checkpoint | `FAIL rule=voice-bank-note-on-slot` |
| voice note-off moved past the release checkpoint | `FAIL rule=voice-bank-release-intent` |
| retired-generation record replaced by the live generation | `FAIL rule=voice-bank-stale-generation-refusal` |

### Remaining rows

The gate stays **NOT_QUALIFIED**. These admission-contract rows are
`NOT_CHECKED` by this fixture:

* one shared epoch and absolute sample origin coordinating all four families
  in a single consumer projection; each family runs in its own graph or
  binding;
* MIDI/UMP/MPE projection through `SignalGraph::inject_midi` inside this
  fixture (the G-5 consumer covers it separately);
* parameter overflow telemetry counters through the graph mailbox; only the
  `false` return and the retained prefix are asserted;
* a sequence mutation that fails while the unaffected records remain
  explainable;
* callback allocation and topology-mutation freedom; the probe `PluginSlot`
  records events into a vector;
* more than two partition schedules per family;
* routed versus reference executor parity for the graph-authored lane and the
  parameter mailbox.

Parameter delivery to a plain `Processor` node and public `TriggerContext`
delivery remain unavailable and are not used.
