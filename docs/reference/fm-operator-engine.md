# FM/PM operator engine

`<pulp/signal/fm_operator_engine.hpp>` provides the fixed-capacity melodic
operator core shared with Pulp's six- and eight-operator drum routing law. It
supports up to eight sine operators by default, with no allocation or locking
on the audio thread.

Each operator can follow the played frequency by ratio or use an absolute Hz
frequency. Operators also have independent DAHDSR envelopes, linear levels,
key scaling in dB per octave, and bounded self-feedback. Two separate routing
matrices keep the modulation domains explicit:

- phase-modulation depths are radians of phase deviation per unit source
  output;
- frequency-modulation depths are Hz of instantaneous-frequency deviation per
  unit source output.

Every routing edge reads the source's previous sample. That uniform one-sample
delay makes arbitrary cycles deterministic and independent of operator
evaluation order. Carrier gains are linear; gains summing above unity are
normalized by their sum, while a sub-unity sum is intentional attenuation.

The default `bright_band_safe` policy fades routed modulation as a destination
approaches Nyquist, including current FM deviation in that estimate. It is a
conservative bright-note safeguard, not a promise that arbitrary deep PM at a
low fundamental is alias-free. Select `bounded` when preserving requested
modulation is more important; it still clamps instantaneous frequency to the
representable band.

The engine reports zero latency. `tail_samples()` is the exact longest
configured release after `note_off()`. `reset()` restores deterministic initial
phase and envelope state. Operator-count changes also reset the voice so a
disabled operator cannot later resume stale state.

```cpp
#include <pulp/signal/fm_operator_engine.hpp>

pulp::signal::FmOperatorEngine engine;
engine.prepare(48000.0);
engine.set_operator_count(2);
engine.set_operator_ratio(0, 1.0f);  // carrier
engine.set_operator_ratio(1, 2.0f);  // modulator
engine.routing().set_phase_modulation_radians(0, 1, 2.0f);
engine.routing().set_carrier_gain(0, 1.0f);
engine.note_on(0.8f);

float block[128];
engine.process(block, 128);
```

The centralized module index, capability registry, umbrella include, and
catalog exports are integration-owned surfaces and are intentionally not
updated by this leaf implementation.
