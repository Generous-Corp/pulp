# N-way crossfade

`pulp::signal::NWayCrossfadeT<SampleType, MaxPaths>` mixes a bounded, ordered
set of signal paths without allocating or locking in `process()`. It is intended
for morph selectors and other routing controls whose endpoints are discrete
render paths.

## Position and gain law

The position is expressed in path indices. Position `0` selects path 0,
position `1` selects path 1, and so on. Values outside the prepared path range
clamp to the nearest endpoint. At a fractional position, only the two adjacent
paths are active. The coordinate is already shaped: apply any desired easing
before calling `configure()`.

The laws are the shared `CrossfadeGainLaw` laws from `crossfade.hpp`:

- `EqualGain` gives adjacent weights `(1-u, u)`. Their L1 sum is one, so
  identical correlated paths reconstruct without a level bump.
- `EqualPower` gives `(cos(u*pi/2), sin(u*pi/2))`. Their squared sum is one,
  which preserves power for decorrelated paths. Correlated signals can sum as
  high as `sqrt(2)` at the midpoint, so downstream headroom must allow for that.

For two paths these weights are exactly the existing `crossfade_gains()` result.
At every integer endpoint exactly one path has weight one.

## Preparation and real-time contract

Call `configure(path_count, position, law)` on one control thread. The path
count must be between 2 and `MaxPaths`, the position must be finite, and the law
must be a declared enum value. Preparation uses fixed inline storage. A rejected
configuration is not published, so the audio thread continues using the last
valid plan.

Call `process(inputs, output, frames)` on one audio thread. It reads one complete
plan at the block boundary and does no allocation, locking, or I/O. Every
consumed input and the output must cover `frames`. Exact in-place processing is
supported; shifted/partial overlap is rejected before output is modified. The
same plan produces sample-identical output regardless of block partitioning.

The publication primitive is single-writer/single-reader. Multiple control
writers or multiple audio readers require external serialization outside the
real-time path.
