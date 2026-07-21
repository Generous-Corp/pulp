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

## Five-machine evidence

- **Confirmed — SP-1200.** The [owner's manual](https://www.autistici.org/2000-maniax/texts/sp1200_manual.pdf),
  page 12, defines three output classes: channels 1-2 have time-varying-bandwidth
  filters, channels 3-6 have fixed filtering, and channels 7-8 are unfiltered.
  The [service manual](https://archive.org/details/emu-sp-1200-service-manual-1987_202010)
  is the circuit authority for the input and output stages. **Test** — capture
  each physical output; the manuals do not give enough coefficients to select a
  Pulp approximation family and order from prose alone.
- **Confirmed — S950.** The [operator's manual](https://manuals.fdiskc.com/flat/Akai%20S-950%20Owners%20Manual.pdf),
  pages 19 and 40-42, exposes record bandwidth from 3,000 to 19,000 Hz and a
  per-keygroup low-pass tone control with its own envelope. **Test** — separate
  record anti-alias response, pitch-linked reconstruction response, and the
  programmable tone filter; the manual's control values do not identify their
  analog topology or a Butterworth/Chebyshev/elliptic family.
- **Confirmed — S1100.** The [service-manual specifications](https://manualzz.com/doc/6408169/akai-s1100-digital-sampler-service-manual)
  identify a digital moving low-pass filter at -18 dB/octave. The same page
  gives 20 Hz-20 kHz response at 44.1 kHz sampling and 20 Hz-10 kHz at
  22.05 kHz sampling. **Test** — determine the moving filter's coefficient law,
  resonance behavior, and the separate record/reconstruction responses.
- **Test — S612.** The [service manual and schematics](https://www.florian-anwander.de/akai_s612/Akai_S-612_Service_Manual_Part_2.pdf)
  are sufficient to trace the analog stages, but the source set does not state
  a named mathematical response family. Trace component values and measure
  swept response at each sampling-clock setting before selecting fixed-hertz or
  machine-rate-relative behavior.
- **Confirmed — S-550.** The [service notes](https://archive.org/details/roland_S-550_SERVICE_NOTES),
  pages 7-9, describe a digital TVF that returns filtered 16-bit waveform data
  before D/A conversion; the parts list and analog schematic identify PFB-3 LC
  filters with 13.7 kHz and 14.5 kHz corners. Those are distinct stages.
  **Test** — measure the TVF control law and identify which LC corner belongs to
  record versus playback before encoding either one in a profile.

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
- **Confirmed** — the optional analog-color filter delegates to Pulp's existing
  fixed-memory nonlinear four-pole ladder module, then applies the block's
  asymmetric drive/VCA mix. `none` preserves the waveshaper-only path. This is
  a neutral implementation topology and does not assert OTA, SSM, or any named
  product's circuit behavior.
- **Confirmed** — the ladder profile envelope is bounded to cutoff values from
  1 Hz through 0.49 times machine rate and resonance from 0 through 0.3. Its
  ten-second tail bound covers both impulse and saturated step-release probes
  across the accepted envelope; values outside it fail profile validation.
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
