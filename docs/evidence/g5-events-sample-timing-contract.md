# G-5 events sample-timing contract

Evidence probe for the current public signal APIs. This fixture composes
`TriggerDetect::process_signal`, `DecayEnvelope::trigger/process`,
`IndexedControl::process`, `MapClip::process`,
`TptFilter::set_cutoff/process_lowpass`, and a caller-owned one-sample feedback
state.

## Contract

At 48 kHz, input onset samples 8 and 40 must produce envelope trigger receipts
at exactly samples 8 and 40. The envelope is consumed once per sample; its
value feeds the filter and the previous filtered value is the feedback input on
the next sample. The caller supplies an external table index every sample; it
advances, pauses, then reverses. The indexed table value is mapped and clipped
to a filter cutoff. The fixture must finish finite and give byte-identical
output across the tested block partitions.

Positive control: `first_trigger=8`, `second_trigger=40`, finite feedback,
identical 96-sample output as one block and as six uneven blocks.

Planted negative control: a sample exactly equal to the strict trigger
threshold (`0.5`) must not fire. Invalid mapping bounds must be rejected while
retaining the prior mapping. Empty tables and non-finite indices return zero.

## Event semantic boundary

For this scalar path, production occurs at `TriggerDetect::process_signal()` on
the audio thread. Each call consumes exactly one input sample; a true result is
an event at that sample's zero-based absolute index. `input > threshold` is
strict. The caller immediately triggers the envelope, then consumes the
envelope, indexed control, filter, and previous feedback in that order. A
callback split preserves detector, envelope, filter, and feedback state; reset
must explicitly reset each state owner. There is no queue, so overflow and
drop are inapplicable to this path. Equal-time events from several producers,
late arrivals, and cross-block queued delivery are outside this API. They need
an explicit producer identity, tie order, late policy, reset epoch, and bounded
capacity before a graph event transport can be added. Host timestamps cannot
be inferred from the caller's table index.

`MapClipT` accepts only finite bounds with `input_max > input_min`, clips its
input, and returns its lower output bound for a non-finite input. The table
reader uses linear interpolation, holds its endpoint for finite out-of-range
indices, and returns zero for an empty table or non-finite index. Both process
without allocation. The caller owns and keeps the table memory stable while
the reader uses it; table replacement and mapping configuration occur off the
audio thread. The external index is in table-sample units and may pause or
reverse. These utilities have no hidden sample clock or scheduling contract.

## Wave 2 implementation disposition (2026-11-03)

The G-5 implementation lane is **PARTIAL / bounded scalar API**. It adds one
typed map-and-clip combinator and an external time-indexed control reader. It
does not add graph event transport. The fixture produced:

```text
fixture=g5-events sr=48000 first_trigger=8 second_trigger=40 positive=1 planted_negative=1 partition_invariant=1 indexed_control=1 final_feedback=0.751888
```

The next graph event API needs the unresolved queued-event semantics above and
a graph-authored consumer fixture before it can claim graph composition.

## Receipt and limits

The exact receipt is the fixture stdout line:

`fixture=g5-events sr=48000 first_trigger=8 second_trigger=40 positive=1 planted_negative=1 partition_invariant=1 indexed_control=1 final_feedback=<finite>`

This proves ordering and sample timing for the composed scalar path and
partition invariance for the tested sequence only. It does not prove host
callback scheduling, MIDI/OSC timestamp transport, arbitrary block partition
invariance, native plugin parity, or realtime allocation behavior.

Run from the repository root with a configured Pulp build/toolchain:

```sh
c++ -std=c++20 -Icore/signal/include -Icore/timebase/include -Icore/runtime/include \
  tools/validation/g5_events_sample_timing.cpp -o /tmp/g5-events-sample-timing
/tmp/g5-events-sample-timing
```
