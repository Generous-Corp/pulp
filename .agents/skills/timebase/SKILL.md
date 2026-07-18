---
name: timebase
description: Pulp musical/media time primitives, immutable compiled tempo maps, monotonic transport beats, exact sample anchors, corrected inverse conversion, and shared transport quantization arithmetic.
---

# Timebase

Use this skill when changing `core/timebase`, tempo conversion, or the transport
quantizer's beat/frame arithmetic.

## Contracts

- `kTicksPerQuarter` is 705,600. Musical positions are stored and accumulated as
  integer `TickPosition`; samples use integer `SamplePosition`. Tick-position,
  duration, and `MonotonicBeat` arithmetic saturates at the signed 64-bit
  endpoints; it must not invoke signed-overflow UB.
- `CompiledTempoMap` is immutable and sample-rate-specific. Its first tempo point
  is tick zero, points are strictly ordered, and BPM is finite in `[1, 1000]`.
- Construction preserves its established `std::invalid_argument` contract in
  native exception-enabled builds. WAM/WebCLAP compile the same TU without an
  exception runtime; invalid constructor input fails closed there, so web-facing
  decode/model paths must validate tempo points before constructing the map.
- Tempo ramps are BPM-linear in tick position. Integrate them analytically; do not
  approximate ramps block-by-block or accumulate floating-point deltas.
- Every segment begins at an integer sample anchor. `samples_to_ticks()` returns
  the first canonical tick mapping to that sample when one exists. Exact sample
  -> tick -> sample requires a tick grid at least as dense as samples. On a
  sparser grid, use `resolve_sample()` and inspect `represented_sample`,
  `absolute_error_samples`, and `exact`; the nearest tick is returned.
- Arbitrary tick -> sample -> tick cannot be identity because many ticks share an
  integer sample. Test monotonicity and canonical-sample preservation instead.
- Keep `TransportQuantizer`'s public behavior stable. Generic beat/frame/grid
  arithmetic belongs in `<pulp/timebase/quantize.hpp>` and the format wrapper
  delegates to it.

## Validation

Build and run `pulp-test-timebase` and the existing
`pulp-test-transport-quantizer` oracle. Keep at least 1,000,000 deterministic
randomized constant/ramp cases, plus tempo-point boundary cases.

`MonotonicBeat` is the strong type for the transport's non-looping musical
clock; the transport owns how it advances while timeline positions seek or
wrap. `MeterMap` is not part of the initial module.
