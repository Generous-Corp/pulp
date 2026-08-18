# Early reflections

`<pulp/signal/early_reflections.hpp>` provides the wet-only
`EarlyReflectionsT` renderer for sparse, caller-authored reflection patterns. It
is a fixed-capacity true-stereo signal primitive, not a room model: room
dimensions, source/listener geometry, image-source construction, per-path
filtering, diffusion, and late-reverb policy remain with the caller or a higher
layer.

Each of the 16 tap slots has a fractional delay, signed gain, pan, and stereo
width. Pan is the centre of the reflected image. Width separates the delayed
left and right sources around that centre; the default `pan = 0, width = 1`
preserves left and right identity, while width zero collapses both sources to
the selected pan position. Routing uses the shared equal-power panner, and
fractional timing uses the shared linearly interpolating delay history.

## Headroom contract

`configure()` accepts the same explicit `MatrixHeadroomPolicy` used by Pulp's
audio routing matrix:

- `NormalizePeak` (default) scales each output row by
  `1 / max(1, sum(abs(route gains)))`. For inputs bounded to ±1, each wet output
  is therefore bounded to ±1 without clipping.
- `Raw` applies every signed tap gain exactly. It is useful when a caller owns
  downstream gain staging, and it can exceed full scale.

Normalization is computed once when the complete tap set is published. It does
not run an automatic level detector and does not change with the input signal.

## Lifecycle and timing

`prepare(sample_rate, maximum_delay_ms)` allocates two delay histories
transactionally. `configure(span, policy)` validates and publishes the entire
tap set transactionally; failure leaves the current configuration and history
unchanged. Both are control-thread operations. After preparation,
`process_sample()`, `process_block()`, and constant-time `reset()` allocate no
memory and take no locks. Block processing is deterministic across partitions
and supports same-channel in-place buffers.

The renderer has zero algorithmic latency because it is a wet send rather than
a delayed direct path. `tail_samples()` is the latest audible tap rounded up,
including the second sample of a fractional linear-interpolation read. An empty,
unprepared, or fully muted pattern reports zero tail. Non-finite input clears
both histories and emits silence for the faulting sample, so invalid state does
not persist into later blocks.

Example:

```cpp
using ER = pulp::signal::EarlyReflections;
ER reflections;
const std::array taps{
    ER::Tap{.delay_ms = 11.3, .gain = 0.45, .pan = -0.4, .stereo_width = 0.7},
    ER::Tap{.delay_ms = 23.8, .gain = 0.30, .pan = 0.5, .stereo_width = 0.5},
};
reflections.configure(taps);
reflections.prepare(48000.0, 100.0);
```
