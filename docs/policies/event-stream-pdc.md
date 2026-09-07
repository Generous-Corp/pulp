# Event-stream plugin delay compensation

Status: **RESOLVED for the scheduled event stream. Event-to-event device
execution remains open.**

Typed device placements make device-chain topology durable. A track can declare
ordered pre-fader and post-fader placements, typed event-to-event,
event-to-audio, and audio-to-audio slots, format-neutral bindings, bypass and
wet/dry values, and a content-addressed state reference. Those declarations are
sufficient for editing, persistence, interchange loss accounting, and later host
resolution. They are not, by themselves, a runtime lowering contract.

This document is that contract. It answers the latency unit and lifecycle, how a
latency change invalidates state, how event and audio paths meet, and what a
track may offer a musician playing live. What it does not yet do is execute an
event-to-event device: the runtime envelope still admits exactly one device
shape, so the compensating shift resolves to zero for every chain Pulp can
currently play.

## The contract

### Unit and range

Latency is counted in **samples**, at the sample rate the program was compiled
against. A device reports `int`, matching `PluginSlot::latency_samples()` and
`CustomNodeType::latency_samples`. An accumulated shift is `std::int64_t`,
matching the graph's own accumulated path latency and `SamplePosition`.

Never ticks. A device's latency is a property of its buffering, not of the
music, so expressing it musically would shrink it as tempo rises and make a
tempo ramp change the compensation of a filter that did not change. This is the
same reason a `ProductionDeclaration`'s wall-clock lookahead is barred from
reaching a delay-compensation computation.

Because the shift is a constant number of samples and a lowered event's sample
position was already resolved through the exact compiled tempo map the program
carries, a shifted window crosses a tempo point with no special case. The map's
non-linearity is already baked into the event positions.

**Ceiling — and the limitation it imposes.** A device's reported latency is
range-checked against the graph's existing per-node ceiling,
`CustomNodeType::kMaxLatencySamples` = **65 535 samples**, and so is the
accumulated chain shift. One constant serves the audio lane and the event lane.
A second, event-only ceiling would be two numbers that must be kept in step by
hand, which is the failure shape this repository has paid for before; the
constant is shared rather than split.

The cost of sharing it is concrete and is expected to bind:

- **65 535 samples is 1.365 s at 48 kHz** (1.486 s at 44.1 kHz, 0.683 s at
  96 kHz, 0.341 s at 192 kHz — the ceiling is a sample count, so the wall-clock
  headroom halves every time the rate doubles).
- The class of device it constrains is the **musically scaled** one: a long
  arpeggiator or generative delay that wants a bar of context is over the
  ceiling at any tempo below 176 BPM in 4/4 at 48 kHz, and a two-bar pattern
  generator is over it at any usable tempo. An audio filter never needs this
  much; an event device plausibly does.
- A device over the ceiling **fails admission with
  `EventDeviceLatencyOutOfRange`, carrying its reported value and the limit. It
  is never silently clamped**, because a clamped device is one whose events are
  quietly early by the difference, with nothing in the output saying so.

Raising the ceiling is a later decision that must move the shared constant, not
introduce a second one beside it.

### Discovery and update lifecycle

Latency is read on the **control thread, once, at admission**, through
`latency_query()` before `latency_samples()`, and cached into the immutable
prepared binding. `PluginSlot` metadata accessors reach into the live plugin
object and are unsafe from the audio thread, so `process()` never reads a
latency; it reads a prepared number.

An unanswered query is never coerced to zero. `LatencyQuery::Unsupported` and
`LatencyQuery::QueryFailed` both fail admission, because collapsing them into a
confident zero is exactly the reading a later alignment proof would certify.

Pulp as a host currently offers a hosted plugin no latency-changed callback, so
a device that changes its latency mid-stream is observed only at the next
control-thread relatch. That asymmetry is deliberate for now: the notification
path is additive and can be added without changing anything below.

### What a run-time change does during playback

A changed shift is **held until the transport stops**, then latched. Re-aligning
mid-playback would displace every later event by the delta, which is audible in
a stream a musician is listening to, and no arrangement of the scheduler makes
it inaudible. The scheduler reports the pending relatch so a UI can say the
alignment is stale rather than leaving it silent.

A shift never changes mid-block. It is adopted at a block boundary, the same way
a program generation is.

### Alignment: shift the window, never the data

For an event stream injected at chain position *k*:

```
shift(k) = Σ event-domain latency(device_i) for i in [k, end of the event chain)
```

and the stream is scheduled from `range.timeline_sample_start + shift(k)`
instead of `range.timeline_sample_start`. Events are read ahead; the downstream
chain delays them back into place; they land on the authored sample.

Reading ahead is legal here precisely because the program is lowered over the
whole timeline before playback begins. The future is already known. That is also
why scheduled playback and live input cannot share one policy.

The shift lives in the **query window, not in the event data**. `PlaybackProgram`
is immutable and shared by the offline and realtime paths, and a lowered event's
sample position is authoritative against the compiled tempo map. Folding a
host-derived latency into it would make the program a function of the host graph
as well as the document, force a recompile on every device swap, and break the
documented meaning of the field.

**A device that converts events to audio contributes nothing to the shift.** Its
audio output already participates in the graph's own delay compensation, which
delays every sibling branch to match, so adding the same number to the event
window would compensate twice and pull the stream early by exactly the amount
the graph had already handled. Only latency that delays the **events** moves the
window. This is the narrow addition that closes the gap between event routing
and audio PDC: a MIDI edge still carries no latency-aligned audio, and still
does not participate in the audio compensation pass; what it now declares is
that the destination's event-domain latency belongs in the source's scheduling
shift.

> **Deliberate deviation from the design proposal — do not "fix" it back.**
> The proposal this contract was written from
> (`planning/2026-09-06-event-stream-pdc-contract-proposal.md`, section 3)
> stated the rule as the event-chain sum **plus** `audio_latency_from(the
> event→audio boundary to the track output)`. That second term is excluded here
> on purpose. The graph's plug-in delay compensation already inserts the delay
> lines that align that device's audio output against every sibling branch, so
> the whole output is uniformly late by the path latency and the event stream
> needs no correction for it. Adding the term would shift events early by an
> amount the graph had already handled — and double compensation does not
> error, it produces a plausible-looking wrong offset. Excluding it is also what
> keeps the promise that no second latency total is computed anywhere: the audio
> total is never read on this path. The distinction is covered by a test that
> uses a **different non-zero** latency in each domain, so the two formulas give
> different answers and the test can tell them apart; the test is named in the
> acceptance section below.

### Invalidation

A latency change invalidates the **binding, never the program**. The document
model contains no latency and must not gain one; the program is a pure function
of the document, the tempo map, and the sample rate. The prepared per-track
shift lives in the binding generation and is republished the way any other
prepared value is.

### Block, loop, and tempo boundaries

The shift is applied **per `TransportRange`, never per block**. The loop wrap is
the real hazard: a block that straddles a wrap carries two monotonic ranges, and
shifting per block would read the second range from the first range's origin and
replay the pre-wrap window. The existing rule that a note whose onset precedes
the new range is not chased applies to the **shifted** range, so reading ahead
does not turn a seek into a chase.

A range that locates events by host beat rather than by document sample has no
document-sample origin for a sample-domain shift to land on, and converting the
shift to ticks is what the unit rule forbids. That combination fails closed with
a typed process code rather than scheduling against an undefined quantity.

**Reading ahead past an enabled loop's end is refused, not approximated.** Near
a loop end the shifted window wants the content from *after* the wrap; the
document positions past the loop point are events this pass will never reach.
Wrap-aware read-ahead is a distinct mechanism and is not implemented, so a
compensating shift whose window would cross the loop end fails closed with
`CompensationLoopWrapUnsupported` rather than playing the wrong events. An
uncompensated stream is unaffected, and the guard costs it no per-range work.

### Live input

**A track that offers live input into a compensated event chain is refused.**

A scheduled note can be issued *L* samples early and arrive on time. A live note
cannot: it has no future to read from, so it is heard *L* late relative to the
arrangement whatever the software does. Refusing the pair documents that
asymmetry instead of shipping a silent divergence of exactly *L*.

The destination is a **per-track monitor bypass**, expressed through the
recording vocabulary that already separates recorded-input placement from live
monitoring from output scheduling — a take can be *placed* correctly even when
it was *monitored* late. That is a later slice. Refusing now forecloses none of
it, and it removes no capability: the compiler currently rejects any track
policy that makes a provider other than the arrangement available, so no
compiled program can reach this refusal. It is written now so that the first
executable event-to-event device meets a stated policy rather than an
undefined one.

### Typed diagnostics

Admission is control-thread and reports `EventDeviceLatencyUnavailable` (the
query could not be answered), `EventDeviceLatencyOutOfRange` (negative, or past
the ceiling, carrying the reported value and the limit), and
`EventChainLiveInputUnsupported`. The per-block path reports
`EventCompensationUnsupported` for a compensating shift meeting a host-beat
range. Admission codes and process codes stay separate because admission and
per-block execution are different threads with different recovery.

### Offline and realtime equivalence

The offline renderer pre-rolls from tick origin with output discarded, so
stateful nodes enter a region with exactly the history a full bounce would have
given them, and it prepares the same binding the realtime path uses. Pre-roll
from origin is therefore at least the maximum event shift by construction, and
the shift comes from one prepared binding rather than from two code paths. The
scheduled stream is identical between the two.

## What remains open

- **Executable event-to-event devices.** The runtime envelope admits one device
  shape; every other declaration is still a typed refusal. Until an
  event-to-event device executes, every chain Pulp can play resolves to a zero
  shift, so the compensating path above is proven by its own tests rather than
  exercised by production content.
- **Per-track monitor bypass.** Live input into a latent event chain is refused,
  not compensated.
- **Host-side latency-changed notification.** Pulp offers a hosted CLAP plugin
  only the GUI extension, so no hosted plugin can report a latency change today;
  one is seen at the next control-thread relatch. This is why the hold-and-
  relatch rule is a hold rather than a refusal: refusing would reject plugins
  that legitimately vary their latency, for a condition Pulp cannot currently
  observe. It is also why the dynamic-latency acceptance test drives a synthetic
  latency source rather than a real backend.
- **Wrap-aware read-ahead.** A compensating shift refuses at a loop end instead
  of reading the post-wrap content.

## Acceptance

The behaviour above is covered by `test/test_playback_event_pdc.cpp` (shift
accumulation and range checking, saturation, tempo change inside a shifted
window, block-boundary crossing at several partitions, loop-wrap crossing,
the hold-until-stop relatch, whole-span versus small-block equivalence, the
loop-end read-ahead refusal with its uncompensated control, and the host-beat
refusal with its control) and
`test/test_timeline_event_pdc_admission.cpp` (control-thread discovery, the
unanswerable and out-of-range refusals, the live-input refusal with controls on
both axes, an admitted chain as the positive control, and a synthetic device
that changes its reported latency mid-stream — the dynamic-latency case, driven
by a test double because no real backend can report the change).

The audio-boundary exclusion above is covered specifically by *"the scheduling
shift uses event-domain latency alone"* in
`test/test_timeline_event_pdc_admission.cpp`. It builds a chain whose
event-domain and audio-domain latencies are different non-zero values, so the
implemented rule and the proposal's rule give different answers, and asserts the
shift is the event-domain value alone.

Two causal controls are recorded through `tools/scripts/confirm_failure.sh`,
each breaking the code in the direction of a specific bug:

1. Removing the compensating addend from the scheduler must make the shift
   acceptance tests fail.
2. Making an event-to-audio device contribute its latency to the shift — the
   proposal's original formula — must make the exclusion test above fail.

