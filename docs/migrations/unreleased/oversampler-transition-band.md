---
breaking = true
summary = "OversamplerT's polyphase lane narrows its transition to 0.485-0.515 Fs and rejects deeper at both qualities (114.6 dB standard, 153.2 dB pristine), with about 1.1 base samples more delay at x2"
---

## What changed

Both qualities of the `polyphase_iir` lane (the default oversampler) use
elliptic half-band designs with a narrower transition. The passband edge moves
to 0.485 of the base rate and the stopband starts at 0.515, so harmonics a
callback creates just above base Nyquist now meet the full stopband instead of
the transition band. `Quality::standard` uses twelve sections at stage 0
instead of six; `Quality::pristine` uses sixteen instead of eleven.

| | Before | After |
|---|---|---|
| Standard rejection | 94.8 dB from 0.58 Fs | 114.6 dB from 0.515 Fs |
| Pristine rejection | 146.3 dB from 0.55 Fs | 153.2 dB from 0.515 Fs |
| Passband, standard / pristine | flat (< 1e-8 dB) to 0.42 / 0.45 Fs | flat (< 1e-8 dB) to 0.485 Fs |
| Standard delay near DC, x2 / x4 | 2.66 / 3.49 base samples | 3.79 / 4.90 base samples |
| Standard delay at 0.4 Fs, x2 / x4 | 7.6 / 8.6 base samples | 12.6 / 13.9 base samples |
| Pristine delay near DC, x2 / x4 | 4.56 / 5.98 base samples | 5.16 / 6.87 base samples |
| Round-trip cost with a tanh callback, standard x2 / x4 | 1 | about 1.10 / 1.05 |
| Round-trip cost with a tanh callback, pristine x2 / x4 | 1 | about 1.06 / 1.09 |

A tanh driven at 5 kHz into a 48 kHz base rate puts its fifth harmonic at
0.521 Fs. Before, that harmonic sat in the transition band and aliased at
-45.5 dBc (standard) and -70.5 dBc (pristine) at x2 and x4 alike. Now the
x4 round trip aliases at -147 dBc (standard) and -167 dBc (pristine). At x2
both read -91 dBc, which is the fold the callback creates above the
oversampled Nyquist before any filter sees it.

The float build's stages also keep their state in double now, so a float
round trip adds only the rounding of its float samples (about -150 dB)
instead of -132 to -139 dB of accumulated section rounding.

The delay remains uncompensated: `latency()` still reports a non-constant
delay and `latency_samples()` still returns 0.

## Who is affected

Code that uses the default kind or selects `polyphase_iir`. Aliasing drops
and the top of the band is flat further up; the extra delay matters only where
the oversampled path is mixed with or aligned against another signal.

## Migration steps

Nothing is required. To align a dry path, read the delay from the filter
rather than hard-coding it:

```cpp
const double delay = oversampler.group_delay_samples(0.0); // base samples
```

For a constant, host-compensated latency use `Kind::linear_phase_fir`.
Filter figures for every kind are in
[Oversampling filters and the real-input FFT](../reference/oversampling-and-real-fft.md).
