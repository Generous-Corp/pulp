# Chroma and onset analysis front ends

`<pulp/signal/analysis_frontends.hpp>` exposes reusable, fixed-capacity front
ends for consumers that need chroma frames or onset novelty without rebuilding
Pulp's spectral analysis stack. Both use `signal::FftT`; they do not introduce
another FFT implementation.

## Ownership and preparation

`ChromaFrontEndT<Sample, MaxFftSize, MaxChannels>` and
`OnsetNoveltyFrontEndT<Sample, MaxFftSize, MaxChannels>` own their window,
history, FFT input/output, and novelty state in fixed-capacity member storage.
The caller owns planar input pointers and the sink. `prepare()` is control-thread
work: it constructs the existing FFT backend and may allocate its retained FFT
storage. Admission is transactional and checked against
`max_retained_fft_bytes`; a rejected prepare leaves the previous prepared state
usable. `process()` and `reset()` allocate no memory after a successful prepare.

The sink must be nothrow-invocable. Its work, including any allocation or
publication, belongs to the caller. A front end and its callback have one
processing-thread owner; the classes do not synchronize concurrent calls.
Publish copied frames to another thread through an application-owned bounded
handoff rather than sharing the mutable front end.

Allocation-free is not a deadline guarantee. FFT and callback cost still have
to fit the caller's scheduling budget. Background/offline ownership is the safe
default unless a product has measured its chosen FFT size, hop, channel count,
and sink on its audio-thread budget.

## Window and cadence contract

The configuration carries an explicit sample rate, exact channel count, FFT
size, and hop size. The sample rate must be finite and positive. Input channel
count must exactly match the prepared count and must not
exceed the template capacity. Spectral methods require a power-of-two FFT size;
energy flux does not.

Only complete windows are emitted. There is no zero-padded tail. For FFT size
`N` and hop `H`, the first frame covers samples `[0, N)` and is ready after
sample `N - 1`. Later windows start at `H`, `2H`, and so on:

- `H < N` produces overlapping windows.
- `H == N` produces contiguous windows.
- `H > N` produces gapped windows while preserving absolute sample timestamps.

Frames report window start, center (`start + N/2`), readiness sample, and a
one-based sequence. Chroma's centered estimate has an algorithmic lookback of
`N/2`, but the first result still cannot be published before `N` input samples
have arrived. Onset novelty readiness is `N - 1` samples from a clean start.
These are analysis/readiness latencies, not inserted audio-path delay.

`reset()` clears all history, timestamps, and sequence state. Results are
independent of how input is partitioned across `process()` calls. A non-finite
sample is rejected, clears dependent history, advances the absolute input
timeline, and suppresses output until a complete clean window is available.
`process()` returns `false` if any sample or derived value was rejected, even if
later samples recovered and emitted valid frames.

## Chroma policy

Chroma averages channels to mono, applies the symmetric Hann window already
used by the built-in key analyzer, and finds local FFT-magnitude peaks from 55
Hz through 5 kHz. Each peak is assigned to the nearest equal-tempered MIDI
pitch class. A frame exposes both the raw 12-bin magnitude accumulation in
`double` (matching the built-in analyzer's accumulation width) and its
sample-typed unit-sum normalization. The built-in key analyzer accumulates those raw frames
and retains its Krumhansl-profile scoring postpass.

The built-in key adapter checks a maximum of 256 channels. It returns its normal
unsuccessful analysis result outside that supported front-end policy rather
than truncating or silently remixing channels.

## Onset policy

Onset analysis exposes novelty frames, not onset markers:

- Energy flux computes mean power across every channel/sample pair. Opposite-
  polarity stereo therefore retains energy.
- Spectral flux and high-frequency content average channels to mono before a
  rectangular-window FFT. High-frequency content weights each positive bin
  delta by its one-based bin index.
- The first complete window has zero novelty because there is no predecessor.

The distinction is deliberate. `audio::OnsetDetector` needs the complete
novelty sequence to divide confidence by its global maximum, calculate a
symmetric adaptive threshold, and retain the strongest event inside the spacing
window. It continues to own that offline postpass. Normal bounded overlapping or
contiguous configurations use the fixed-capacity front end. Gapped
configurations, larger than 65,536-sample windows, or more than 256 channels
retain legacy support through an explicitly background/offline dynamic adapter
in `core/audio`; that adapter uses `FftT` and the same pure novelty transitions
rather than a separate spectral stack.

## Relationship to other analysis primitives

These classes produce musical-analysis observations. They do not replace
`SpectrumTraceT` or `ScopeCaptureT`, and they do not own a control-platform
registry. A product can publish copied chroma or novelty frames into its own
telemetry/control surface, but capability discovery and policy remain the
product or unified-control layer's responsibility.
