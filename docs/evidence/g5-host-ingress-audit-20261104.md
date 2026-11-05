# G-5 host MIDI/MPE/time-index ingress audit

Audit base: Pulp `origin/main`
`1e591dfe1de462dbca5b39f31ebf2be5c3082ae1` (2026-11-04).

## What is already proven

The public graph host accepts a prepared `MidiBuffer` at a `MidiInput` node
through `SignalGraph::inject_midi()`. Each event already carries a block-relative
`sample_offset`; the prepared mailbox uses fixed storage and publishes a bounded
prefix when an input exceeds its event, SysEx, payload, or UMP capacity. The
existing host tests provide the controls:

* `SignalGraph connect_midi routes events through the graph` proves MIDI and UMP
  offsets 0/16/24, SysEx offset 8, and exactly-once consumption.
* `SignalGraph preserves MPE-bearing UMP events through MIDI edges` proves UMP
  offsets 3/11/19/27 and derives note-on, per-note pitch bend, and timbre with
  `MpeVoiceTracker` after graph extraction.
* `SignalGraph MIDI sidecar drops are caller-visible` plants event, SysEx,
  payload, UMP, and missing-sidecar overflow controls; each refusal retains the
  bounded prefix and remains observable.
* `SignalGraph MIDI injection and extraction require a live node runtime` plants
  unprepared, unknown-node, and released-node controls.

These controls prove block-local MIDI/UMP ingress and bounded failure behavior.
They do not prove host absolute timestamps, a graph-owned sample clock, or MPE
delivery into a graph-authored Processor/voice.

## Missing seam

`SignalGraph::inject_midi()` has no absolute sample/time-index argument: callers
must precompute a block-relative offset. `GraphRuntimeNodeProcessContext` carries
per-node MIDI scratch and its attached UMP sidecar, but no `MpeBuffer`. The
Processor-node adapter currently sets both `Processor::mpe_input()` and
`Processor::ump_input()` to null while dispatching a graph block. Consequently,
MPE is only available to the standalone format adapters and to the post-extract
test derivation above; it is not an input contract for graph-authored voices.

A partial fix in only the routed executor, only the legacy walk, or only one
format adapter would make identical graphs observe different expression and
time semantics. The graph swap mailbox would also need to retain the sidecar and
its tracker generation. No source-owned contract currently defines ties, late
events, future events, reset epoch, or cross-block queue capacity for this seam.

## Safe next slice

Define one immutable ingress record with an absolute sample index and producer
sequence, then add a prepared, bounded adapter that maps records to a block
(`late`, `future`, `overflow`, and stable same-sample order are explicit). Carry
the resulting `MpeBuffer` and UMP sidecars through the mailbox, routed scratch,
legacy walk, and Processor binding together. Add positive partition/swap controls
and planted stale-generation, late, and sidecar-capacity negatives before using
the seam for G-6. Until that contract is implemented end to end, this audit is a
no-change result rather than a runtime capability claim.
