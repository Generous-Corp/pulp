---
name: timebase
description: Pulp musical/media time primitives, editable tempo and meter maps, immutable compiled lookup, streaming cursors, exact sample anchors, and shared transport quantization arithmetic.
---

# Timebase

Use this skill when changing `core/timebase`, tempo conversion, or the transport
quantizer's beat/frame arithmetic.

## Contracts

- `kTicksPerQuarter` is 705,600. Musical positions are stored and accumulated as
  integer `TickPosition`; samples use integer `SamplePosition`. Tick-position,
  duration, and `MonotonicBeat` arithmetic saturates at the signed 64-bit
  endpoints; it must not invoke signed-overflow UB.
- `TempoMap` and `MeterMap` are editable document values built through
  nonthrowing factories. Their first point is tick zero and points are strictly
  ordered. Meter changes must compile on exact preceding bar boundaries.
- `CompiledTempoMap` is immutable and sample-rate-specific. Construct it only
  through `CompiledTempoMap::compile()` and handle `TempoMapError`; public
  throwing construction is forbidden. Its first tempo point
  is tick zero, points are strictly ordered, and BPM is finite in `[1, 1000]`.
- Tempo ramps are BPM-linear in tick position. Integrate them analytically; do not
  approximate ramps block-by-block or accumulate floating-point deltas.
- Every segment begins at an integer sample anchor. `samples_to_ticks()` returns
  the first canonical tick mapping to that sample when one exists. Exact sample
  -> tick -> sample requires a tick grid at least as dense as samples. On a
  sparser grid, use `resolve_sample()` and inspect `represented_sample`,
  `absolute_error_samples`, and `exact`; the nearest tick is returned.
- Arbitrary tick -> sample -> tick cannot be identity because many ticks share an
  integer sample. Test monotonicity and canonical-sample preservation instead.
- `TempoCursor` is the allocation-free playback path. Monotonic sample advances
  consume segment transitions once (amortized O(1)); seeks and loop wraps reset
  it explicitly. Differential tests must match cold-map canonical results.
- `CompiledMeterMap` uses zero-based bars and exact integer bar/tick conversion.
  Tempo changes never affect bar conversion and meter changes never affect
  tick/sample conversion.
- Keep `TransportQuantizer`'s public behavior stable. Generic beat/frame/grid
  arithmetic belongs in `<pulp/timebase/quantize.hpp>` and the format wrapper
  delegates to it.

## Validation

Build and run `pulp-test-timebase` and the existing
`pulp-test-transport-quantizer` oracle. Keep at least 1,000,000 deterministic
randomized constant/ramp cases, plus tempo-point boundary cases.

`MonotonicBeat` is the strong type for the transport's non-looping musical
clock; the transport owns how it advances while timeline positions seek or
wrap.
