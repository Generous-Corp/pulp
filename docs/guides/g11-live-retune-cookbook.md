# Retune a sounding graph instrument at a callback boundary

This recipe changes a sounding note through existing public Pulp primitives.
It demonstrates a host-selected callback boundary, with continuous oscillator
phase and envelope state. It does not introduce a tuning engine or a graph
scheduler. G-11 remains **PARTIAL**.

The [retained macOS offline receipt](../evidence/g11-live-retune-20261110/README.md)
contains numeric chronology, audio, clean SDK provenance and mutation proof.

The standalone installed-SDK project is
[`g11-live-retune-consumer`](../../examples/g11-live-retune-consumer/README.md).
Its runtime-editable rack contains two independent Processor instruments, one
MIDI input and a stereo output. Each instrument composes `VaOscillator` and
`Adsr64`; the left channel is retuned and the right channel is an unchanged
330 Hz sibling. The graph is the rack composition engine; the Processor owns
the instrument's DSP.

The host prepares the graph once at a maximum of 256 frames, publishes one
MIDI note-on, and retains its `ProcessorNodeInstance`. At a selected boundary,
an external control thread queries `TuningProvider::note_to_frequency`, checks
note validity, filtering, finiteness, Nyquist and the product parameter range,
resolves `voice.frequency` from the frozen public parameter catalog, then
writes its `StateStore`. The instrument's prepared `ParamReader` reads that
frequency once on entry to the next callback. It supplies a new increment to
`VaOscillator::next()` without calling `reset()` or `prepare()`.

| Absolute sample | Control action before rendering that sample | Result |
| --- | --- | --- |
| 0 | One MIDI note-on; initial parameter 440 Hz | Attack starts at phase 0.137 cycles |
| 8191 | Resolve A4 through a 660 Hz tuning provider; write parameter | New increment starts at 8191; entry phase is retained |
| 24577 | Resolve A4 through a 442 Hz tuning provider; write parameter | New increment starts at 24577; entry phase is retained |
| 40960 | End render | Both voices have advanced exactly 40960 samples |

The sample at a change boundary uses the phase accumulated before the change;
the changed increment first affects the next sample's phase. A phase-continuous
frequency step can still change the waveform's slope. This recipe provides no
zipper-free glide or smoothing claim.

The offline host splits regular and irregular callback schedules at the two
declared boundaries. It joins the control thread before starting the next
callback. That join is test-host synchronization outside `process()`, not an
audio callback wait. A UI update arriving at an arbitrary wall-clock instant
has no promised sample timestamp here. An application needing an intra-block
edit must use an admitted existing event/parameter path with the corresponding
timing contract; this recipe does not add one.

## Acceptance

The fixture renders 40960 frames at 44.1, 48 and 96 kHz using 1, 32, 64, 128,
256 and repeating 17/127/3/251-frame schedules, each split at the declared
boundaries. An independent closed-form integral of the frequency timeline
checks phase at every callback endpoint with a circular error bound of
`1e-9` cycles. A scalar envelope state machine and sine formula check every
sample of both output channels with an absolute error bound of `1e-6`.
Each rate's six output buffers must also be bit-identical. Lifecycle checks
require one prepare, one note-on, uninterrupted frame counts and unchanged
envelope progression for both instruments.

The shipped offline `estimate_pitch` analyzer checks pre-change and both
post-change windows: voiced, confidence above 0.99, frequency within 0.01 Hz.
The 48 kHz stereo WAV stores the retuned voice on the left and the sibling on
the right; the numerical gate operates on the original float buffers before
24-bit PCM export.

At both boundaries, 14 refusals check null/unsupported provider selection,
invalid notes, unknown parameter path, invalid/filtered provider results,
zero, negative, product-range, Nyquist, NaN and positive/negative infinity
frequencies. Every refusal must preserve the old parameter, phase, envelope,
frame count and lifecycle. Subsequent rendered audio must still match the
unchanged expected timeline. Malformed-query fixtures are test doubles, not
replacement tuning implementations.

Three planted controls actually move the first write one sample late,
re-prepare the sounding instrument, or edit its sibling. Each must fail the
unchanged oracle with its specific reason and exit code. A separately compiled
mutation removes the accepted parameter write; the positive recipe must fail
its phase/audio oracle. A crash, configure error or unrelated nonzero exit
cannot satisfy these controls.

`ScalaTuningProvider`, when present in the selected SDK, loads a local scale
and anchors A4 to 660 Hz on the control side, then exercises the same live
boundary render. File parsing and optional MTS session discovery stay outside
the callback. A disabled provider is explicitly unavailable; an MTS external
master is **NOT CHECKED** by this deterministic fixture. Applications can pass
an existing MTS provider through the same provider-neutral boundary adapter,
but that is not measured MTS session evidence.

## Remaining obligations

This is deterministic macOS offline evidence. Realtime allocation/lock/I/O and
deadline qualification, raced control/audio writes, device capture, external
MTS master behavior, non-Darwin installed-consumer parity, notation and graph
serialization, baked parity, spatial/HRTF behavior and other live-sampling
obligations remain **NOT CHECKED** here. Existing static tuning and bounded
recording/publication recipes remain separate evidence. This recipe does not
complete G-11 or the roadmap's broader developer-experience gates.
