# Nonlinear Space and Zero-Latency Convolution

Pulp has two advanced space processors with different jobs. `NonlinAmbience`
generates a designed, gated/reverse/nonlinear tap envelope. `ZeroLatencyConvolver`
runs an imported impulse response with a direct head and scheduled partitioned
tail while reporting zero algorithmic latency.

## Nonlinear ambience

```cpp
#include <pulp/signal/nonlin_ambience.hpp>

pulp::signal::NonlinAmbience ambience;
ambience.prepare(sample_rate);
ambience.set_program(pulp::signal::NonlinProgram::gated);
ambience.set_length_ms(900.0);
ambience.set_density_pct(70.0);
ambience.set_width_pct(100.0);
ambience.set_mix_pct(100.0);
ambience.process(left, right, num_frames);
```

Use `set_topology()` on a non-audio thread when applying a coherent snapshot.
Use `request_topology()` for hosted automation; it bounds the audio-thread work
and swaps the prepared tap bank. `envelope(tau)`, `tap()`, `tap_norm()`, and
`worst_case_gain()` expose the actual design for editors and tests.

## Zero-latency convolution

```cpp
#include <pulp/signal/zero_latency_convolver.hpp>

pulp::signal::ZeroLatencyConvolver convolver;
convolver.prepare(sample_rate, max_block, 2);
convolver.set_normalize_mode(pulp::signal::IrNormalizeMode::peak);
convolver.load_impulse_response(ir_channels, ir_channel_count,
                                ir_length, ir_sample_rate);

convolver.process(input_channels, output_channels, num_frames);
```

`prepare()` and `load_impulse_response()` allocate and perform design work; call
both off the audio thread. `process()` accepts any `n <= max_block` and remains
allocation-free. IR resampling, trim/fade, normalization, filters, and true
stereo routing are resolved at load time or through the documented controls.

`latency_samples()` is zero by construction. Predelay is an artistic delay and
is exposed separately through `predelay_samples()`. The per-level scheduling
accessors and `last_block_cost()` are intended for tests, diagnostics, and
budget-aware hosts; normal plugins do not need to schedule partitions themselves.

The [complete advanced DSP API](../reference/advanced-dsp-api.md#space-and-convolution)
lists every method on both processors.

