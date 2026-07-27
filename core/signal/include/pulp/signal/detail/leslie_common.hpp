#pragma once

namespace pulp::signal {

/// Physical and interpolation constants shared by the rotary and scanner.
namespace leslie_detail {

/// Speed of sound in dry air at 20 °C, m/s. From the ideal-gas approximation
/// `c ≈ 331.3 + 0.606·T(°C)`: `331.3 + 0.606·20 ≈ 343.4`, rounded to 343.
///
/// A compile-time constant rather than a parameter on purpose: `c` varies by
/// ±0.6 m/s per °C, so a 10 °C room swing moves the Doppler depth by ±1.7 %,
/// far below the resolution at which radius and rate are actually voiced.
inline constexpr double kSpeedOfSound = 343.0;

inline constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Extra samples kept beyond the longest modelled read so the 4-point
/// interpolator's `index - 1` and `index + 2` neighbours are always inside the
/// buffer. Four covers the stencil with a sample to spare.
/// [design parameter] default 4, range 2 .. 8 samples.
inline constexpr int kInterpolatorMargin = 4;

}  // namespace leslie_detail

}  // namespace pulp::signal
