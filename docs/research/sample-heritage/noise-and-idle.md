# Noise and idle behavior

## Evidence

- **Confirmed** — TI's [*Dynamic Performance Testing of Digital Audio D/A
  Converters*](https://www.ti.com/lit/an/sbaa055/sbaa055.pdf) treats idle-channel
  noise, dynamic range, THD+N, and SNR as related but distinct measurements.
- **Confirmed** — the [PCM56 data sheet](https://www.ti.com/lit/ds/symlink/pcm56.pdf)
  specifies dynamic range separately from differential linearity and low-level
  THD. A single broadband-noise number cannot account for all low-level
  converter behavior.
- **Confirmed** — the [TLV320DAC3100-Q1 data sheet](https://www.ti.com/lit/ds/symlink/tlv320dac3100-q1.pdf)
  reports SNR as an A-weighted idle-channel-noise measurement and separately
  specifies THD and THD+N. Measurement bandwidth, weighting, load, gain, and
  signal condition are therefore part of the evidence.
- **Confirmed** — AES dither research distinguishes intentional dither from
  equipment noise and signal-dependent quantization effects; see
  [*Dither in Digital Audio*](https://aes.org/publications/elibrary-page/?id=5173).

## Mapping to the neutral mechanism

- **Confirmed** — [`SampleHeritageBusNoiseIdleBlock`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  has two linear full-scale amplitudes from 0 to 1, an always-on or voice-active
  gate, a seeded state policy, and a target spectral tilt from -24 to +24 dB per
  octave with explicit floor and reference frequencies.
- **Confirmed** — [`SampleHeritageBusDsp`](../../../core/audio/include/pulp/audio/sample_heritage_bus_dsp.hpp)
  generates one seeded bipolar-uniform stream for `idle_amplitude` on every
  frame and a second draw for `noise_amplitude` when its gate is open. Both pass
  through the same deterministic high-shelf cascade.
- **Probable** — separate always-present and activity-conditioned stochastic
  components are useful neutral controls. The labels `idle` and `noise` do not,
  by themselves, map those controls to standard converter measurements.
- **Test** — the 0-to-1 amplitudes and +/-24 dB-per-octave tilt range are safe
  authoring bounds, not evidence-derived ranges for equipment.
- **Test** — promote a spectral tilt only after unweighted spectra at multiple
  levels and activity states show a stable slope. An A-weighted SNR figure alone
  cannot determine it.

## Schema concerns

1. The present names obscure gating semantics: `idle_amplitude` is always
   generated, while `noise_amplitude` may be voice-active. Names such as
   `always_on_amplitude` and `active_amplitude` would describe the mechanism
   without claiming a physical cause.
2. The schema does not identify measurement bandwidth, weighting, or amplitude
   convention. Profile evidence needs those fields even if the runtime block
   remains normalized linear amplitude.
3. One broadband random model plus tilt cannot represent hum, spurs, limit
   cycles, correlated code noise, or level-dependent distortion. Do not fold
   those observations into this block merely to obtain a spectral match.

## Unresolved calibration questions

- What remains with a digital-zero input, and what appears only while voices are
  active?
- Are channels independent, correlated, or fed by a shared analog stage?
- Is the residual stationary broadband noise, deterministic spurs/hum, or
  signal-correlated conversion error?
- Which bandwidth, weighting, gain, load, and source state produced each number?
