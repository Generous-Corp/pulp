# P1 event admission consumer

Status: **PARTIAL / qualification pending**.

The source fixture is an installed-SDK consumer at
`examples/p1-event-admission-consumer`. It verifies all four typed families
through public seams across at least two blocks and two partition schedules:
MIDI host ingress, the graph-authored event lane for triggers, the parameter
mailbox into a `PluginSlot` node, and voice intent through the installed
`VoiceBankBinding`. A combined control also drives all four families through
one test-owned epoch and absolute sample origin on the same block boundaries.
No runtime API or queue is added.

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

## Combined epoch and sequence-mutation controls — 2026-11-13

The fixture now drives all four families through one test-owned epoch
(`kCombinedEpoch = 7`) and one absolute sample origin, on the same block
boundaries, in one control loop over 256 samples. Source commit
`59fff62c0d355677bd5f44c87535988db87730d8`, built from the same clean Release
source install of `4c69e183387017c47d048b477218a6d0a4947bcc`.

Per block, in this order:

* **MIDI.** A control-side `HostEventIngress<32>` stages eight absolute
  note-on records in block 0 (including two producers at sample 2), projects
  the block window, converts each absolute sample to a block-relative
  `sample_offset`, and calls `SignalGraph::inject_midi` on a MIDI input node.
  That node is joined with `connect_midi` to the graph-authored lane consumer,
  which reads `CustomNodeEventBlock::in`. A stale test epoch is refused with
  `StaleEpoch`. This closes the `inject_midi` row for MIDI 1.0 note events; the
  UMP and MPE expression sidecars are not exercised here.
* **Parameter.** Absolute parameter records are converted to block-relative
  offsets in a `ParameterEventQueue` and injected with
  `inject_parameter_events` into a `PluginSlot` node in the same graph,
  including two records at the same sample in push order.
* **Trigger.** A `CustomNodeType` producer in the same graph emits each
  trigger with `GraphEventWriter::emit_at` three samples before its absolute
  target. Small blocks stage the record as bounded-future; large blocks deliver
  it in the producer's own block. Every delivery lands in the consumer block
  whose window contains its sample.
* **Voice intent.** Note-on and note-off records are published to a
  `VoiceBankBinding` (`MpeVoiceBankOwner`, release held past the run) with the
  same `begin_block`/`render` boundaries. Slot and generation are read after
  each render, the release keeps the note-on's slot and generation, rendered
  audio places each intent at its absolute sample, and a generation from
  another slot is refused with `StaleGeneration`. The binding renders its own
  prepared graph on the shared block grid; it is not a node of the
  `SignalGraph`.

The runtime has no shared epoch, so the adapter maps each native epoch to the
test epoch and requires it to stay constant: MIDI ingress uses the test epoch
directly, the graph-authored lane reports epoch 1, the voice bank reports
epoch 1, and the block-relative parameter mailbox has no epoch (recorded as 0).

The canonical trace is the list of (family, test epoch, absolute sample, order
key) records stably sorted by absolute sample and family, so records of one
family keep their delivery order. It is identical, together with the voice
audio and the native epoch map, across five schedules: 256 blocks of 1, 8 of
32, 4 of 64, 127+127+2, and 3+61+1+127+64. The 27 records and the per-family
FNV-1a digests are in
[combined-trace.jsonl](p1-event-admission-consumer-20261112/combined-trace.jsonl)
(`80d08295b8321e279f8ec04914edc9d42866bf3ee6edb621fecdde86125bff92`), written
by `pulp-p1-event-admission-consumer --trace <path>`.

**Sequence mutation.** In the same binary, five planted negatives run against
the canonical trace. For each family, adding one to a single record's order key
(MIDI producer sequence, parameter push index, trigger lane sequence, voice
generation) changes that family's digest, leaves the other three digests
identical, and a record-by-record diff names only that record. Swapping the two
equal-sample MIDI records changes only the MIDI digest, and the diff names only
those two records. The mutated digests are recorded in the trace file and the
receipt.

**Callback observation.** The graph-lane, reset and combined consumers and
the `PluginSlot` probe now record into preallocated fixed arrays with a count
and an overflow flag, which is asserted after rendering; no container grows in
those callbacks. The block loop calls only `inject_midi`,
`inject_parameter_events`, `process`, and the voice bank's
`begin_block`/`publish`/`render`; it makes no topology call. This is a
construction property of the fixture, not an allocation or realtime proof of
the runtime or of `VoiceBankBinding::render`.

The installed `graph_event_contract.hpp`, `voice_bank_binding.hpp`,
`custom_node_type.hpp`, `custom_node_events.hpp`, `plugin_slot.hpp`,
`parameter_event_queue.hpp`, `trigger_graph.hpp`,
`signal_graph_execution_snapshot.hpp`, `midi/buffer.hpp`, and
`midi/message.hpp` are byte-identical to `origin/main`
`d486ff70c6b02c59acdeac504af747724d1fb906`. `signal_graph_runtime.hpp`
differs only in the `inject_parameter_events` comment: `origin/main` adds
`a91f5681944c`, which refuses live parameter injection into a `Processor` node
in `core/host/src/signal_graph_mailbox.cpp` and leaves `PluginSlot` nodes
unchanged. The fixture injects only into a `PluginSlot` node. The other
`core/` differences between the SDK source and `origin/main` are in the
CoreMIDI device layer, which the fixture does not use.

The positive run prints

```text
p1-event-admission PASS checks=2763 partitions=4,8 graph_event_schedules=2+2+4,3+5 graph_events=9 voice_slot_reuse=1 combined_schedules=1,32,64,127+127+2,3+61+1+127+64 combined_records=27 digests=midi:349d8d9cbaead7ac,parameter:21c6a5a653826137,trigger:79300472c428c8e4,voice:76096164fc3782a6 sequence_mutations=5 consumer_gate=NOT_QUALIFIED
```

and CTest passes 1/1. The captured `--trace` count of 2765 is historical evidence from before the executor-parity control (two file-write checks). The current committed run records 14135 without `--trace`, so the current trace count is 14137 by the same increment; the trace command was not rerun in this evidence packet.

Each new control was checked with a planted input mutation, rebuilt against
the same installed SDK, run, and restored with `git checkout`; the restored
build passes again:

| Mutation | Result |
|---|---|
| producer adds one late `emit_at` per block after the first, shifting lane sequences by block count | `FAIL rule=combined-partition-invariance` |
| producer of the sample-2 note-on changed from 4 to 2 | `FAIL rule=combined-midi-tie-order` |
| stale-epoch probe published with the live test epoch | `FAIL rule=combined-midi-stale-epoch-refusal` |
| MIDI adapter offset forced to 0 | `FAIL rule=combined-midi-absolute` |
| parameter adapter offset forced to 0 | `FAIL rule=combined-parameter-order` |
| trigger emitted one sample after its target | `FAIL rule=combined-trigger-order` |
| voice note-off sent on another MIDI channel | `FAIL rule=combined-voice-release-intent` |
| voice records published at the block start instead of their sample | `FAIL rule=combined-voice-onset-sample` |
| voice note-off published as a note-on | `FAIL rule=combined-voice-onset-sample` |
| stale-generation probe uses the slot's own generation | `FAIL rule=combined-voice-stale-generation-refusal` |
| graph reset requested at sample 200 | `FAIL rule=combined-epoch-map` |
| consumer trigger array shrunk to 4 entries | `FAIL rule=combined-observation-capacity` |
| digest ignores the order key | `FAIL rule=sequence-mutation-detected` |
| digest also mixes each record's order key into the next family | `FAIL rule=sequence-mutation-isolated` |
| planted negative also changes a second record | `FAIL rule=sequence-mutation-explained` |
| digest made order-insensitive (sum of record hashes) | `FAIL rule=order-mutation-detected` |

A note-off published as a note-on retriggers the note: the allocator releases
the old generation, so the release-intent rule still sees a releasing slot.
The onset rule is what catches it.

## Executor parity — 2026-11-13

The combined control now renders the same 256-sample schedule on the legacy
reference walk (`set_canonical_executor_routing_enabled(false)`) and on the
routed serial executor (`true`, set before `prepare()`), for each of the five
partition schedules, and requires identical output. The graph input is a
deterministic ramp, so the graph output is a non-silent audio comparison, and
the voice bank audio is compared as well. Per backend and schedule the
comparator requires equality of the canonical (family, epoch, absolute sample,
order key) trace, the four per-family digests, the voice audio, the graph audio
and the native-epoch map.

Backend dispatch is proved from telemetry, not assumed:
`routing_executor_stats().blocks_processed` is 0 on the reference walk and
equals the block count on the routed executor (276 of 276 blocks across the
five schedules), and `routed_walk_fallbacks()` is 0, so the routed result is not
a silent fallback to the walk.

Parallel routing is **refused** for this fixture, and the refusal is recorded
rather than forced: `set_parallel_routing_enabled(true)` is a no-op once
`connect_graph_event` has bound the graph-authored lane, because one shared
ordered lane cannot be dispatched with the consumer possibly before the
producer. The fixture requests it after the bind, asserts
`parallel_routing_enabled()` stays `false` and `parallel_levels_dispatched`
stays 0, and still compares that run (which executes on the routed serial path)
to both other backends. A parallel-versus-reference comparison on this graph is
therefore **NOT_CHECKED**; enabling parallel before the bind would bypass the
runtime guard and is not done. Fifteen backend comparisons pass (three per
schedule).

Planted negatives sit inside the fixture, on the routed side only: one
parameter record's order key is bumped (the comparator must name
`backend-trace`, and only the parameter digest may change), one graph-output
sample is shifted by 1.0 (`backend-graph-audio`), and one voice-audio sample is
shifted by 1.0 (`backend-voice-audio`).

The SDK prefix is the one used above (`signal_graph_runtime.hpp`
sha256 `9527c8e5...`, `graph_event_contract.hpp` sha256 `1364609e...`).
`graph_event_contract.hpp` is byte-identical to `origin/main`
`333b37effe57`; `signal_graph_runtime.hpp` differs from it only in comments (the
non-comment lines are identical), and the executor routing setters, stats and
`routed_walk_fallbacks()` the control uses are the same on both.

The positive run now prints

```text
p1-event-admission PASS checks=14135 partitions=4,8 graph_event_schedules=2+2+4,3+5 graph_events=9 voice_slot_reuse=1 combined_schedules=1,32,64,127+127+2,3+61+1+127+64 combined_records=27 digests=midi:349d8d9cbaead7ac,parameter:21c6a5a653826137,trigger:79300472c428c8e4,voice:76096164fc3782a6 sequence_mutations=5 executor_parity=reference-walk,routed-executor parity_comparisons=15 routed_executor_blocks=276/276 routing_fallbacks=0 parallel_after_bind=refused parity_planted_negatives=3 consumer_gate=NOT_QUALIFIED
```

The digests and `--trace` output are unchanged (trace sha256 `80d08295...`).
Each change was committed first, mutated, rebuilt, run, restored with
`git checkout --`, and rebuilt to PASS:

| Mutation | Result |
|---|---|
| reference backend also enables canonical executor routing | `FAIL rule=parity-reference-walk-only` |
| routed backend's trace shifted by one sample after rendering | `FAIL rule=parity-reference-vs-routed` |
| routed backend's graph output sample shifted by 0.5 | `FAIL rule=parity-reference-vs-routed` |
| parallel request asserted accepted instead of refused | `FAIL rule=parity-parallel-refused-after-bind` |

## Remaining rows

The gate stays **NOT_QUALIFIED**. Closed by the combined control: one
test-owned epoch and absolute sample origin across all four families, MIDI 1.0
projection through `SignalGraph::inject_midi` inside this fixture, more than
two partition schedules (five, for the combined control), the explainable
sequence mutation, and routed-executor versus reference-walk parity for the
graph-authored lane, the MIDI input and the parameter mailbox. These admission-contract rows remain `NOT_CHECKED`:

* UMP and MPE expression sidecars through `inject_midi(node, midi, mpe)`
  inside this fixture (the G-5 consumer covers them separately);
* parameter overflow telemetry counters through the graph mailbox; only the
  `false` return and the retained prefix are asserted;
* reset and epoch cancellation applied to all four families at once; reset is
  checked per family (graph-authored lane reset, MIDI and voice stale-epoch
  refusal);
* callback allocation freedom as a measured or realtime property; the fixture
  callbacks write only into fixed arrays, but the runtime and
  `VoiceBankBinding::render` are not instrumented;
* parallel-routed versus reference parity: parallel routing is refused once the
  graph-authored lane is bound (see "Executor parity");
* voice intent inside the `SignalGraph`; the voice bank runs its own prepared
  graph on the shared block grid.

Parameter delivery to a plain `Processor` node and public `TriggerContext`
delivery remain unavailable and are not used.
