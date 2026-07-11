#pragma once

/// @file crossfade.hpp
/// The one shared old→new crossfade gain law for the SIGNAL side.
///
/// Every click-free/gap-free transition that blends an OLD render into a NEW one
/// — the live plugin swap (`TransitionMixer` / `crossfade_plugin_slot`), the
/// convolver IR swapper (`PartitionedConvolver`), the loop-wrap crossfade
/// (`LoopRenderer`), and the live-kernel structural swap — computes its per-frame
/// gain pair HERE instead of hand-rolling cos/sin. Pure math, no state, no
/// allocation, no locks: safe to call per sample on the audio thread.
///
/// A crossfade is fully described by two orthogonal choices:
///
///   1. The PROGRESS SHAPING — how the raw fade fraction `t = pos/len` maps to a
///      shaped progress `u ∈ [0,1]`:
///        - identity (`u = t`)            — a plain ramp; the loop-wrap law.
///        - `crossfade_smoothstep(t)`     — cubic, zero slope at both ends; the
///          live-swap law (click-free at BOTH the swap instant and the fade end).
///
///   2. The GAIN LAW — how the shaped progress `u` splits into (old, new) gains:
///        - `EqualGain`  → (1 − u, u):            amplitude sum == 1. No level
///          bump for CORRELATED old/new (the usual hot-reload: same input, a
///          small DSP tweak); a −6 dB mid dip when decorrelated.
///        - `EqualPower` → (cos(u·π/2), sin(u·π/2)): power sum == 1. Avoids the
///          mid-fade dip when old/new are DECORRELATED (a big DSP change); the
///          −3 dB "constant power" cross-fade.
///
/// Callers compose the two: `crossfade_gains(shaping(t), law)`. Keeping the
/// shaping OUT of `crossfade_gains` is deliberate — it lets the loop-wrap path
/// use the raw ramp and the live-swap path use smoothstep while sharing the one
/// gain law, so all of them stay identical bit-for-bit to their prior hand-rolled
/// math (the loop wrap and live swap only differed in shaping + a duplicated
/// cos/sin, never in the underlying law).

#include <cmath>

namespace pulp::signal {

/// The old→new gain split applied to a shaped progress `u ∈ [0,1]`.
enum class CrossfadeGainLaw {
    EqualGain,   ///< (1−u, u) — amplitude sum == 1.
    EqualPower,  ///< (cos(u·π/2), sin(u·π/2)) — power sum == 1.
};

/// Smoothstep progress shaping: cubic `3u²−2u³` with zero slope at both ends,
/// clamped to [0,1]. `constexpr`/pure — the click-free ramp shared by the
/// live-swap crossfades.
template <typename T>
constexpr T crossfade_smoothstep(T t) noexcept {
    if (t < T{0}) t = T{0};
    else if (t > T{1}) t = T{1};
    return t * t * (T{3} - T{2} * t);
}

/// Old/new blend gains for an ALREADY-SHAPED progress `u` (the caller applies any
/// smoothstep/clamp first). Pure — no state, no allocation. Full-precision π/2 so
/// the double instantiation is exact; at float precision the constant rounds to
/// the same value the callers used before, so every float caller is unchanged.
template <typename T>
inline void crossfade_gains(T u, CrossfadeGainLaw law,
                            T& old_gain, T& new_gain) noexcept {
    if (law == CrossfadeGainLaw::EqualPower) {
        // π/2 to full double precision; `u * kHalfPi` reproduces the callers'
        // `u * 0.5 * π` (u·0.5 is exact, so the single rounding is identical).
        constexpr T kHalfPi = T{1.57079632679489661923};
        const T theta = u * kHalfPi;
        old_gain = std::cos(theta);
        new_gain = std::sin(theta);
    } else {
        old_gain = T{1} - u;
        new_gain = u;
    }
}

}  // namespace pulp::signal
