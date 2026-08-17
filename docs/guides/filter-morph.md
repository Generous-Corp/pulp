# Stable Filter Morph

`pulp::signal::FilterMorphT` is a fixed-state processor that morphs between two
second-order filter endpoints. Each endpoint can be a low-pass, band-pass,
high-pass, or notch with its own frequency and Q.

```cpp
#include <pulp/signal/filter_morph.hpp>

pulp::signal::FilterMorph filter;
using Type = pulp::signal::MorphFilterType;

filter.configure(48000.0f,
                 {Type::lowpass, 800.0f, 0.707f},
                 {Type::highpass, 2400.0f, 0.9f});
filter.set_morph(0.35f);
filter.process_block(samples, frames);
```

## Stability and gain policy

The morph is a linear-amplitude parallel blend of two complete, independently
stable RBJ biquads. It never interpolates recursive coefficients. Amount `0`
and `1` take exact endpoint outputs; both filters still advance at every sample,
so changing the amount is independent of block partitioning. The convex blend
preserves the endpoint filters' canonical unity passband or unity peak
normalization and adds no midpoint boost, limiter, or makeup gain.

`configure()` validates both endpoints before changing either live filter.
Frequencies must be from 20 Hz through 45% of sample rate, Q must be from 0.1
through 20, and sample rate must be finite, positive, and no greater than 384
kHz. Rejected configurations and morph amounts leave the previous values
unchanged. Configure or retune at a block boundary; coefficient changes are
stable and transactional, but deliberately are not sample-smoothed.

## Runtime contract

Construction, configuration, morph changes, sample/block processing, response
queries, and reset use fixed scalar storage and allocate no memory. Blocks may
be separate input/output or in-place, and arbitrary block partitioning is
deterministic. A non-finite input resets both recursive states and emits zero.

The filter has zero algorithmic latency. As an IIR it has an asymptotic,
formally unbounded tail, reported by `tail_samples() == -1`. `magnitude()`,
`magnitude_db()`, and caller-owned `response_curve_db()` evaluate the exact
complex parallel response of the live endpoints and morph amount.

This processor is an audio-filter topology morph. It is not preset-state
morphing, parameter smoothing, source-filter formant tracking/shifting, a
formant peak bank, or an explicit-Q resonator/constant-Q analysis primitive.
