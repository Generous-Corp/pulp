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
  25%-to-2000% range.
- **Confirmed** — [`SampleHeritageRecordCommitAdaptiveStretchBlock`](../../../core/audio/include/pulp/audio/sample_heritage_schema.hpp)
  provides an offline decision hop, bounded search radius and stride, crossfade,
  zone, and stereo-link policy. The implementation uses deterministic,
  DC-removed normalized cross-correlation with stable tie-breaking.
- **Probable** — fixed cyclic is a direct neutral mapping of the manual's public
  mechanism. Bounded WSOLA is a defensible, explainable adaptive counterpart;
  it is not a claim to reproduce the undocumented `INTELL` algorithm.
- **Confirmed** — live cyclic stretch is a separate per-voice mechanism with
  factor 0.25 to 20, fixed cycle and splice durations expressed in milliseconds,
  optional deterministic subdivisions, and no adaptive search on the audio
  thread.
- **Test** — live fixed-millisecond cycles are a real-time extension, not a
  behavior established by the S1000 manual. Calibrate them from renders or
  lawful captures when a profile needs a specific sound.
- **Confirmed** — `pitch_mode` and `tempo_lock` are intentionally absent because
  the cited sources do not define interoperable meanings for them. Captures are
  optional calibration evidence, not a prerequisite for the neutral API.

## Schema concerns

1. Keep the opaque manual labels `quality` and `width` out of the schema until
   evidence defines what they control. The explicit WSOLA parameters are more
   reproducible.
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
- Do future sources define `pitch_mode` or `tempo_lock` precisely enough for a
  versioned schema addition and migration rule?
