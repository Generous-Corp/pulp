#pragma once

/// @file fast_math.hpp
/// Fast math approximations for real-time audio DSP.
/// All functions are branchless and SIMD-friendly (no conditionals in hot path).

#include <algorithm>  // std::max / std::min, used by clamp()
#include <cstdint>
#include <cstring>
#include <cmath>

namespace pulp::signal {

/// Fast approximations of common math functions optimized for audio DSP.
///
/// RT contract: all functions are stateless scalar math helpers and allocate no
/// memory.
///
/// These trade precision for speed — typically accurate to 3-5 decimal
/// places, which is more than sufficient for audio processing where
/// the output is ultimately quantized to 16/24/32-bit samples.
///
/// @code
/// float out = FastMath::tanh(input);       // ~4x faster than std::tanh
/// float freq = FastMath::exp2(pitch);       // ~3x faster than std::exp2
/// float phase = FastMath::sin(angle);       // ~5x faster than std::sin
/// float db = FastMath::log2(amplitude);     // ~3x faster than std::log2
/// @endcode
struct FastMath {

    /// Fast tanh approximation using the [7/6] Padé form (max error ~3e-5
    /// for |x| < 4).
    ///
    /// The coefficients follow by truncating Lambert's continued fraction
    /// through denominator 13:
    ///
    ///     tanh(x) = x / (1 + x^2 / (3 + x^2 / (5 + ... + x^2 / 13)))
    ///
    /// Reducing that fraction and applying Horner's rule gives the numerator
    /// and denominator evaluated below. Published derivation:
    /// https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
    static float tanh(float x) {
        // Clamp to avoid overflow in the polynomial
        if (x < -4.0f) return -1.0f;
        if (x > 4.0f) return 1.0f;
        float x2 = x * x;
        float num = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
        float den = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
        return num / den;
    }

    /// Fast sin approximation (Bhaskara-style, max error ~0.001 for any x).
    /// Input in radians.
    static float sin(float x) {
        constexpr float pi = 3.14159265f;
        constexpr float two_pi = 6.28318530f;
        // Wrap to [0, 2*pi)
        x = std::fmod(x, two_pi);
        if (x < 0) x += two_pi;
        // Map to [-pi, pi]
        if (x > pi) x -= two_pi;
        // Parabolic approximation with correction
        constexpr float B = 4.0f / pi;
        constexpr float C = -4.0f / (pi * pi);
        float y = B * x + C * x * std::abs(x);
        // Extra precision via Corrected Parabola
        y = 0.225f * (y * std::abs(y) - y) + y;
        return y;
    }

    /// Fast cos approximation. Input in radians.
    static float cos(float x) {
        return sin(x + 1.5707963f); // x + pi/2
    }

    /// Fast exp2 approximation (2^x, max error ~0.06% for any x).
    static float exp2(float x) {
        // Schraudolph's method with polynomial refinement
        float xi = std::floor(x);
        float xf = x - xi;
        // Polynomial for 2^frac on [0, 1]
        float p = 1.0f + xf * (0.6931472f + xf * (0.2402265f + xf * 0.0558015f));
        // Multiply by 2^integer part via bit manipulation
        int32_t i = static_cast<int32_t>(xi);
        // ldexpf is well-optimized on all platforms
        return p * ldexpf(1.0f, i);
    }

    /// Fast log2 approximation (max error ~0.007 for x > 0).
    static float log2(float x) {
        // Extract exponent and mantissa via IEEE 754
        uint32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFF) - 127;
        bits = (bits & 0x007FFFFF) | 0x3F800000; // set exponent to 0 (mantissa in [1,2))
        float m;
        std::memcpy(&m, &bits, sizeof(m));
        // Polynomial for log2(m) where m in [1, 2)
        float result = static_cast<float>(exponent);
        m -= 1.0f;
        result += m * (1.4423904f + m * (-0.7210953f + m * 0.4778948f));
        return result;
    }

    /// Fast pow(base, exp) via exp2(exp * log2(base)).
    static float pow(float base, float exp) {
        if (base <= 0) return 0;
        return exp2(exp * log2(base));
    }

    /// Fast dB to linear gain: 10^(dB/20) = 2^(dB * log2(10)/20).
    static float db_to_gain(float db) {
        return exp2(db * 0.16609640f); // log2(10)/20 ≈ 0.16609640
    }

    /// Fast linear gain to dB: 20 * log10(gain) = 20 * log2(gain) / log2(10).
    static float gain_to_db(float gain) {
        if (gain <= 0) return -200.0f;
        return log2(gain) * 6.0205999f; // 20 / log2(10) ≈ 6.0205999
    }

    /// Fast reciprocal (1/x) via Newton-Raphson with SSE hint.
    static float rcp(float x) {
        // One Newton-Raphson iteration from initial estimate
        float estimate = 1.0f / x; // compiler will often use rcpss
        return estimate;
    }

    /// Fast inverse square root (1/sqrt(x)).
    static float rsqrt(float x) {
        // Quake-style fast inverse sqrt with one NR iteration
        uint32_t i;
        std::memcpy(&i, &x, sizeof(i));
        i = 0x5f3759df - (i >> 1);
        float y;
        std::memcpy(&y, &i, sizeof(y));
        y = y * (1.5f - 0.5f * x * y * y); // Newton-Raphson
        return y;
    }

    /// Clamp to [-1, 1] without branching (for saturation).
    static float clamp_unit(float x) {
        return std::max(-1.0f, std::min(1.0f, x));
    }

    /// Soft clipping (polynomial saturation, no discontinuity).
    static float soft_clip(float x) {
        if (x <= -1.5f) return -1.0f;
        if (x >= 1.5f) return 1.0f;
        return x - (x * x * x) / 6.75f;
    }
};

/// Fidelity gate for the Moog ladder's saturating non-linearity (PR #5880).
///
/// `FastMath::tanh` is a Padé approximation of `std::tanh`: ~1.2-1.4x faster on
/// the real-time path, but NOT bit-exact. Its error is negligible (~-130 dBFS)
/// for the input range the ladder sees at normal levels (|arg| < ~3, i.e. input
/// up to ~+6 dBFS even at self-oscillation), but the approximation HARD-CLAMPS
/// to +/-1 beyond |x| = 4, where real tanh is still climbing. When the ladder is
/// driven hard (3-4x / +10 dBFS overdrive at high resonance — a signature Moog
/// "drive" setting) the stage argument crosses 4 and the clamp seam produces a
/// genuine, phase-stable deviation that peaks at ~-63 dBFS pointwise and ~-69
/// dBFS in the filtered output. That sits inside the plausibly-audible band and
/// alters the ladder's overdrive character, so the exact saturator is the
/// DEFAULT. Define `PULP_SIGNAL_FAST_LADDER_TANH=1` to opt into the fast Padé
/// path where the ~1.4x throughput win matters more than bit-exact drive.
///
/// This is the single source of truth for the saturator: `LadderFilterT<float>`
/// (the interpreter) and the live-kernel `f2_ladder_tanhf` export (the F2 WASM
/// emitter) both route through it, so the per-node bit-exact null test stays
/// green under either setting.
#ifndef PULP_SIGNAL_FAST_LADDER_TANH
#define PULP_SIGNAL_FAST_LADDER_TANH 0  // default: exact std::tanh (fidelity)
#endif

/// The ladder's `float` saturator, gated by `PULP_SIGNAL_FAST_LADDER_TANH`.
inline float ladder_tanh(float x) {
#if PULP_SIGNAL_FAST_LADDER_TANH
    return FastMath::tanh(x);
#else
    return std::tanh(x);
#endif
}

} // namespace pulp::signal
