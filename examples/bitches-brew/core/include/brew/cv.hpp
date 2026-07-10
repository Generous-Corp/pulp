#pragma once

// The suite's control-voltage convention, in one place.
//
// A plug-in never knows about volts. Full-scale voltage is a property of the
// audio interface and differs between devices, and sometimes between outputs on
// one device. So every plug-in emits a **normalized** sample in [-1, +1] and
// leaves the volts to the hardware.
//
// Two per-instance knobs sit between the DSP and the jack, and every plug-in in
// the suite exposes them identically:
//
//   Output Scale — attenuate toward zero, to calibrate against the interface's
//                  actual full-scale voltage.
//   Invert       — flip polarity, because some interfaces wire their outputs
//                  reversed. Without it the suite is unusable on those.
//
// Everything downstream of `resolve_output` must be bit-exact. No smoothing, no
// dithering, no DC blocking: a change that is inaudible in an audio path is a
// wrong voltage in a CV path.

#include <algorithm>

namespace pulp::examples::brew {

/// Full-scale, normalized. A CV plug-in never emits outside [-kFullScale, +kFullScale].
inline constexpr float kFullScale = 1.0f;

/// The suite's shared output stage: clamp, attenuate, then optionally invert.
///
/// Clamping is not cosmetic. A sample above the interface's rail is a *clipped*
/// voltage, not a louder one, and a plug-in that emits 2.0 has silently handed
/// the user a wrong number rather than a loud one.
///
/// `scale` is clamped to [0, 1] as well: a negative scale would be a second,
/// hidden polarity inversion fighting `invert`.
[[nodiscard]] inline constexpr float resolve_output(float value,
                                                    float scale,
                                                    bool invert) noexcept {
    const float scaled = std::clamp(value, -kFullScale, kFullScale) *
                         std::clamp(scale, 0.0f, 1.0f);
    return invert ? -scaled : scaled;
}

/// Interpret a normalized [0,1] parameter as a boolean toggle.
///
/// Pulp parameters are floats, and a host is free to hand back 0.499998 for what
/// the UI shows as "off". Every plug-in in the suite must round the same way, or
/// two plug-ins disagree about the same saved value.
[[nodiscard]] inline constexpr bool as_toggle(float param) noexcept {
    return param >= 0.5f;
}

}  // namespace pulp::examples::brew
