---
breaking = false
summary = "Per-sample filter recursions (TptFilter, Biquad, Svf, Linkwitz-Riley, TwoPoleResonator, BallisticsFilter, WaveguideReflectionFilter) no longer snap their state every sample on 64-bit ARM, where subnormals are free; output only changes in tails below 1e-15, and per-sample TptFilter runs about 2.4x faster on M5"
---

## What changed

On 64-bit ARM (`__aarch64__` / `_M_ARM64`), these per-sample paths no longer
snap their recursive state to zero below 1e-15 on every sample:

- `TptFilter::process_lowpass(x)` and `TptFilter::process(x)`
- `Biquad::process(x)`
- `Svf::process(x)`
- the Linkwitz-Riley TPT stage's per-sample `process`
- `TwoPoleResonator::process`
- `BallisticsFilter::process(x)`
- `WaveguideReflectionFilter::process(x)`

64-bit ARM cores handle subnormal arithmetic in hardware at full speed, so the
snap only cost time. Its compare and select sat in the recursion's dependency
chain, and dropping it makes per-sample `TptFilter` calls about 2.4x faster on
M5 Max. x86 and other targets snap exactly as before.

The rule is the new `pulp::signal::kSubnormalsAreFree` constant and the
`snap_recursion_state()` helper in `denormal.hpp`. `snap_to_zero()` itself is
unchanged, and block paths still snap once per `kSnapChunkSamples` chunk on
every target.

## Effect on output

- On 64-bit ARM the output is bit-identical to before until a state value
  first falls below 1e-15 (about -300 dB).
- From then on, a decaying tail continues through the subnormal range to zero
  instead of stopping at zero. The difference from the old output stays at
  the tail's own scale: below 1e-13 (about -260 dB) in the measured lowpasses
  from 100 Hz to 8 kHz.
- x86 and other targets are unchanged.

## Migration steps

None for plug-ins. Every Pulp plug-in and graph entry point already runs its
audio under `ScopedFlushDenormals`, which flushes subnormals in hardware.

A per-sample caller outside a plug-in entry (an offline tool, a test or a
standalone loop) that needs subnormal-free state on every target should hold a
`ScopedFlushDenormals` across its processing loop.
