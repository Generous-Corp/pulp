# S4: one audio-owner reset transaction

Status: **PARTIAL**. The reset transaction and the timeline-to-graph epoch
mapping are built and tested in-tree (slice 1). Bounded typed UMP/MPE storage
for the graph-authored lane and the voice bank (slice 2) and a graph
voice-intent endpoint (slice 3) are not built; their exact remainder is below.

## What exists

`pulp::host::HostTimelineBinding` (`core/host/include/pulp/host/host_timeline_binding.hpp`)
is the one place that maps a `HostBlockTimeline` onto the event families a
graph-owning Processor or host feeds. It binds, each optionally:

| Family | Owner | Coordinates the binding hands it |
|---|---|---|
| MIDI 1.0 with the full UMP packet, and the MPE sidecar derived from it | the prepared trigger `SignalGraph`'s host ingress | the graph's current trigger epoch and cursor; current block only |
| Graph-authored trigger events | the same trigger plan (`try_reset_events()` resets them with the ingress) | the graph's rendered-sample clock |
| Voice intent | `VoiceBankBinding` | the bank's epoch, timeline samples; future records staged |
| Sample-accurate parameters | the binding's own fixed staging (256 records) | timeline samples; future records staged; delivered block-relative into a caller `ParameterEventQueue` |

Each family keeps its own capacity and refusal rules: the graph ingress (256
records, duplicate producer sequence refused), the voice bank's ingress and
MIDI 1.0-only voice rules, and the parameter staging (late, stale, producer
zero, duplicate sequence and overflow refused without change). No queue is
shared between families and no record is converted to another family's type.

### Reset transaction

The first block, every timeline `NewEpoch` (seek, loop, backwards clock,
clock-source change) and every block after `request_reset()` reset every bound
family. Each moves to a fresh epoch, so every record and deferred voice token
published before the reset is refused afterwards (stale timeline epoch, stale
graph epoch, stale voice epoch, cut voice generation).

Ordering makes it all or nothing:

1. The timeline step is staged on a copy.
2. `VoiceBankBinding::reserve_event_reset()` claims a fresh voice epoch and
   changes nothing else (refused inside an open block).
3. `SignalGraph::try_reset_events()` resets the graph's host ingress, MPE
   tracker, projection and trigger state. It is the only step that can still
   fail (for example `Busy` while another owner holds the ingress block open).
4. Only then: `commit_event_reset()` with the reserved epoch (infallible),
   the staged parameters are dropped (counted in `parameters_reset`), and the
   timeline copy is committed.

A refusal at step 2 or 3 leaves every family, the staged parameters and the
timeline exactly as they were; the same host block can be retried and resets
exactly once.

### Timeline epoch alignment with the graph ingress

The D4 gap: `SignalGraph::begin_host_event_block()` takes the trigger
controller's epoch and requires the block to start at the graph's cursor, so a
`HostBlockTimeline` epoch could not be fed to it. The binding reads the
graph's epoch and cursor every block and maps the timeline window start onto
the cursor:

- After a binding reset both start at sample zero in fresh epochs.
- A host gap (same timeline epoch, steady counter skipped forward) is
  compressed onto the graph's rendered-sample clock. Because a later gap would
  retime a staged graph record, the binding accepts graph records for the
  current block only and refuses a future one with `OutOfRange`.
- A graph reset made elsewhere (plan install, a direct `try_reset_events()`)
  is re-mapped onto the new graph epoch at the next block and counted in
  `graph_remaps`; it does not reset the other families.

### Failed and oversized graph blocks

The graph refuses a host block in two different ways, and the binding treats
each to match what the graph actually did:

- **Refused before the graph touched the block** (for example `Busy` on the
  diagnostic gate, or short buffers): the graph's ingress block is still open,
  so the binding block stays open and `render()` can be retried.
- **`ProcessingFailure`**: a refused projection seals the graph's ingress
  block, restores the block's records and leaves the graph cursor in place.
  The refusals behind it (MIDI or UMP capacity, the MPE fan-out preflight, the
  mailbox copy) fail identically on a retry, so keeping the block open would
  stall. The binding closes the block as a failed graph block: it clears the
  graph ingress (the binding only ever stages graph records inside the
  current block, so this drops exactly that block's records), zeroes the
  graph output when the cursor did not move, still renders the voice bank, and
  counts it in `graph_failed_blocks`. The next block maps onto the unmoved
  graph cursor and carries only its own records at their own offsets, so they
  are neither replayed a block late nor stacked onto the next block. If the
  ingress cannot be cleared at that moment (gate contention), the next
  `begin_block()` clears it first or refuses as a skipped block.
- **Oversized block**: `SignalGraph::begin_host_event_block()` now refuses a
  block longer than the live trigger generation's prepared frame limit with
  `OutOfRange`. Before, it accepted the block, `process_checked()` then
  refused it with `Frames` before projecting, and the ingress block (and the
  binding block) stayed open forever. The binding consumes such a block as a
  skipped block, like any other post-transaction refusal.

The MPE fan-out preflight in `project_host_events_()` reserves
`count * (kMaxNotes + 2)` = 130 sidecar slots per projected record against a
1024-slot sidecar, so any graph block with 8 or more MIDI records (an 8-note
chord) is refused with `ProcessingFailure`. The binding now handles that
coherently, but the records of such a block are dropped; the preflight bound
itself is unchanged (see "Remaining work").

### Realtime fix found on the way

`derive_mpe()` rebinds the caller's MPE tracker on every block on the audio
thread. Through `bind_tracker_to_buffer()` (whose captures do not fit
`std::function`'s inline storage) every trigger-graph host-event block that
carried MIDI allocated four times. `midi::bind_tracker_to_sink()` forwards
identically through a one-pointer capture, and `derive_mpe()` now uses it.
`derive_mpe()` binds those callbacks to a sink on its own stack, so it now
unbinds them on every return path; a later tracker call reaches no sink.

## Evidence

`test/test_host_timeline_binding.cpp` (`pulp-test-host-timeline-binding`):

- A four-family consumer drives MIDI/UMP into a trigger graph, the graph's own
  trigger events (audio impulses), a real `VoiceBankBinding` and parameter
  records through a first span, a forward host gap, a seek and a requested
  reset. Five partitions (1, 7, 16, 32, and 32/5/11) give identical graph
  audio, MIDI traces with full UMP words, trigger traces, parameter traces,
  voice audio (2e-6) and reset counters, with exact expected graph-domain and
  timeline-domain samples asserted on the reference.
- Equal-time order across producers for MIDI and parameters; a future voice
  and parameter record delivered at its own sample; a staged parameter
  skipped by the gap and one invalidated by the seek, both counted.
- Stale timeline, graph and voice epochs and a cut voice generation refused
  after a reset, with in-epoch positive controls.
- Late, negative, unbound, future-graph, duplicate-sequence and overflow
  refusals per family; parameter projection refusing a short destination with
  both sides unchanged.
- Transactional failure both ways: the graph refusing after the voice
  reservation, and the voice bank refusing before the graph reset; nothing
  changes and the retried block resets exactly once.
- Voice reset tokens single-use and refused inside a block.
- An external graph reset re-mapped, not treated as a binding reset.
- No allocation across continuous, gap, seek and requested-reset blocks
  (`RtAllocationProbe`, with its self-check), and `derive_mpe()` allocation
  free with a positive control that the probe sees an oversized
  `std::function` capture.
- A payload/sequence mutation changes only the graph MIDI trace.
- A block of 8 projected MIDI records fails in the graph; the binding closes
  it with silent graph output, the graph cursor unmoved, and the next block
  delivering only its own record at its own offset, with the graph clock and
  the mapping advancing together afterwards. Negative control: 7 records are
  accepted and delivered. Before the fix the next block failed too (the 8
  restored records plus the new one) and nothing was delivered.
- A 64-frame block on a graph prepared for 32 is refused at open as a skipped
  block; the following 32-frame block opens and delivers its record. Before
  the fix the binding stayed open and every later block was refused `Busy`.
  The graph refuses `kGraphMaxFrames + 1` at `begin_host_event_block()` and
  accepts `kGraphMaxFrames`.
- `derive_mpe()` leaves no tracker callback bound after success and after a
  refusal, and a later tracker call does not reach the sidecar.

`tools/scripts/confirm_failure.sh` on the committed tree: CONFIRMED for six
planted breaks (graph refusal ignored in the transaction, graph cursor mapping
replaced by the timeline start, the stale timeline-epoch check removed,
`derive_mpe()` back on `bind_tracker_to_buffer()`, the voice reset token check
removed, staged parameters kept across a reset). Four more for the failed and
oversized block handling: the failed-block drop not called from `render()`,
the graph ingress clear skipped, the graph's open-time frame-limit check
disabled, and `derive_mpe()` leaving `on_note_lifecycle` bound.

Existing suites still pass: trigger graph, host signal graph, voice bank
binding, MPE, host-event timestamps, control index, and the g5, p1 and
trigger in-tree consumers.

## Not checked

- Lock and free freedom of the callback path: only allocation is probed. The
  RealtimeSanitizer lane is not run for this binding.
- An installed-SDK build of the consumer: the consumer is an in-tree Catch2
  suite, not an `examples/` project built against an installed prefix.
- The graph-authored Custom-node event lane (`connect_graph_event`,
  `process_control_checked`, transport `should_reset_stream_history()`) is a
  separate execution mode from the trigger plan and is not bound: its epoch
  moves through the transport reset flag, not through this transaction.
- Parameters are delivered to the caller; the binding does not inject them
  into a `PluginSlot` node (trigger plans do not host plugins).
- Routed versus reference-walk parity of the binding's graph traces: the
  trigger plan runs only on the canonical routed executor
  (`process_checked()` refuses other modes), so no reference trace exists.

## Remaining work

1. **Slice 2: bounded typed UMP/MPE storage in the graph-authored lane and the
   voice bank.** `GraphAuthoredEvent` carries a 16-bit `payload`, and
   `VoiceBankBinding::publish()` refuses any record with `has_ump`. Add a
   separate fixed-capacity typed sidecar per lane holding the full
   `midi::UmpPacket` (four words, word count) and the MPE note identity
   (`MpeNoteGeneration`, channel, note), indexed by the event's sequence, with
   its own capacity and overflow refusal; do not widen the 16-bit payload.
   For the voice bank, accept MIDI 2.0 channel-voice UMP (note on/off with
   16-bit velocity, per-note pitch bend, per-note controllers) by deriving
   through the existing `MpeVoiceTracker::process(UmpPacket)` path, refusing
   every other message type. Acceptance: held and releasing expression at high
   resolution across partitions and resets, packet truncation controls.
2. **Slice 3: a graph voice-intent endpoint.** A trigger-graph node that turns
   graph-authored events into voice intent must delegate to
   `VoiceBankBinding::publish()` / `dispatch_deferred()` through this binding,
   using the bank's allocator, never a second one; its records join the voice
   family above and inherit the same reset.
3. **Bind the Custom-node event lane** to the same transaction, or document a
   rule that the two execution modes never share a binding.
4. **Lock/free probes and an installed-prefix consumer** for the rows under
   "Not checked".
5. **Tighten the MPE fan-out preflight.** `count * (kMaxNotes + 2)` refuses
   any graph block with 8 or more MIDI records. A bound in terms of the
   tracker's active notes (each record adds at most one) would admit ordinary
   chords, but it must be proven against every `MpeVoiceTracker` emission
   path before it replaces the conservative bound.
6. **An abort path for persistent pre-projection refusals.** `Mode` (for
   example anticipation or parallel routing enabled) and `ClockExhausted`
   leave the graph ingress block open, and the binding has no way to seal it
   without a graph-side abort; such a binding stays `Busy` until the mode is
   cleared.
