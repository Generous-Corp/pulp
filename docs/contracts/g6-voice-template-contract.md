# G-6 voice-template contract and allocator characterization

Status: **design proposal / allocator evidence; graph binding not implemented**.
Authority: [advanced graph roadmap](https://github.com/danielraffel/pulp-planning/blob/d17bfed119e92b8a178531d78b2fad2570d5b295/advanced-graph-capability-roadmap.md),
G-6, sections 2, 5 and 6. Source baseline:
`2419026d6e69ea6ae672a24342d09433db8f9ccc`.

The first graph voice capability must expand voice instances into the existing
SignalGraph execution plan and reuse `MpeVoiceAllocator<MpeSynthVoice-derived>`
semantics. It must not wrap independent SignalGraphs in each voice, add a
second allocator, or call control-thread graph mutation from the callback.
This document separates the existing allocator contract from proposed graph
requirements. Acceptance of this proposal does not implement those requirements.

## Existing public behavior

The standalone [consumer](../../examples/g6-voice-contract-consumer/main.cpp)
uses only installed public headers and `Pulp::midi`. It is a state probe:
its voice advances expression smoothers but produces no audio. It does not
construct a graph, use a format adapter, or exercise `MpeVoiceTracker` itself.

| Surface | Source-audited behavior; executable confirmation pending |
|---|---|
| Capacity | Construction preallocates the requested pool. Eight notes fill eight slots; a ninth steals without growing the pool. Zero capacity silently ignores notes. There is no allocator maximum/admission error. |
| Stealing | Oldest, lowest velocity, lowest pitch and highest pitch are selectable. Equal priority selects the first pool slot. Release tails remain active and have no special stealing priority. |
| Lifetime | NoteOff begins a voice-owned tail. The voice calls `finish_release()` when complete. Reuse calls virtual `reset()`: the base clears note/expression/tail bookkeeping, but preserves smoothing configuration. Derived DSP/graph state needs its own reset implementation. |
| Identity | Distinct generations may share pitch/channel. Stale NoteOff/expression for a stolen generation is ignored; current-generation expression reaches its slot. |
| Reset | `reset_all()` calls each voice's reset, clears glide state and resets the age counter; it does not clear the age vector or cumulative steal count. `reset_steal_count()` clears that counter separately. |
| Ordering | `dispatch()` acts immediately, regardless of `sample_offset`. `dispatch_all()` respects buffer order without sorting or rendering sample intervals. Explicit `MpeBuffer::sort()` preserves producer order at equal offsets. |

Input generations must already be valid and unique for simultaneous lifetimes;
the allocator does not validate them. The fixture generates them explicitly,
so it does not prove tracker generation, retrigger, rollover, or adapter behavior.
Sustain is not an event kind in `MpeExpressionEvent` and is not implemented by
this allocator. A graph integration cannot infer sustain from this evidence.

## Proposed first graph voice boundary

1. **Fixed capacity and resources.** The first workflow admits exactly eight
   voices before prepare. Zero, other counts, and resource estimates above the
   declared graph limits are structured admission failures, before expansion.
   Preallocate voice instances, expression state, event storage and output
   scratch; report their byte counts and graph work estimate. The existing
   allocator's permissive constructor remains unchanged. No realtime CPU
   qualification follows from a work estimate; measured budgets remain required.
2. **One allocator.** Preserve the existing four policies and first-slot tie
   behavior. Do not silently prefer tails or duplicate allocation bookkeeping.
   Active/releasing/steal telemetry must reflect that same allocator. Define
   explicit voice-slot callbacks/bindings before implementation: the allocator
   currently stores concrete Voice objects and does not supply a graph-binding
   protocol. Graph ownership must not depend on private allocator members.
3. **Authored instances.** Use stable authored paths such as
   `voice[3].filter.cutoff`; slot index and note generation are different
   identities. Expansion names include the slot, because G-4's separate
   `instance_index` field alone does not establish a unique authored name.
   Allocate one state namespace per voice; modifying one must not affect another.
4. **Event boundary.** Events retain source sample offsets and accepted producer
   sequence. The graph integration must render intervals between events before
   dispatch; calling `dispatch_all()` once per block is insufficient. Equal-time
   events retain producer order, including retirement before replacement.
   Set the event buffer's reserved capacity before prepare and enable
   `set_realtime_capacity_limit(true)`; the default buffer may grow. Reject a
   full-capacity append without partial publication, and publish the saturating
   dropped-event count at the same bounded observation boundary. This does not
   by itself prove callback allocation behavior. Cross-block coordinates, late-event rejection and
   reset epoch must use the eventual G-5 graph event contract. No transport is
   defined by the current scalar indexed-control slice.
5. **Sustain and tails.** Sustain policy belongs at the existing MIDI/voice
   integration boundary, not in a second allocator. Before implementation,
   specify where pedal state holds a generation, when deferred NoteOff is emitted,
   channel/zone scope, all-notes-off behavior and same-time pedal ordering.
   This is an unresolved prerequisite, not a claim that the current tracker or
   allocator handles it. Voice-owned tail completion and reset must be bound to
   graph node state without calling authoring APIs on the audio thread.
6. **Persistence.** First schema version persists authored template identity,
   voice count, policy and per-slot parameters. Active-note, envelope and tail
   checkpoint restoration is unsupported and must be rejected explicitly;
   do not silently load it as an idle preset. Preset activation resets active
   generations at a defined boundary. Restore validates the whole payload before
   publication; reject wrong template/slot/path, non-finite values and unsupported
   versions without partial mutation. Add migration and previous-version tests
   when a prior serialized version actually exists. G-4's in-memory parameter
   restore is not a saved graph voice schema.

## Missing bindings and implementation order

[G-4's initial template](https://github.com/Generous-Corp/pulp/pull/11445)
supports Input/Output/Gain nodes and gain parameters. It cannot yet declare an
oscillator/filter/envelope Processor voice, event ports, or per-note modulation.
[G-5's indexed-control slice](https://github.com/Generous-Corp/pulp/pull/11473)
does not supply graph event transport. Both were open at the source inspection;
their merge alone does not satisfy the missing contracts above.

Before implementing G-6, settle and land the minimal G-4 public
Processor/event/parameter binding and the G-5 event delivery contract, including
the sustain integration decision. Add catalog/notation parity and persistence
without inventing a second notation. Then implement one fixed eight-voice graph
consumer. The current proposal deliberately names no speculative public C++ API.

The eventual consumer must use only published headers and CLI, at most 200 lines
of non-generated glue, named parameters and structured errors. Required controls:
ninth-note stealing under every policy; stale expression after reuse; overlapping
same-pitch notes; pedal hold/release; finite tails; per-voice isolation; exact
event offsets across block partitions; reset/restore refusal atomicity; saved
identity; and positive audio output. Plant routing, timing, identity and state
faults and require the corresponding oracle to fail. The A1 instrument additionally
requires audio-rate modulation, bounded feedback, presets and live edits.
Realtime claims require the roadmap's paced-callback measurements, allocation/
lock/I/O checks, sample-rate/block matrix, and independent phase review.

## Running the bounded consumer

Configure the example with `CMAKE_PREFIX_PATH` pointing at an installed SDK,
then build its sole target with the repository's governed build wrapper and run
`pulp-g6-voice-contract`. No source-tree include path or private test helper is
allowed. The SDK release may differ from the source baseline only when the
relevant installed headers are recorded and compared; this is header API
characterization, not a claim about all binaries in that SDK.

Use `tools/scripts/confirm_failure.sh` on the consumer to change the stale
pressure event's generation from 1 to 9. The unchanged stale-generation oracle
must fail after a real recompile and pass after restoration. This changes the
event sent through the public API, not the expected value or the installed SDK.
Record baseline, broken and restored outputs in the phase status.

This change currently supplies the executable source and source-backed contract.
The installed-SDK build and mutation control are registered as
`cmake-g6-voice-contract-sdk-consumer` in native POSIX CTest, with isolated
fixture sources and captured phase outputs. Execution remains pending; the
behavior table is not a passing-test receipt. No installed-SDK build result is
inferred from registration, configuration or matching header hashes.
