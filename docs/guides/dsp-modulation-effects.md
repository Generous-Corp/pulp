# Modulation Effects

Pulp's modulation effects use distinct mechanisms rather than one overloaded
modulated-delay class. This keeps their latency, phase, stereo, and coloration
contracts visible.

| Family | Header | Main types |
|---|---|---|
| Staged phasing and vibrato | `<pulp/signal/phaser_stages.hpp>`, `<pulp/signal/vibrato.hpp>` | `PhaserStages`, `PhaseVibrato`, `UniVibe`, `DelayVibrato` |
| Chorus and flanging | `<pulp/signal/chorus_family.hpp>`, `<pulp/signal/flanger.hpp>` | `ChorusEnsemble`, `Flanger` |
| Frequency translation | `<pulp/signal/frequency_shifter_ssb.hpp>` | `SsbFrequencyShifter` |
| Rotary and scanner | `<pulp/signal/leslie_rotary.hpp>`, `<pulp/signal/scanner_vibrato.hpp>` | `LeslieRotary`, `ScannerVibrato` |

## Stereo chorus example

```cpp
#include <pulp/signal/chorus_family.hpp>

pulp::signal::ChorusEnsemble chorus;
chorus.prepare(sample_rate);
chorus.set_voicing(pulp::signal::ChorusEnsemble::Voicing::dimension_d);
chorus.set_rate_hz(0.35f);
chorus.set_depth(0.6f);
chorus.set_stereo_width(1.0f);
chorus.set_mix(0.5f);
chorus.process(left, right, num_frames);
```

## Mechanism and latency

- `PhaserStages`, `PhaseVibrato`, and `UniVibe` are all-pass/filter mechanisms;
  they do not acquire the bulk delay of a delay-line vibrato.
- `DelayVibrato`, `ChorusEnsemble`, and `Flanger` own delay history. Call
  `discard_history()` for constant-time hostile-input recovery and `reset()` for
  an ordinary deterministic restart.
- `SsbFrequencyShifter` translates every partial by a fixed number of hertz; it
  is not a pitch shifter. Feedback delay is part of its sound and its reported
  state.
- `LeslieRotary` models independently accelerating horn and drum paths plus
  microphone geometry and reflections. Speed changes are targets, not phase
  resets.
- `ScannerVibrato` scans a delay line and exposes the realized depth and peak
  pitch ratio for visualization or bounds checks.

Do not infer latency from effect category. Query each instance's
`latency_samples()` after preparation and parameter setup. The
[complete advanced DSP API](../reference/advanced-dsp-api.md#modulation-effects)
lists every mode, control, processing overload, and observable.
