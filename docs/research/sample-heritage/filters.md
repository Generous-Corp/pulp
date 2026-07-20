# Reconstruction and record filters

## Evidence

- **Confirmed** — TI's [*Basic Introduction to Filters*](https://www.ti.com/lit/an/snoa224a/snoa224a.pdf),
  sections 1.4-1.5, distinguishes standard approximation families by response:
  Butterworth is maximally flat, Chebyshev trades passband ripple for a sharper
  transition, and elliptic uses passband and stopband ripple plus stopband zeros
  for the sharpest transition at a given order.
- **Confirmed** — the same source specifies an elliptic low-pass using filter
  order, passband ripple, and stopband attenuation. Those parameters identify a
  mathematical approximation, not a particular analog circuit or component set.
- **Confirmed** — TI's [filter-response application report](https://www.ti.com/lit/an/sloa063/sloa063.pdf),
  section 3.1.1, warns that family names imply different ripple and transition
  behavior. A family name and nominal corner alone do not establish phase,
  component tolerances, or an installed sampler's response.
- **Confirmed** — a zero-order DAC output contains spectral images and sinc
  roll-off; TI's [DAC output-response note](https://www.ti.com/content/dam/videos/external-videos/en-us/2/3816841626001/5577272810001.mp4/subassets/TIPL4705-DAC-Output-Response.pdf)
  shows that the images need filtering during reconstruction.

## Mapping to the neutral mechanism

- **Confirmed** — the voice and record schemas offer `one_pole`, `butterworth`,
  `chebyshev`, and `elliptic`, with cutoff in hertz or as a machine-rate ratio.
  The standard families accept even orders from 2 through 16; `one_pole` is
  order 1. Chebyshev additionally uses passband ripple, while elliptic uses
  passband ripple and stopband attenuation.
- **Confirmed** — [`SampleHeritageVoiceDsp`](../../../core/audio/include/pulp/audio/sample_heritage_voice_dsp.hpp)
  realizes the named responses as digital IIR sections after the modeled hold.
  [`record_commit`](../../../core/audio/src/sample_heritage_record_commit.cpp)
  reuses that mechanism before publishing the committed asset.
- **Probable** — the four family names are defensible neutral DSP vocabulary.
  They describe response classes only; they do not prove analog topology or
  device identity.
- **Test** — `fixed_hz` is suitable when a measured corner stays fixed.
  `machine_rate_ratio` is suitable when measurements across clock settings show
  the corner following machine rate. Do not choose between them from reputation.
- **Test** — order, ripple, and attenuation should be fitted against measured
  magnitude and, where available, phase or impulse response. Listening alone
  cannot uniquely identify the family.

## Schema concerns

1. `cutoff_value` needs a normative definition for each family: for example,
   Butterworth's -3 dB natural frequency versus a Chebyshev passband edge.
   "Cutoff" is not identical across approximation families.
2. The even-order 2-to-16 restriction is a Pulp implementation envelope, not a
   property of Butterworth, Chebyshev, or elliptic theory. Docs should not imply
   that odd orders are historically impossible.
3. A digital IIR response can approximate measured magnitude behavior but does
   not by itself model analog component tolerance, saturation, noise, or
   time variance. Those belong in separate blocks or evidence.

## Unresolved calibration questions

- Which domain owns each measured filter: record input, per-voice reconstruction,
  or post-mix output?
- Is the reported corner a -3 dB point, a passband edge, or a control-panel
  label with another transfer law?
- Do clock sweeps support fixed-hertz or machine-rate-relative cutoff?
- Do impulse and swept-sine evidence support the chosen family and order, or
  only a generic measured response that needs a future coefficient-based form?
