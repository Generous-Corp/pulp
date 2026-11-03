# G-5 events sample-timing contract

Evidence probe for the current public signal APIs. This fixture composes
`TriggerDetect::process_signal`, `DecayEnvelope::trigger/process`,
`TptFilter::set_cutoff/process_lowpass`, and a caller-owned one-sample feedback
state. It does not add a runtime abstraction.

## Contract

At 48 kHz, input onset samples 8 and 40 must produce envelope trigger receipts
at exactly samples 8 and 40. The envelope is consumed once per sample; its
value feeds the filter and the previous filtered value is the feedback input on
the next sample. Filter cutoff is modulated by a 3 Hz caller-owned LFO. The
fixture must finish finite and return success.

Positive control: `first_trigger=8`, `second_trigger=40`, finite feedback.

Planted negative control: a sample exactly equal to the strict trigger
threshold (`0.5`) must not fire. If it fires, the event boundary is wrong.

## Wave 2 implementation disposition (2026-11-03)

The G-5 implementation lane is **PARTIAL / evidence-only**. The public scalar
composition remains the accepted bounded surface; no additional runtime event
transport or callback scheduling contract is specified by the roadmap. The
fixture was rerun from the Wave 2 implementation worktree with the exact command
below and produced the positive and planted-negative receipt:

```text
fixture=g5-events sr=48000 first_trigger=8 second_trigger=40 positive=1 planted_negative=1 final_feedback=0.767825
```

The next implementation step is blocked until the roadmap names whether this
contract should become a reusable runtime API (including block partition and host
timestamp semantics).

## Receipt and limits

The exact receipt is the fixture stdout line:

`fixture=g5-events sr=48000 first_trigger=8 second_trigger=40 positive=1 planted_negative=1 final_feedback=<finite>`

This proves ordering and sample timing for the composed scalar path only. It
does not prove host callback scheduling, MIDI/OSC timestamp transport, block
partition invariance, native plugin parity, or realtime allocation behavior.

Run from the repository root with a configured Pulp build/toolchain:

```sh
c++ -std=c++20 -Icore/signal/include -Icore/timebase/include -Icore/runtime/include \
  tools/validation/g5_events_sample_timing.cpp -o /tmp/g5-events-sample-timing
/tmp/g5-events-sample-timing
```
