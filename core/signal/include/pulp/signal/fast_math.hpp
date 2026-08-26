#pragma once

/// @file fast_math.hpp
/// Scalar math helpers for real-time audio DSP.

#include <algorithm>  // std::max / std::min, used by clamp()
#include <cstdint>
#include <cstring>
#include <cmath>

namespace pulp::signal {

/// Semantic accuracy choices for bounded-cycle real-time trigonometry.
///
/// The names describe the contract a consumer requests. Polynomial degree and
/// coefficient layout are implementation details and are not session state.
enum class FastTrigProfile : std::uint8_t {
    /// Platform sine; the compatibility default.
    reference,
    /// Lower-cost float path with maximum absolute error at most `1.2e-4`
    /// over one bounded cycle. This profile can audibly change recursive FM.
    realtime_efficient,
    /// Precise real-time path. Float implementations have maximum absolute
    /// error at most `2.5e-7` over one bounded cycle; a double consumer may
    /// tighten that budget while preserving this semantic choice.
    realtime_precise,
};

/// Math helpers optimized for audio DSP.
///
/// RT contract: all functions are stateless scalar math helpers and allocate no
/// memory.
///
/// Functions documented as approximations trade precision for speed. Functions
/// without that label retain the standard-library numerical contract.
///
/// @code
/// float out = FastMath::tanh(input);       // ~4x faster than std::tanh
/// float freq = FastMath::exp2(pitch);
/// float phase = FastMath::sin(angle);       // ~5x faster than std::sin
/// float db = FastMath::log2(amplitude);     // ~3x faster than std::log2
/// @endcode
struct FastMath {

    /// Computes sine for a normalized phase in `[0, 1)`.
    ///
    /// The real-time profiles fold the already-bounded phase into a quarter
    /// cycle and avoid general range reduction. Callers with an arbitrary phase
    /// must wrap it before calling this function.
    ///
    /// The two polynomial expressions are adapted from Lasse Schlör's
    /// "Fast MiniMax Polynomial Approximations of Sine and Cosine", pinned at
    /// https://github.com/publik-void/sin-cos-approximations/tree/d65178e684c7626b0fe7df6f261dbadc54403bce
    /// and used under the author's public permission:
    /// https://gist.github.com/publik-void/067f7f2fef32dbe5c27d6e215f824c91?permalink_comment_id=5556230#gistcomment-5556230
    template <FastTrigProfile Profile>
    static float sin_cycles(float phase_cycles) noexcept {
        if constexpr (Profile == FastTrigProfile::reference) {
            constexpr float two_pi = 6.28318530717958647692f;
            return std::sin(two_pi * phase_cycles);
        } else {
            float x = phase_cycles;
            if (x > 0.5f)
                x -= 1.0f;
            if (x > 0.25f)
                x = 0.5f - x;
            else if (x < -0.25f)
                x = -0.5f - x;
            const float x2 = x * x;

            if constexpr (Profile == FastTrigProfile::realtime_efficient) {
                return x * (6.28250560008354834003487064338056964f +
                            x2 * (-41.1664423308903732524414077000881397f +
                                  74.4524187069072428211419543641796188f * x2));
            } else {
                static_assert(Profile == FastTrigProfile::realtime_precise);
                return x *
                       (6.28318527379078585274731929079414949f +
                        x2 *
                            (-41.3416774783915252855640244027643612f +
                             x2 *
                                 (81.6022312427274226421465134076212909f +
                                  x2 *
                                      (-76.5749921819992128192000934020817094f +
                                       39.7109181438058471453004860893416233f * x2))));
            }
        }
    }

    /// Runtime profile dispatch for setup, control, and non-inner-loop use.
    static float sin_cycles(float phase_cycles, FastTrigProfile profile) noexcept {
        switch (profile) {
            case FastTrigProfile::realtime_efficient:
                return sin_cycles<FastTrigProfile::realtime_efficient>(phase_cycles);
            case FastTrigProfile::realtime_precise:
                return sin_cycles<FastTrigProfile::realtime_precise>(phase_cycles);
            case FastTrigProfile::reference:
            default: return sin_cycles<FastTrigProfile::reference>(phase_cycles);
        }
    }

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

    /// Computes 2^x with the platform's standard float exp2 semantics.
    ///
    /// NaN propagates, positive infinity and finite overflow produce positive
    /// infinity, and negative infinity and finite underflow produce positive
    /// zero. With gradual underflow enabled, subnormal results and representable
    /// integer powers are preserved. Under Pulp's audio-callback
    /// `ScopedFlushDenormals` policy, the finite subnormal range may flush to
    /// positive zero exactly as the platform `std::exp2` does in that ambient FP
    /// mode. This deliberately uses the standard implementation: a polynomial
    /// plus `ldexp` measured slower on supported Apple hardware while weakening
    /// these edge contracts.
    static float exp2(float x) {
        return std::exp2(x);
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

    /// Precise double sine for a bounded cycle count. Folding is part of this
    /// helper because FM phase offsets and harmonic multiples can span several
    /// cycles.
    /// This degree-13 expression comes from the same pinned source and public
    /// permission cited by `sin_cycles` above.
    static double sin_cycles_precise64(double phase_cycles) noexcept {
        double x = phase_cycles - std::floor(phase_cycles + 0.5);
        const double magnitude = std::abs(x);
        x = std::copysign(std::min(magnitude, 0.5 - magnitude), x);
        const double x2 = x * x;
        return x *
               (6.28318530717919415440631052356951227 +
                x2 *
                    (-41.3417022398184912491504586563309009 +
                     x2 *
                         (81.6052491334177909789178729942153114 +
                          x2 *
                              (-76.7058464941280158505651312164454235 +
                               x2 *
                                   (42.0581028415940046209613080938107769 +
                                    x2 *
                                        (-15.0810317173017800774891418165142071 +
                                         3.66346472229432872653352143098494556 * x2))))));
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

/// A [3/2] Padé tanh that is 1-LIPSCHITZ and bounded — the saturator to reach
/// for inside a feedback loop.
///
/// This is a different curve from `FastMath::tanh`, not a cheaper one, and the
/// difference is the whole reason it exists. `FastMath::tanh` chases fidelity
/// to `std::tanh`; this one is chosen for a property. Its derivative is
/// (x^2-9)^2 / (9(3+x^2)^2), which is exactly 1 at the origin and strictly
/// below 1 everywhere else, so it can never amplify — and the clamp at the
/// point where the curve reaches unity (f(3) = 1 exactly) makes it bounded as
/// well, without breaking that slope bound.
///
/// Inside a recursion those two properties are load-bearing. A saturator with
/// small-signal gain above 1 (tanh(k*x) for k > 1 is the usual way to get one)
/// silently raises the loop gain with it, which turns a decay-time control into
/// a stability question; an unbounded one lets a self-oscillating loop settle
/// only by growing more slowly than it decays. Two modules derived this same
/// curve independently for those reasons — the character delay's dub feedback
/// and the FDN reverb's in-loop drive — which is why it lives here now instead
/// of once in each.
inline double lipschitz_tanh(double v) noexcept {
    const double x = std::clamp(v, -3.0, 3.0);
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

} // namespace pulp::signal
