# Pitch, Time, and Granular Processing

These processors cover four different jobs: delay-tap pitch shifting, pitch
tracking and diatonic harmony, cyclic resampling artifacts, and grain-cloud
construction. They are not interchangeable.

| Processor | Header | Job |
|---|---|---|
| `PitchShifter` | `<pulp/signal/pitch_shifter.hpp>` | Pedal-law or direct-semitone time-domain shifting |
| `YinTracker` | `<pulp/signal/yin_tracker.hpp>` | Monophonic fundamental estimate with confidence and declared analysis latency |
| `HarmonyEngine`, `DiatonicMap` | `<pulp/signal/harmony_engine.hpp>` | Two diatonic voices over tracking, mapping, shifting, glide, and mute policy |
| `CyclicStretch` | `<pulp/signal/cyclic_stretch.hpp>` | Cyclic/granular timestretch whose flutter and splices are deliberate character |
| `GranularEngine` | `<pulp/signal/granular.hpp>` | Deterministic resident-buffer or live-ring grain clouds |

## Direct pitch shift

```cpp
#include <pulp/signal/pitch_shifter.hpp>

pulp::signal::PitchShifter shift;
shift.prepare(sample_rate);
shift.set_shift_source(pulp::signal::ShiftSource::direct);
shift.set_shift_semitones(7.0);
shift.set_window_ms(40.0);
shift.set_glide_ms(25.0, 40.0);
shift.set_mix(1.0);

for (int i = 0; i < num_frames; ++i)
    mono[i] = shift.process(mono[i]);
```

`PitchShifter::latency_samples()` is a throughput/PDC contract for its moving
taps. `HarmonyEngine::latency_samples()` additionally includes tracker and dry
alignment; use the component latency accessors when explaining that total.

## Source ownership and real-time rules

- `GranularEngine::set_buffer()` publishes caller-owned resident sample data.
  Keep that storage alive until processing has stopped or another source has
  been installed. `write_live()` feeds the engine-owned live ring.
- Set maximum grain count and topology before the audio callback. Runtime grain
  scheduling and `process()` are bounded after `prepare()`.
- `CyclicStretch::prepare()` sizes its capture and grain storage. The scheduling
  accessors are pure observability points for editors and tests; do not duplicate
  the schedule in UI code.
- `YinTracker` is monophonic analysis, not a general polyphonic pitch detector.
  Check `voiced()` and `min_cmnd()` before using `f0_hz()`.
- Seeds make renders repeatable. Keep seed changes explicit in presets and test
  fixtures.

The [complete advanced DSP API](../reference/advanced-dsp-api.md#pitch-time-and-granular)
lists every lifecycle, configuration, processing, scheduling, and inspection
method.
