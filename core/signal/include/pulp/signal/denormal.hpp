#pragma once

#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Denormal-flushing utility.
///
/// Denormal floating-point values cause severe CPU stalls on x86 when
/// they enter recursive paths like IIR filter state or reverb feedback.
/// `snap_to_zero` returns 0 when the input is below a threshold whose
/// magnitude is large enough to catch denormals (and very-quiet tails)
/// but small enough not to be audible. The threshold is 1e-15 (~-300 dB
/// FS for `float`; see `snap_threshold()` below) — the single source of
/// truth for the magnitude quoted here.
///
/// Use at the end of each recursive state update in IIR / SVF / TPT /
/// Ladder / Ballistics / Reverb feedback paths.
///
/// @code
/// last_out_ = pulp::signal::snap_to_zero(coef * last_out_ + input);
/// @endcode
///
/// Build-time toggle: define PULP_DSP_ENABLE_SNAP_TO_ZERO=0 to compile
/// out the check (the function becomes the identity). Default: ON.
#ifndef PULP_DSP_ENABLE_SNAP_TO_ZERO
#define PULP_DSP_ENABLE_SNAP_TO_ZERO 1
#endif

/// Threshold in absolute value below which a sample is treated as zero.
/// 1e-15f corresponds to about -300 dB FS for `float` — well below any
/// audible level and well above the denormal range (~1.4e-45f).
template<typename T>
inline constexpr T snap_threshold() { return T(1e-15); }

/// Returns 0 if |x| < snap_threshold, else x. Branchless on most compilers
/// (compiles to a `cmpltps` + `andps` pair on x86-SSE).
///
/// RT contract: `snap_threshold()`, scalar/buffer `snap_to_zero()`, and
/// `is_denormal()` are allocation-free. `is_denormal()` is intended for
/// diagnostics, not audio hot paths.
template<typename T>
inline T snap_to_zero(T x) {
#if PULP_DSP_ENABLE_SNAP_TO_ZERO
    return (x < snap_threshold<T>() && x > -snap_threshold<T>()) ? T(0) : x;
#else
    return x;
#endif
}

/// Snap an entire buffer in place.
template<typename T>
inline void snap_to_zero(T* samples, int n) {
#if PULP_DSP_ENABLE_SNAP_TO_ZERO
    for (int i = 0; i < n; ++i)
        samples[i] = snap_to_zero(samples[i]);
#else
    (void) samples;
    (void) n;
#endif
}

/// Return true if @p x is a denormal (subnormal) float.
/// Useful for diagnostics; not for hot paths.
template<typename T>
inline bool is_denormal(T x) {
    return x != T(0) && std::fpclassify(x) == FP_SUBNORMAL;
}

} // namespace pulp::signal
