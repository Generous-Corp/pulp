# Sampler pitch engines

## Evidence

- **Confirmed** — [US patent 4,991,218](https://patents.google.com/patent/US4991218A/en),
  assigned at filing to E-mu Systems, describes two distinct sampler pitch
  mechanisms: skipping samples at a fixed output rate (zero-order interpolation)
  and reading every sample while varying the read rate (variable sample rate).
- **Confirmed** — [US patent 5,111,727](https://patents.google.com/patent/US5111727A/en),
  also assigned at filing to E-mu Systems, describes fixed-output-rate pitch
  shifting as stepping through waveform memory with an integer-plus-fractional
  address and interpolating surrounding samples. Its background explicitly
  distinguishes line-segment (linear) and windowed-sinc interpolation.
- **Confirmed** — the latter patent's claimed implementation is a moderate-order
  multichannel interpolator. It is not evidence that every early sampler used
  linear interpolation, nor that a generic two-point interpolator matches a
  particular product.

## Five-machine evidence

- **Confirmed — SP-1200.** The [owner's manual](https://www.autistici.org/2000-maniax/texts/sp1200_manual.pdf),
  pages 8 and 93, gives a tuning span of plus or minus a fifth, says tuning may
  produce ring-modulation-like results, and says received sample-period metadata
  is ignored because playback has one 26.04 kHz rate. **Probable** — this is a
  fixed-output-rate family. **Test** — distinguish drop/repeat from interpolation
  by inspecting repeated-sample patterns and alias trajectories; the manual does
  not name that sub-mechanism.
- **Confirmed — S950.** The [operator's manual](https://manuals.fdiskc.com/flat/Akai%20S-950%20Owners%20Manual.pdf),
  page 40, permits per-keygroup transposition by plus or minus 50 semitones.
  **Test** — the user control range does not identify the resampling topology;
  capture output-rate images and pitch-step behavior before assigning
  `variable_clock`, `drop_repeat`, or `early_linear`.
- **Confirmed — S1100.** The [service-manual specifications](https://manualzz.com/doc/6408169/akai-s1100-digital-sampler-service-manual)
  explicitly identify a 24-bit custom-LSI interpolation/decimation pitch
  algorithm, a plus or minus two-octave range, and one-cent steps. **Probable** —
  this belongs to a fixed-rate interpolating family, but it is not evidence for
  Pulp's two-point `early_linear` kernel. **Test** — estimate the actual
  interpolation response from fractional-delay and alias-residual captures.
- **Probable — S612.** The [service manual, part 1](https://www.florian-anwander.de/akai_s612/Akai_S-612_Service_Manual_Part_1.pdf),
  sampling instructions and Table 1, tie MIDI notes 36, 48, 60, and 72 to 4, 8,
  16, and 32 kHz sampling clocks and explain the corresponding octave
  transposition on playback. Intermediate notes select intermediate frequencies.
  This strongly supports a clock-linked family. **Test** — capture DAC update
  timing across notes to prove whether playback itself varies the converter
  clock and to bound the usable range.
- **Test — S-550.** The [service notes](https://archive.org/details/roland_S-550_SERVICE_NOTES),
  pages 7-9, place note/envelope/loop computation and 12-to-16-bit waveform
  expansion in the wave gate array, but do not specify the interpolation kernel.
  Use fractional-pitch impulses and tones to classify the pitch family rather
  than inferring it from the fixed 30/15 kHz record-rate choices.

## Mapping to the neutral mechanism

- **Confirmed** — [`SampleHeritagePitchFamily`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  exposes `variable_clock`, `drop_repeat`, and `early_linear`.
- **Confirmed** — [`SampleHeritagePitchProcessor`](../../../core/audio/include/pulp/audio/sample_heritage_pitch.hpp)
  maps `variable_clock` to a machine-clock multiplier with unit source advance,
  `drop_repeat` to zero-order source selection at a fixed machine rate, and
  `early_linear` to two-point interpolation at that fixed rate.
- **Confirmed** — note/source pitch factor is accepted from 1/64 through 64.
  That is Pulp's bounded resource and arithmetic envelope, not a sourced range
  for historical equipment.
- **Probable** — `variable_clock` and `drop_repeat` are defensible mechanism
  names grounded in primary descriptions. They do not identify a product.
- **Probable** — the two-point linear mechanism is defensible. Schema-v3 keeps
  `early_linear` as a flavor label only; public format documentation explicitly
  says it is not a historical claim.
- **Test** — choose a pitch family from clock-rate response, repeated/dropped
  sample patterns, interpolation residuals, and alias trajectories. A single
  steady tone is insufficient to distinguish all mechanisms.

## Schema concerns

1. Schema-v3 resolves the naming concern by normatively documenting `early` as
   a non-historical flavor label. Current evidence supports only the algorithm.
2. If the 1/64-to-64 range remains, docs should call it an implementation bound.
   A profile author must record a narrower measured or usable range in evidence,
   not infer one from the validator.
3. `variable_clock` couples pitch to the machine-rate stages by design. Profile
   docs should make that topology explicit so authors do not also bake the same
   pitch-dependent cutoff change into a second block.

## Unresolved calibration questions

- Does pitch change the physical/update clock, a memory address increment at a
  fixed clock, or both in different ranges?
- For fixed-clock playback, is selection zero-order, linear, or a higher-order
  interpolator, and does it change by voice count or pitch range?
- Where are anti-imaging or anti-alias filters relative to pitch generation?
- Are pitch-factor limits continuous, quantized, or clamped by the source system?
