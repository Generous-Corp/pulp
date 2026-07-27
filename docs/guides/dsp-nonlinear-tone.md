# Nonlinear and Tone Processors

These processors cover memoryless saturation, circuit clipper primitives,
two-transistor fuzz, tape, and speaker behavior. Pick the smallest class that
owns the behavior you need instead of treating every coloration task as one
generic distortion.

| Processor | Header | Role |
|---|---|---|
| `Saturator` | `<pulp/signal/saturator.hpp>` | General waveshaping with drive, bias, tone, alias policy, mix, and trim |
| `DiodeClipper`, `FeedbackClipper`, `ToneStack` | `<pulp/signal/distortion.hpp>` | Circuit-oriented clipping and surrounding pre/post tone stages |
| `FuzzPair` | `<pulp/signal/fuzz_pair.hpp>` | Coupled two-stage fuzz with device, source-loading, starvation, and seeded drift controls |
| `TapeMachine` | `<pulp/signal/tape_machine.hpp>` | Stereo record/playback chain with archetype, speed, EQ, bias, age, crosstalk, companding, and print-through |
| `SpeakerModel` | `<pulp/signal/speaker_cabinet.hpp>` | Driver, cabinet, breakup, compression, microphone geometry, and diffraction |

## General saturation

```cpp
#include <pulp/signal/saturator.hpp>

pulp::signal::Saturator sat;
sat.prepare(sample_rate);
sat.set_shape(pulp::signal::SaturatorShape::tanh_soft);
sat.set_drive_db(12.0);
sat.set_alias_policy(pulp::signal::SaturatorAliasPolicy::oversample_2x);
sat.set_mix(1.0);

for (int i = 0; i < num_frames; ++i)
    mono[i] = sat.process(mono[i]);
```

Use `shaped(x)` to plot or test the selected memoryless transfer without running
filter or oversampling state. `worst_case_gain()` and `latency_samples()` are the
corresponding bounds for host integration.

## Circuit and system boundaries

- `DiodeClipper` owns a shunt diode node with resistance and capacitance.
  `voltage()`, `resistive_residual()`, and `last_iteration_count()` expose the
  solver rather than forcing tests to infer it from audio.
- `FeedbackClipper` owns the feedback-amplifier topology. Its `linear_gain()` is
  useful for gain staging before the nonlinear solve.
- `ToneStack` deliberately separates `process_pre()` and `process_post()` so a
  graph, plot, or test can observe the two stages independently.
- `FuzzPair` includes its required oversampling. Keep
  `set_oversampling_enabled(false)` confined to measurement tests.
- `TapeMachine` is stereo and may allocate during `prepare()` or while loading
  derived FIR state. Set archetype, speed, EQ, and long-lived topology outside
  the sample loop. Its inspection methods expose the actual record/playback EQ
  and gap filter.
- `SpeakerModel` is a complete mono cabinet/microphone chain. Re-run `prepare()`
  when the sample rate changes, then report `latency_samples()` from the owning
  plugin.

The [complete advanced DSP API](../reference/advanced-dsp-api.md#nonlinear-and-tone)
lists every control, processing overload, and inspection method.
