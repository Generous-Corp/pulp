---
breaking = false
summary = "FastMath::tanh is a continuous [5/4] constrained minimax clamped at ±5.24: 5.7e-5 worst error (was 6.7e-4), about 30% cheaper; the ladder's realtime_efficient saturator uses the Estrin form of the minimax fit"
---

## What changed

- `FastMath::tanh(x)` was the [7/6] Lambert Padé with its output saturated to
  exactly ±1 for |x| > 4. That left a 6.7e-4 step at |x| = 4. It is now an
  odd [5/4] rational: the minimax fit of tanh in absolute error on [0, 5.24],
  constrained so that its value reaches 1 at 5.24. Its argument is clamped to
  ±`FastMath::kTanhClamp` (5.24).
  - The curve is continuous and bounded.
  - Its numerator is scaled by 1 − 2⁻²¹, so results stay strictly inside ±1.
    Beyond the clamp the result is about ±0.9999996, not ±1.
  - The slope at the origin is 0.99976, not 1.
  - Because the error is now equiripple, results anywhere in (-5.24, 5.24)
    can move by up to about 5.7e-5 relative to before. Between 4 and 5.24
    they move by up to about 6.7e-4, toward `std::tanh`.
  - A NaN input still returns NaN.
  - It costs about 30% less per element than before: nine operations and a
    division, the same count as the unclamped Padé.
- `LadderSaturation::realtime_efficient` now evaluates the new
  `FastMath::tanh_minimax_recursive()`. That covers
  `LadderFilter::process_realtime_efficient()` and the tom voice's
  `realtime_efficient` noise ladder. It uses the same fit, clamp and bound as
  `FastMath::tanh_minimax()`, in Estrin form. Its results differ from before
  by a few float ulps. It removes a 2.6x slowdown of this profile inside
  polyphonic voice loops.

## Measured error

Worst absolute error against double `std::tanh`, over every finite float:

| Function | With fused multiply-add | Without (GCC, MSVC, x86-64 without FMA) | Largest result |
|---|---|---|---|
| `FastMath::tanh`, before | 6.71e-4 | 6.71e-4 | exactly 1 |
| `FastMath::tanh`, now | 5.69e-5 | 5.69e-5 | 0.99999964 (FMA), 0.99999958 (no FMA) |
| `FastMath::tanh_minimax` (unchanged) | 3.70e-6 | 3.72e-6 | exactly 1 |
| `FastMath::tanh_minimax_recursive` (new) | 3.67e-6 | 3.70e-6 | exactly 1 |

All four functions are odd.

## Migration steps

- Code that compared `FastMath::tanh(x)` to exactly ±1 for large |x| should
  compare against `FastMath::tanh(FastMath::kTanhClamp)`.
- Code that relied on `FastMath::tanh` matching the Lambert Padé below
  |x| = 4 should evaluate that fraction itself.
- No change is needed for `tanh_minimax()`, `tanh_accurate()` or the default
  ladder saturator.
