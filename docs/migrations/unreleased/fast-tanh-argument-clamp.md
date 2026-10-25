---
breaking = false
summary = "FastMath::tanh clamps its argument where the Padé reaches 1: continuous, 9.6e-5 worst error (was 6.7e-4), faster; the ladder's realtime_efficient saturator uses the Estrin form of the minimax fit"
---

## What changed

- `FastMath::tanh(x)` used to saturate its output to exactly ±1 for
  |x| > 4. That left a 6.7e-4 step at the clamp. It now clamps the
  *argument* to ±`FastMath::kTanhPadeClamp` (4.9717869), the point where its
  [7/6] Padé reaches 1.
  - The curve is continuous, and its worst error over every float is 9.6e-5.
  - Its numerator is scaled by 1 − 2⁻²¹, so results stay strictly inside
    ±1. Beyond the clamp the result is ±0.9999996, not ±1.
  - Outputs for |x| ≤ 4 move by at most 5e-7. Outputs between 4 and 5 move
    by up to 6.7e-4 (toward `std::tanh`).
  - A NaN input still returns NaN.
  - It costs about 14% less per element than before.
- `LadderSaturation::realtime_efficient` (and so
  `LadderFilter::process_realtime_efficient()` and the tom voice's
  `realtime_efficient` noise ladder) now evaluates the new
  `FastMath::tanh_minimax_recursive()`. That is the same fit, clamp and bound
  as `FastMath::tanh_minimax()`, in Estrin form. Its results differ from
  before by a few float ulps, within the same 3.8e-6 bound. It removes a
  2.6x slowdown of this profile inside polyphonic voice loops.

## Migration steps

- Code that compared `FastMath::tanh(x)` to exactly ±1 for large |x| should
  compare against `FastMath::tanh(FastMath::kTanhPadeClamp)`, or test
  `std::abs(y) > 1 - 5e-7f`.
- No change is needed for `tanh_minimax()`, `tanh_accurate()` or the default
  ladder saturator.
