# Fractional delay

`<pulp/signal/fractional_delay.hpp>` provides one prepared mono delay-line
contract with stable first-order Thiran allpass, four-point/order-3 Lagrange,
and six-point/order-5 Lagrange reconstruction. It is intended as a foundation
for waveguides, resonators, and physical models that need an explicit
fractional-delay policy rather than an effect-specific private ring.

There was no general Thiran primitive to promote. Existing Pulp code provides a
linear `DelayLineT`, the shared `Interpolator::lagrange` order-3 kernel, and
effect-private Hermite/Lagrange rings. The new line reuses the shared order-3
kernel and does not replace those established effect internals.

## Processing contract

`prepare(maximum_delay_samples, method)` allocates and fixes the ring capacity
and method. For Thiran at an arbitrary static delay or modulation within a
specific integer interval, use
`prepare_thiran1(maximum_delay_samples, integer_interval_start)`. It prepares
the half-open delay range `[integer_interval_start, integer_interval_start + 1)`.
The general `prepare(..., thiran1)` overload selects the default interval
`[1, 2)`.
Rejected geometry or allocation leaves the previously prepared line usable.
`reset()` clears history and recursive state. No processing or reset operation
allocates.

Processing pushes the current sample before reading. Ring tap zero therefore
means the current sample, although every public method requires a causal minimum
delay. The valid requested ranges are:

| Method | Delay range | Read geometry |
|---|---:|---|
| `thiran1` | prepared `[k, k + 1)` only | fixed tap `k - 1`, then an allpass delay in `[1,2)` |
| `lagrange3` | `[1, prepared maximum]` | taps around integer delay at `{-1,0,+1,+2}` |
| `lagrange5` | `[2, prepared maximum]` | taps around integer delay at `{-2,-1,0,+1,+2,+3}` |

The prepared ring retains the requested maximum plus every older stencil tap;
`required_older_lookback()` reports that geometry. Before prepare, configuration
queries report zero/`false`; `method()` and
`thiran_integer_interval_start()` return `std::nullopt`. Processing itself has
zero host-compensated latency: the requested delay is the audible signal-path
delay, and its result is returned during the same call.

Values within eight floating-point epsilon-scaled units of an integer are
canonicalized to that integer. This makes exact-tap behavior deterministic
after ordinary control arithmetic. The line supports exact in-place blocks. A
nonzero block with a null input, output, or delay array returns
`invalid_argument` without advancing or writing output.

## Retuning and faults

Lagrange delay may change on every sample across the full prepared range.
Thiran-1 may change on every sample only inside its prepared integer interval.
Its recursive transfer function changes discontinuously if the integer tap
changes, so the exact/canonicalized upper endpoint and all values outside the
interval fail with `invalid_delay`. There is no hidden smoother or crossfade.
To move Thiran across an integer boundary, callers must prepare two lines for
the adjacent intervals and crossfade them at the composition layer. This keeps
the primitive's state and transfer contract explicit.

Per-sample and block calls return typed status. A nonfinite sample, invalid
delay, or derived value outside the sample type injects zero for that timeline
position, emits zero, and clears the recursive Thiran state. Later finite input
therefore recovers deterministically without retaining a NaN or infinity in the
ring. A block continues after faults, reports the first fault status, and counts
all faulted frames.

## Thiran design surface

The public pure `design_thiran1()` helper exposes:

```text
H(z) = (a + z^-1) / (1 + a z^-1)
a = (1 - D) / (1 + D), D in [1,2]
```

The pole is at `-a`; this range keeps its radius at or below `1/3`. Magnitude is
exactly unity. `thiran1_group_delay_samples()` exposes the exact
frequency-dependent group delay so physical-model tuning and visualizations do
not need to rederive it. Its DC group delay is `D`; the response becomes
frequency-dependent away from DC, which is the allpass tradeoff rather than an
error.

Lagrange interpolation is FIR and follows the requested fractional delay more
uniformly across frequency, but its magnitude is not exactly unity. Order 5
reduces high-frequency approximation error at the cost of two more taps and a
two-sample causal minimum. `lagrange5_weights()` and `lagrange5()` expose the
pure six-tap design/value surface; order 3 delegates to the existing
`Interpolator::lagrange` helper.
