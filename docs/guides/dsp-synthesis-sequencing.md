# Additive Synthesis, Vocoding, and Modular Sequencing

This guide covers the authoring pieces for additive voices, a channel vocoder,
and clocked modular control. They compose inside a `Processor`; use a
`SignalGraph` only when users can change the routing itself at run time.

## Additive bank

`AdditiveBank` renders up to the prepared partial capacity with a voice table,
inharmonicity, spectral tilt, two morphable spectral envelopes, attack/release,
detuned doublets, pitch glide, and deterministic retrigger phase.

```cpp
#include <pulp/signal/additive_bank.hpp>

pulp::signal::AdditiveBank bank;
bank.prepare(sample_rate, 128);
bank.set_partial_count(32);
bank.set_fundamental_hz(110.0);
bank.set_spectral_tilt_db_oct(-3.0);
bank.set_attack_ms(8.0);
bank.set_release_ms(400.0);
bank.retrigger();
bank.process(output, num_frames); // replaces output
```

`SpectralEnvelope::gain_db_at()` and `AdditiveBank::envelope_db_at()` expose the
configured and realized curves respectively. Use them for plots and tests.

## Vocoder

`Vocoder` owns the analysis and synthesis banks, envelope followers, internal or
external carrier, unvoiced/sibilance path, formant shift/freeze, trim, and
dry/wet mix. `process(modulator, carrier, out_dry)` returns the wet sample and
writes the DC-blocked dry sample used by its mix contract. Band inspection
methods expose the exact filters and gains used by the renderer.

## Modular sequencing

| Type | Header | Purpose |
|---|---|---|
| `StageSeq` | `<pulp/signal/stage_sequencer.hpp>` | Per-stage pitch, pulses, gate modes, slide, skip, and direction |
| `CartesianWalk` | `<pulp/signal/cartesian_walk.hpp>` | Independently clocked X/Y grid walk with cell CV and gate |
| `Rungler` | `<pulp/signal/rungler.hpp>` | Shift-register feedback pattern and DAC output |
| `QuantizeScale` | `<pulp/signal/scale_quantizer.hpp>` | EDO or scale-mask voltage quantization with hysteresis |
| `ProbGate` | `<pulp/signal/probability_gate.hpp>` | One seeded draw per trigger edge |
| `GateLogic` | `<pulp/signal/gate_logic.hpp>` | Boolean or thresholded level-domain gate operations |

All event processors distinguish continuous level from edge events. Compute an
edge once, then pass that boolean to every consumer that should observe the same
event; do not let several processors independently reinterpret one noisy clock.
`apply_reset_edge()` performs the documented live reset, while `reset()` returns
the whole object to deterministic initial state.

See the [modulation toolkit](../reference/modulation-toolkit.md) for clock,
trigger, envelope, VCA, and routing primitives. The
[complete advanced DSP API](../reference/advanced-dsp-api.md#synthesis-and-sequencing)
lists every method on the types above.

