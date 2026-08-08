# Modulation language primitives

Pulp's modulation language uses one curve vocabulary for arbitrary breakpoint
segments and purpose-built rise/fall sources. These primitives complement the
existing fixed-shape envelopes in `envelope.hpp`; they do not replace or fork
their AR, AD, AHD, or DAHDSR state machines.

```cpp
#include <pulp/signal/modulation_curve.hpp>
#include <pulp/signal/breakpoint_envelope.hpp>
#include <pulp/signal/rise_fall_generator.hpp>
```

`ModulationCurve` names five shapes: linear, exponential, logarithmic,
smoothstep, and hold. Exponential and logarithmic reuse the same stage law as
the existing modulation toolkit. Their `strength` is dimensionless in `[0, 1]`;
zero becomes linear. Curves interpolate the authored endpoint values without
normalizing them, so the same API can describe hertz, decibels, semitones, or a
unitless modulation depth.

`BreakpointEnvelopeT<SampleType, MaxPoints>` stores an authored program in a
fixed-capacity array. Every point has an absolute `time_ms`, a finite value in
the consumer's real unit, and a curve to the next point. Configuration and
sample-rate changes happen on the control thread. Triggering, sample/block
playback, reset, and inspection are bounded and allocation-free.
Successful control-side reconfiguration resets playback to the start; rejected
configuration leaves both the existing program and playback position intact.

The timing contract is sample-exact: triggering places the output at point
zero, and each call to `next()` advances one sample. A segment rounded to N
samples reaches its endpoint on the Nth call. Equal adjacent times are bounded
instantaneous transitions. The final point is held after playback completes.
Loops emit their end point before wrapping; unequal loop endpoints therefore
request an explicit discontinuity. A repeat count is the number of additional
passes through the selected loop range. An all-zero-duration infinite loop has
no observable time, so it quiesces immediately instead of consuming unbounded
audio-callback work.

`RiseFallGeneratorT` is a three-point specialization of the breakpoint engine,
not a separate envelope implementation. Rise and fall times are milliseconds,
levels retain their caller-owned unit, and optional infinite looping is
continuous because the first and final low levels are identical.

Invalid programs are rejected transactionally. Times must be finite,
non-negative, ordered, begin at zero, and fit within one day. Sample rates must
be finite and in `[1, 768000]` Hz. Values must be finite. Non-finite curve
strength becomes linear, and subnormal outputs snap to zero.
Unknown serialized curve enum values also fall back to linear deterministically.
