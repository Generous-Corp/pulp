# Oversampling filters and the real-input FFT

Reference for `<pulp/signal/oversampling.hpp>`, `<pulp/signal/halfband_iir.hpp>`
and the real-input transforms in `<pulp/signal/fft.hpp>`.

## `OversamplerT` filter kinds

| Kind | Default | Rejection | Passband | Latency |
|---|---|---|---|---|
| `polyphase_iir` | yes | `standard`: 114.6 dB from 0.515 Fs. `pristine`: 153.2 dB from 0.515 Fs | flat (< 1e-8 dB) to 0.485 Fs | minimum phase, frequency dependent |
| `fir_biquad` | no | about 7 dB just above base Nyquist, ~9 dB at 0.55 Fs | -1.9 dB at 0.3 Fs | minimum phase, frequency dependent |
| `linear_phase_fir` | no | `standard`: 96 dB, `pristine`: 140 dB | flat to 0.40 Fs / 0.45 Fs | constant, reported by `latency()` |

`Kind::polyphase_iir` cascades elliptic half-band allpass stages whose
coefficients `design_halfband_iir(count, passband_edge)` derives in closed
form. It is the cheapest lane at a given rejection: per base sample it costs
about a sixth of the biquad lane's time at x2 and about a quarter at x4. The
stopband figures are `OversamplerT::polyphase_stopband_db(quality)`, the worst
stage design's equiripple level. Stage 0 sets the base-band edge; each higher stage only keeps the
same absolute band clean at twice the rate, so it uses a much shorter design.

A half-band's transition is symmetric about base Nyquist, so content the
callback creates in (0.5, 0.515) Fs folds onto (0.485, 0.5) Fs with only
partial rejection. That lands above the advertised passband, and above 21 kHz
at a 44.1 kHz base rate; if it matters, `linear_phase_fir` ends its transition
at Nyquist instead. Everything the callback creates from 0.515 Fs up meets the
full stopband: a tanh driven at 5 kHz, whose fifth harmonic falls at 0.521 Fs
of a 48 kHz base, aliases at -91 dBc at x2 and -147 dBc at x4 (standard).

### Additional methods

| Method | Description |
|---|---|
| `set_quality(Quality)` | Linear-phase lane: `standard` (96 dB, flat to 0.40 Fs) or `pristine` (140 dB, flat to 0.45 Fs). Polyphase lane: `standard` (114.6 dB) or `pristine` (153.2 dB), both from 0.515 Fs and flat to 0.485 Fs. |
| `group_delay_samples(double f)` | Round-trip group delay in base samples at `f` cycles per base sample; constant for the linear-phase lane. |
| `polyphase_stage_design(Quality, int stage)` | The `HalfBandIirDesign` a polyphase stage uses; evaluate it with `halfband_iir_response()`, `halfband_iir_magnitude_db()`, `halfband_iir_group_delay()` or `halfband_iir_stopband_db()`. |

### Precision

The polyphase stages keep their coefficients and state in double even when the
sample type is float. A float round trip therefore differs from the double one
only by the rounding of its float samples: -148 to -152 dB, against -132 to
-139 dB with float state. Double and float arithmetic cost the same in this
latency-bound kernel; measured, the float lane got about 3% slower.

### Delay of the minimum-phase lanes

Both minimum-phase lanes report `latency().constant == false` and
`latency_samples() == 0`; their delay is not host-compensated.

| Round trip | Near DC | At 0.4 Fs |
|---|---|---|
| `polyphase_iir` standard, x2 | 3.79 base samples | 12.6 |
| `polyphase_iir` standard, x4 | 4.90 | 13.9 |
| `polyphase_iir` pristine, x2 | 5.16 | 16.7 |
| `polyphase_iir` pristine, x4 | 6.87 | 18.8 |
| `fir_biquad`, x2 | 0.97 | 1.5 |
| `fir_biquad`, x4 | 1.46 | 2.1 |

`group_delay_samples(f)` evaluates the curve from the filter coefficients. For
the biquad lane it holds only below about 0.2 Fs: that lane rejects images so
weakly that the decimator folds a tone's own image back onto it above there.
A caller blending the oversampled path with a dry path can align the dry path
with `group_delay_samples()`, or select `linear_phase_fir` for a constant,
reportable latency.

`polyphase_iir` became the default in v0.997.2; that
[migration note](../migrations/v0.997.2.md) covers the consequences and how
to keep `fir_biquad`. Both designs later narrowed their transition to
0.485-0.515 Fs; `pulp upgrade --notes` lists that change and its delay cost
for the release that carries it.

### Silence

The polyphase stages zero any state below about 1e-30 (float) at the end of
each processed block, so silence decays to exact zero (within about 4200
samples at standard quality, 5600 at pristine, in float) rather than a subnormal limit
cycle. Per-sample `process()` flushes every sample, so block and per-sample
output can differ by less than that threshold while a signal dies away.

## Real-input FFT

| Method | Description |
|---|---|
| `forward_real(in, out)` | N real samples to all N bins, upper half mirrored. |
| `real_bins()` | `size() / 2 + 1`: the independent bins of a real transform. |
| `forward_real_half(in, out, scale = 1)` | N real samples to DC..Nyquist only, scaled in the same pass. Nothing past Nyquist is written. |
| `inverse_real_half(in, out[, scale])` | DC..Nyquist back to N real samples; the default scale 1/N recovers the input. Imaginary DC/Nyquist parts are ignored, and `out` may reuse `in`'s storage. |

The half-spectrum pair runs an N/2-point complex transform, so it is roughly a
quarter faster than `forward_real()` and about twice as fast as a complex
transform of the same real data. `PartitionedConvolver`, the STFT and the
spectral frame engine run on it. The overlap-add `Convolver` keeps its spectra
as separate real and imaginary arrays instead, the layout vDSP's real
transform produces, so a block multiplies its spectrum without interleaving
it on the way in or out. All of these calls are realtime
safe after construction; size must be at least 2.

On Apple platforms the float transform keeps its vDSP split buffers 16 bytes
past a 64-byte boundary, for accuracy. vDSP takes a separate code path when
both halves of a split array sit on the boundary, and from 512 points up that
path rounds 0.4-5.3 dB worse on every processor measured (real and complex
transforms on M1 Max, M5 Max and M5 Ultra; real transforms on M3 Ultra). The
placement is pinned rather than left to the allocator, so a transform's
accuracy does not depend on where its buffer lands;
`FftT::vdsp_split_offset_bytes()` reports it, and a test asserts the 16-byte
offset at every size and gates the real transform's error at -134 dB. Speed is
a separate, processor-dependent effect: on M5 Max and M5 Ultra the
off-boundary path is also the faster one from 512 to 8192 points, while on
M1 Max and M3 Ultra the boundary path is faster for large transforms (from
2048 real and 1024 complex points on M1 Max, from 1024 real and 512 complex
points on M3 Ultra), so there the placement costs speed for accuracy.
