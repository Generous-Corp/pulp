# Cyclic and adaptive stretch

## Evidence

- **Confirmed** — the original [Akai S1000 Series software version 2.0
  manual](https://theatrecrafts.com/archive/documents/s1000_v2_0_manual.pdf),
  pages 45-47, defines offline time stretch over a selected zone from 25% to
  2000% of original length while preserving pitch.
- **Confirmed** — the manual distinguishes `CYCLIC`, which uses a user-selected
  fixed cycle length in samples, from `INTELL`, which varies its decisions with
  sample content. Automatic cycle selection is offered for `CYCLIC` but is
  explicitly described as fallible.
- **Confirmed** — the manual's `quality` and crossfade `width` controls run from
  01 to 99 and apply only to `INTELL`; it does not define their DSP units.
- **Confirmed** — the [S950 operator's manual](https://manuals.fdiskc.com/flat/Akai%20S-950%20Owners%20Manual.pdf),
  TIMESTRETCH display page `>14`, describes offline stretch into a new sample,
  a `D-TIME` control, `AUTO-D`, and mono/poly material choices. It says shorter
  D-TIME settings emphasize metallic behavior while longer settings tend toward
  tremolo, but it does not define D-TIME as a cycle length or publish its units.
- **Confirmed** — the [S2000 operator's manual v1.30](https://www.polynominal.com/akai-s2000/akai-s2000-manual.pdf),
  pages 152-154, retains the 25%-to-2000% offline range, defines `CYCLIC` with a
  fixed cycle length in samples plus `AUTO`, and defines `INTELL` with explicit
  quality-decision and crossfade controls.
- **Confirmed** — the [S3000XL operator's manual](https://www.platinumaudiolab.com/free_stuff/manuals/Akai/akai_s3000_xl_manual.pdf),
  pages 139-142, independently documents the same offline CYCLIC/INTELL split,
  automatic cycle selection, quality/crossfade controls, source zone, and new
  destination sample workflow.
- **Confirmed** — Verhelst and Roelands' original [WSOLA paper](https://doi.org/10.1109/ICASSP.1993.319366)
  ([author-hosted PDF](https://sps.ewi.tudelft.nl/Education/courses/ee4c03/assignments/audio_lpc/roelands93icassp.pdf))
  selects a segment within a bounded tolerance interval by maximizing a waveform
  similarity measure, then overlap-adds it. The paper presents cross-correlation,
  normalized cross-correlation, and cross-AMDF as possible measures.
- **Confirmed** — the WSOLA paper describes variants rather than one mandatory
  parameter set; its evaluated speech settings are not sampler-profile defaults.

## Mapping to the neutral mechanism

- **Confirmed** — [`SampleHeritageRecordCommitCyclicStretchBlock`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  provides offline factor, cycle length in samples, crossfade length, and an
  optional source zone. Its 0.25-to-20 factor matches the manual's published
  25%-to-2000% range. The declared cycle remains the output-domain snap period;
  a nonzero crossfade blends across that boundary without shortening the cycle.
- **Confirmed** — [`estimate_sample_heritage_auto_cycle`](../../../core/audio/include/pulp/audio/sample_heritage_record_commit.hpp)
  is a deterministic neutral convenience API. It ranks an explicit lag range by
  DC-removed normalized correlation and chooses the shortest tied lag. Callers
  store the returned sample count in the ordinary cyclic block, so rendering
  remains explicit and reproducible. This does not claim to reproduce any
  hardware's undocumented automatic-cycle heuristic.
- **Confirmed** — [`SampleHeritageRecordCommitAdaptiveStretchBlock`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  provides an offline decision hop, bounded search radius and stride, crossfade,
  zone, and stereo-link policy. Its neutral 0-to-99 `quality` control scales the
  declared search-radius maximum, while `width` scales the declared crossfade
  maximum. The implementation uses deterministic, DC-removed normalized
  cross-correlation with stable tie-breaking.
- **Probable** — fixed cyclic is a direct neutral mapping of the manual's public
  mechanism. Bounded WSOLA is a defensible, explainable adaptive counterpart;
  it is not a claim to reproduce the undocumented `INTELL` algorithm.
- **Confirmed** — live cyclic stretch is a separate per-voice mechanism with
  factor 0.25 to 20, fixed cycle and splice durations expressed in milliseconds,
  optional deterministic subdivisions, and no adaptive search on the audio
  thread. It has a fixed startup cycle prebuffer; after activation it is causal
  and uses bounded retained history rather than a future-search window.
- **Test** — live fixed-millisecond cycles are a real-time extension, not a
  behavior established by the S1000 manual. Calibrate them from renders or
  lawful captures when a profile needs a specific sound.
- **Confirmed** — live `pitch_mode` has neutral `preserve` and `rate_linked`
  values. The first keeps unit-rate reads inside each cycle; the second scales
  cycle-local reads by the stretch ratio. `tempo_lock` applies the duration
  factor to source consumption when true and makes the live stage
  duration-neutral when false. These are Pulp semantics, not claims about a
  named machine.

## R1-R7 disposition before captures

- **Test (R1)** — Pulp's declared splice law is an output-domain cycle. The
  independent impulse oracle proves the implementation; a hardware factor sweep
  remains optional calibration evidence for whether a particular device agrees.
- **Test (R2)** — zero crossfade is an explicit butt splice and nonzero values use
  Pulp's declared complementary window. The manuals establish crossfade controls
  for adaptive modes but do not publish a device splice curve.
- **Probable (R3)** — bounded normalized-correlation search is an explainable
  adaptive mechanism. Manual `quality` and `width` labels do not reveal enough
  mathematics to claim that it reproduces INTELL.
- **Test (R4)** — arithmetic fingerprints at extreme factors are covered by the
  analytic gates; any device-specific rounding or overflow signature belongs in
  a capture-fit report rather than the neutral schema.
- **Test (R5)** — Pulp exposes linked-channel decisions and proves deterministic
  stereo coherence. Whether a reference device links channels remains a capture
  question.
- **Test (R6)** — converter-before-stretch-before-hold/reconstruction is the
  versioned Pulp ordering contract. Cross-width hardware captures can calibrate
  it later without changing the neutral mechanism.
- **Confirmed/Test (R7)** — the S950 manual establishes D-TIME, AUTO-D, and
  mono/poly choices, but not a cycle unit or algorithm. Pulp therefore records
  the evidence without aliasing D-TIME to `cycle_samples`.

## Schema concerns

1. Keep `quality` and `width` as neutral normalized controls over explicit,
   reproducible maxima. Do not describe their mapping as the undocumented
   algorithm used by any named machine.
2. The offline cycle limit of 1,048,576 samples, live splice limit of 20 ms,
   and subdivision bounds are Pulp resource envelopes, not manual-derived
   historical ranges. Docs should label them accordingly.
3. Do not name the adaptive block `intelligent` or `INTELL`; that would imply an
   undocumented product algorithm. `adaptive` states only the shipped behavior.

## Unresolved calibration questions

- For fixed cyclic, what cycle and crossfade values minimize discontinuity for
  each material class without erasing the intended cyclic character?
- For adaptive mode, what hop, search radius, stride, and linked-channel policy
  are justified by negative controls and boundary-heavy material?
- Should a future live adaptive mode exist at all, and can it meet bounded
  real-time resource rules without hiding search or allocation?
- Do future captures justify additional pitch or tempo modes without changing
  the existing neutral meanings?
