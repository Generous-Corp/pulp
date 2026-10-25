#pragma once

/// @file fast_math.hpp
/// Scalar math helpers for real-time audio DSP.

#include <pulp/signal/denormal.hpp>

#include <algorithm>  // std::max / std::min, used by clamp()
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <cmath>

#if defined(__APPLE__) && defined(__clang__)
#include <simd/simd.h>
#endif

// Clang's IEEE 754-2019 maximum/minimum builtins propagate NaN, as the
// AArch64 FMAX/FMIN instructions do. Nested so that a compiler without
// __has_builtin never parses the call.
#if defined(__clang__) && defined(__has_builtin)
#if __has_builtin(__builtin_elementwise_maximum) && __has_builtin(__builtin_elementwise_minimum)
#define PULP_FAST_MATH_HAS_IEEE_MINMAX 1
#endif
#endif
#ifndef PULP_FAST_MATH_HAS_IEEE_MINMAX
#define PULP_FAST_MATH_HAS_IEEE_MINMAX 0
#endif

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
    /// Float-precision path with maximum absolute error at most `2.5e-7`
    /// over one bounded cycle.
    realtime_precise,
};

/// Math helpers optimized for audio DSP.
///
/// RT contract: all functions are stateless scalar math helpers and allocate no
/// memory.
///
/// Functions documented as approximations trade precision for speed. Functions
/// without that label retain the standard-library numerical contract. Bounds
/// quoted "with or without fused multiply-add" hold under either side of
/// Pulp's contraction policy (tools/cmake/PulpFloatingPointContraction.cmake:
/// Clang contracts within an expression, GCC does not).
///
/// Choosing a tanh — one family, four points on the cost/accuracy curve
/// (worst absolute error over every float input; cost per element relative to
/// `tanh()` in a 128-sample loop on an M5 Ultra, all measured in one run):
///
/// | Function                 | Worst error | Cost  | Clamp                         |
/// |--------------------------|-------------|-------|-------------------------------|
/// | `tanh()`                 | 9.6e-5      | 1x    | argument at 4.97, continuous  |
/// | `tanh_minimax()` block   | 3.8e-6      | 0.93x | at 7: slope-matched           |
/// | `tanh_minimax()` scalar  | 3.8e-6      | 1.4x  | at 7: slope-matched           |
/// | `tanh_accurate()`        | 3.0e-7      | 1.55x | at 9, branch-free             |
/// | `std::tanh`              | exact       | 8.4x  | none                          |
///
/// The bare [7/6] Padé without `tanh()`'s clamp costs 0.82x but is not a tanh
/// outside a small range: it is 1e-4 off by |x| = 5, exceeds 1 there and
/// grows without bound (about x/28), so every bounded entry above pays for
/// its clamp on purpose.
///
/// Prefer `tanh_minimax()` where `tanh()` would be used and the extra 25x of
/// accuracy is worth its cost, `tanh_minimax_recursive()` for the same fit
/// inside a per-sample recursion, and `tanh_accurate()` inside a resonant
/// recursion whose loop gain amplifies saturator error.
///
/// @code
/// float out = FastMath::tanh(input);       // ~8x faster than std::tanh
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
                       (precise_c0 +
                        x2 *
                            (precise_c1 +
                             x2 *
                                 (precise_c2 +
                                  x2 *
                                      (precise_c3 + precise_c4 * x2))));
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

#if defined(__APPLE__) && defined(__clang__)
    /// Four-wide Apple implementation of the precise bounded-cycle profile.
    /// Input lanes must already be in `[0, 1)`, matching the scalar contract.
    /// Apple's vector library is spelled `::simd` here: inside namespace pulp
    /// an unqualified `simd::` names Pulp's own pulp::simd kernel namespace.
    static simd_float4 sin_cycles_precise(simd_float4 phase_cycles) noexcept {
        simd_float4 x = phase_cycles - ::simd::floor(phase_cycles + 0.5f);
        const simd_float4 magnitude = ::simd::abs(x);
        x = ::simd::copysign(::simd::min(magnitude, 0.5f - magnitude), x);
        const simd_float4 x2 = x * x;
        return x *
               (precise_c0 +
                x2 *
                    (precise_c1 +
                     x2 *
                         (precise_c2 +
                          x2 *
                              (precise_c3 + precise_c4 * x2))));
    }
#endif

    /// Fast tanh approximation using the [7/6] Padé form, within 9.6e-5 of
    /// std::tanh for every float input, continuous, and bounded by ±1.
    ///
    /// The coefficients follow by truncating Lambert's continued fraction
    /// through denominator 13:
    ///
    ///     tanh(x) = x / (1 + x^2 / (3 + x^2 / (5 + ... + x^2 / 13)))
    ///
    /// Reducing that fraction and applying Horner's rule gives the numerator
    /// and denominator evaluated below. Published derivation:
    /// https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
    ///
    /// The fraction rises monotonically to exactly 1 at |x| = 4.9717869
    /// (kTanhPadeClamp) and keeps growing past it, so the argument is clamped
    /// there: the curve meets its saturation value with no step, and the
    /// worst error is 1 - tanh(4.97) = 9.6e-5, at the clamp. (Saturating the
    /// output at |x| = 4 instead would leave a 6.7e-4 step.) Clamping the argument costs one min and one max ahead of the
    /// rational, where saturating the output costs a compare and a select per
    /// side after it. The numerator is scaled by 1 - 2^-21, as in
    /// tanh_minimax_rational(), so every evaluation order stays strictly inside
    /// ±1; beyond the clamp the result is ±0.9999997.
    ///
    /// Measured in float over every input, with and without fused
    /// multiply-add: worst absolute error 9.6e-5, odd, |result| < 1. The clamp
    /// propagates NaN (IEEE maximum/minimum on Clang, ordered selects
    /// elsewhere), so a NaN input returns NaN. No branches or calls, so loops
    /// over it vectorize.
    static float tanh(float x) {
        const float c = tanh_pade_clamp(x);
        constexpr float kScale = 1.0f - 0x1p-21f;
        const float c2 = c * c;
        const float num = c * (135135.0f * kScale +
                               c2 * (17325.0f * kScale + c2 * (378.0f * kScale + c2 * kScale)));
        const float den = 135135.0f + c2 * (62370.0f + c2 * (3150.0f + c2 * 28.0f));
        return num / den;
    }

    /// Where the [7/6] Padé in tanh() reaches 1: tanh() clamps its argument
    /// to [-kTanhPadeClamp, kTanhPadeClamp].
    static constexpr float kTanhPadeClamp = 4.97178686f;

    /// tanh()'s argument clamp, propagating NaN.
    static float tanh_pade_clamp(float x) noexcept {
#if PULP_FAST_MATH_HAS_IEEE_MINMAX
        return __builtin_elementwise_minimum(__builtin_elementwise_maximum(x, -kTanhPadeClamp),
                                             kTanhPadeClamp);
#else
        const float upper = x > kTanhPadeClamp ? kTanhPadeClamp : x;
        return upper < -kTanhPadeClamp ? -kTanhPadeClamp : upper;
#endif
    }

    /// tanh within 3.0e-7 absolute of std::tanh for every float input when
    /// the compiler fuses multiply-adds, and within 3.6e-7 when it does not
    /// (-ffp-contract=off, MSVC, x86-64 without FMA); both measured over every
    /// float. Cheap enough for a saturator inside a per-sample feedback loop.
    ///
    /// x * P(x^2) / Q(x^2) with both polynomials of degree 4 and P(0) = Q(0)
    /// = 1, fitted for minimax absolute error on [0, 9]; x^2 is clamped to 81
    /// and the result to [-1, 1], so the result is exactly +/-1 for |x| >= 9.0001
    /// (std::tanh itself rounds to 1 only above about 9.01).
    /// Properties the ladder relies on:
    ///   - odd: tanh_accurate(-x) == -tanh_accurate(x) exactly, -0 stays -0;
    ///   - unit slope at 0 and relative error below 3e-7 for small |x|, so the
    ///     small-signal loop gain of a resonant filter is unchanged;
    ///   - bounded for every non-NaN input (+/-inf give +/-1); NaN propagates,
    ///     as with std::tanh;
    ///   - monotone to within its error bound (float rounding can step down by
    ///     a few ulps near saturation);
    ///   - no branches or calls, so loops over it vectorize.
    /// About 2.4x the throughput of std::tanh inside a ladder recursion.
    static float tanh_accurate(float x) {
        // Clamping x^2 rather than x keeps the sign path short; past |x| = 9
        // the ratio exceeds 1 and the output clamp saturates it.
        const float u = std::fmin(x * x, 81.0f);
        // Ordered compares rather than fmin/fmax: a NaN fails both and
        // propagates, as std::tanh's does, instead of becoming a full-scale
        // constant that hides the upstream fault.
        const float r = tanh_accurate_rational(x, u);
        const float upper = r > 1.0f ? 1.0f : r;
        return upper < -1.0f ? -1.0f : upper;
    }

    /// The unsaturated rational inside tanh_accurate(): x * P(u) / Q(u) with
    /// u = min(x^2, 81). tanh_accurate() is this clamped to [-1, 1]; exposed so
    /// a vectorized caller can apply the same clamp with its own selects.
    static float tanh_accurate_rational(float x, float u) {
        const float u2 = u * u;
        // Both polynomials in Estrin form: shorter dependency chains than
        // Horner, which matters inside a serial per-sample recursion.
        const float p = (1.0f + u * 0.133803148f) +
                        u2 * ((0.00349475497f + u * 2.05960245e-05f) + u2 * 1.33379685e-08f);
        const float q = (1.0f + u * 0.467136322f) +
                        u2 * ((0.0258737793f + u * 0.000328429886f) + u2 * 7.7694232e-07f);
        return x * p / q;
    }

    /// Constrained-minimax [7/6] rational tanh: the evaluation cost of
    /// `tanh()`'s rational above, and about 25x lower worst-case error.
    ///
    /// `tanh()` truncates Lambert's continued fraction, so its error is tiny
    /// near the origin and grows toward its clamp, reaching 9.6e-5 there. This
    /// form spends the same coefficients on the worst case instead. They are the
    /// minimax fit of `x·P(x²)/Q(x²)` (P and Q cubic) to `tanh` on [0, 7] under
    /// three linear equality constraints: unit slope at the origin, `f(7) = 1`
    /// and `f'(7) = 0`, so the clamp beyond |x| = 7 meets the curve with
    /// matching slope. Fit by bisection on the error bound over a
    /// linear-programming feasibility problem (the linearised rational minimax
    /// of Cheney & Loeb, Numer. Math. 3, 1961), coefficients rounded to float.
    ///
    /// The numerator is then scaled by `1 − 2⁻²¹`. Unscaled, rounding lets the
    /// rational overshoot 1 by up to two float ulps near the clamp, and how
    /// far depends on which multiply-adds a compiler contracts; the scale
    /// keeps every evaluation order strictly inside ±1 without an output
    /// clamp. Its cost is a 4e-7 step where the clamp takes over and a slope
    /// of `1 − 2⁻²¹` at the origin, both far below the fit error.
    ///
    /// Measured in float over every representable input: absolute error below
    /// 3.8e-6 (the tail beyond the clamp contributes 1 − tanh(7) = 1.7e-6),
    /// odd, and bounded by ±1 exactly. The clamps are branches: inside a
    /// serial recursion such as a ladder they are predicted off the
    /// critical path, where min/max instructions would sit on it and cost
    /// 6-65% in a ladder (more under heavy drive, where a predicted branch
    /// skips the division). Over a buffer use the block overload below, which
    /// clamps branch-free. A NaN input returns NaN.
    static float tanh_minimax(float x) {
        if (x < -kTanhMinimaxClamp)
            return -1.0f;
        if (x > kTanhMinimaxClamp)
            return 1.0f;
        return tanh_minimax_rational(x);
    }

    /// Block form of `tanh_minimax()`; `output` may equal `input`.
    ///
    /// The clamp is a min/max pair rather than branches, so the whole
    /// evaluation is straight-line and runs four lanes at a time, four
    /// vectors per iteration (GNU vector extensions on Clang and GCC; a
    /// scalar loop the compiler may vectorise elsewhere). A clamped lane
    /// evaluates the rational at ±7, about 4e-7 short of ±1, so beyond
    /// the clamp this differs from the scalar form by that much and inside it
    /// not at all; both stay within the 3.8e-6 bound. On Apple Silicon it
    /// costs ~1.2x the unclamped [7/6] Padé per element, which is the price of
    /// the two clamp instructions on a nine-instruction rational: that form
    /// has no clamp and grows without bound past |x| ~ 5. With
    /// auto-vectorisation disabled it holds its speed while the scalar forms
    /// slow by about 3x. The result for a NaN input is unspecified.
    static void tanh_minimax(const float* input, float* output, std::size_t count) noexcept {
        std::size_t i = 0;
#if defined(__clang__) || defined(__GNUC__)
        using Lanes = float __attribute__((vector_size(16)));
        constexpr std::size_t kVectors = 4;
        constexpr std::size_t kStep = kVectors * 4;
        for (; i + kStep <= count; i += kStep) {
            Lanes v[kVectors];
            std::memcpy(v, input + i, sizeof v);
            for (auto& lane : v) {
#if defined(__clang__)
                const Lanes c = __builtin_elementwise_min(
                    __builtin_elementwise_max(lane, Lanes{} - kTanhMinimaxClamp),
                    Lanes{} + kTanhMinimaxClamp);
#else
                const Lanes lo = Lanes{} - kTanhMinimaxClamp;
                const Lanes hi = Lanes{} + kTanhMinimaxClamp;
                const Lanes floored = lane < lo ? lo : lane;
                const Lanes c = floored > hi ? hi : floored;
#endif
                lane = tanh_minimax_rational(c);
            }
            std::memcpy(output + i, v, sizeof v);
        }
#endif
        for (; i < count; ++i)
            output[i] = tanh_minimax_rational(
                std::fmin(std::fmax(input[i], -kTanhMinimaxClamp), kTanhMinimaxClamp));
    }

    /// The rational inside `tanh_minimax()` for an argument already clamped
    /// to [-7, 7], generic over float and a GNU float vector so both forms
    /// round identically.
    template <typename T> static T tanh_minimax_rational(T x) noexcept {
        constexpr float kScale = 1.0f - 0x1p-21f;
        const T t = x * x;
        const T num = x * (kScale + t * (1.2183626499918065e-01f * kScale +
                                         t * (2.1714800492144003e-03f * kScale +
                                              t * (3.4510574788349538e-06f * kScale))));
        const T den = 1.0f + t * (4.5515217250939749e-01f +
                                  t * (2.0577605545641264e-02f + t * 1.3105688057792639e-04f));
        return num / den;
    }

    /// tanh_minimax() for a serial recursion: the same fit, clamp and
    /// numerator scale, with each cubic evaluated in Estrin form (two
    /// independent halves joined by t^2), so the chain from x to the quotient
    /// is one multiply-add shorter than tanh_minimax_rational()'s Horner form.
    /// It rounds differently from tanh_minimax(), by a few ulps, within the
    /// same 3.8e-6 bound (measured over every float, with and without fused
    /// multiply-add: 3.67e-6 and 3.70e-6).
    ///
    /// Inside a polyphonic voice loop (oscillator call, envelope, ladder per
    /// voice), the Horner chain let the compiler's pre-register-allocation
    /// instruction scheduler emit an order in which consecutive voices stopped
    /// overlapping: the loop ran 2.6x slower than with the accurate saturator,
    /// with the core stalled on execution (processing-bottleneck share 82%
    /// against 49%; branch mispredicts and instruction delivery both under 1%).
    /// The Estrin form ran 0.71-1.0x the accurate saturator's time in every
    /// tested voice context, at -O2 and -O3. Over a buffer, where throughput
    /// rather than latency binds, Horner is cheaper by its one fewer multiply,
    /// which is why tanh_minimax() keeps it.
    static float tanh_minimax_recursive(float x) noexcept {
        if (x < -kTanhMinimaxClamp)
            return -1.0f;
        if (x > kTanhMinimaxClamp)
            return 1.0f;
        constexpr float kScale = 1.0f - 0x1p-21f;
        constexpr float n1 = 1.2183626499918065e-01f * kScale;
        constexpr float n2 = 2.1714800492144003e-03f * kScale;
        constexpr float n3 = 3.4510574788349538e-06f * kScale;
        constexpr float d1 = 4.5515217250939749e-01f;
        constexpr float d2 = 2.0577605545641264e-02f;
        constexpr float d3 = 1.3105688057792639e-04f;
        const float t = x * x;
        const float t2 = t * t;
        const float num = x * ((kScale + t * n1) + t2 * (n2 + t * n3));
        const float den = (1.0f + t * d1) + t2 * (d2 + t * d3);
        return num / den;
    }

    static constexpr float kTanhMinimaxClamp = 7.0f;

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

    // Branch-free decibel conversions for per-sample dynamics paths: plain
    // arithmetic and bit manipulation with no calls and no branches, so a loop
    // over them auto-vectorizes, unlike the libm calls they replace. Accurate
    // enough to keep a compressor's gain within a few parts in 10^7 of the
    // libm path (a residual below -120 dBFS), unlike the coarse log2-based
    // db_to_gain() / gain_to_db() above.

    /// log10(x) for finite, positive, normal x, within 4 ulps of the correctly
    /// rounded result (3.47 worst measured), with or without fused multiply-add.
    /// Other inputs are outside the contract (callers clamp to a positive floor
    /// first).
    ///
    /// x = m * 2^k with m in [sqrt(1/2), sqrt(2)); ln(m) = 2 atanh(s) with
    /// s = (m - 1) / (m + 1), |s| <= 0.1716, from the odd series
    /// 2s (1 + s^2/3 + s^4/5 + ...) truncated after s^8/9; the next term is
    /// below 2e-9 relative, under float resolution.
    static float log10_accurate(float x) noexcept {
        constexpr std::uint32_t kSqrtHalfBits = 0x3f3504f3u; // bits of sqrt(1/2)
        const std::uint32_t offset = std::bit_cast<std::uint32_t>(x) - kSqrtHalfBits;
        const float k = static_cast<float>(static_cast<std::int32_t>(offset) >> 23);
        const float m = std::bit_cast<float>((offset & 0x007fffffu) + kSqrtHalfBits);
        const float f = m - 1.0f; // exact: m is within a factor of 2 of 1
        const float s = f / (2.0f + f);
        const float z = s * s;
        const float series =
            z * (1.0f / 3.0f + z * (1.0f / 5.0f + z * (1.0f / 7.0f + z * (1.0f / 9.0f))));
        const float two_s = 2.0f * s;
        const float ln_m = two_s * series + two_s;
        // log10(2) and 1/ln(10). The high part of log10(2) has few enough
        // significant bits that k * hi is exact for every float exponent.
        constexpr float kLog10TwoHi = 0.30102539062f; // 0x3e9a2000
        constexpr float kLog10TwoLo = 4.6050390e-06f; // log10(2) - hi
        constexpr float kInvLn10 = 0.43429448190f;
        return k * kLog10TwoHi + (ln_m * kInvLn10 + k * kLog10TwoLo);
    }

    /// 10^x within 1.2e-7 relative of the correctly rounded result, with or
    /// without fused multiply-add. Results beyond the float range saturate to +inf
    /// or 0 (through the subnormal range) as std::pow does, and NaN propagates.
    ///
    /// 10^x = 2^n * 10^d with n = round(x log2(10)) and d = x - n log10(2),
    /// |d| <= log10(2) / 2. The reduction is Cody-Waite in the decimal domain:
    /// log10(2) is split into a high part with few enough significant bits that
    /// n * hi is exact for every n used, so x - n * hi is exact (Sterbenz) and d
    /// carries no rounding from the product whether or not the compiler fuses
    /// multiply-adds. 10^d = e^(d ln 10) is the Taylor series through degree 7
    /// (truncation below 6e-9 relative). 2^n is applied as two normal-range
    /// factors so n may run past the float exponent range and the final product
    /// over- or underflows naturally.
    static float exp10_accurate(float x) noexcept {
        constexpr float kLog2Ten = 3.32192809489f;
        constexpr float kLog10TwoHi = 0.30102539062f; // 0x3e9a2000, 11 significant bits
        constexpr float kLog10TwoLo = 4.6050390e-06f; // log10(2) - hi
        constexpr float kLnTen = 2.30258509299f;
        // Past 10^+/-75 (about 250 octaves) the result is inf or 0 anyway;
        // clamping there keeps |n| <= 250 (so n * hi stays exact) and each half
        // of 2^n inside the normal exponent range.
        const float xc = std::fmin(std::fmax(x, -75.0f), 75.0f);
        const float n = std::nearbyint(xc * kLog2Ten);
        const float d = (xc - n * kLog10TwoHi) - n * kLog10TwoLo;
        const float t = d * kLnTen;
        const float p =
            1.0f +
            t * (1.0f + t * (0.5f + t * (1.0f / 6.0f +
                                         t * (1.0f / 24.0f +
                                              t * (1.0f / 120.0f +
                                                   t * (1.0f / 720.0f + t * (1.0f / 5040.0f)))))));
        const std::int32_t e = static_cast<std::int32_t>(n);
        const std::int32_t e_half = e / 2;
        const float scale_a = std::bit_cast<float>(static_cast<std::uint32_t>(e_half + 127) << 23);
        const float scale_b =
            std::bit_cast<float>(static_cast<std::uint32_t>(e - e_half + 127) << 23);
        const float result = (p * scale_a) * scale_b;
        return x == x ? result : x;
    }

    /// 20 log10(gain) for a finite, positive, normal gain (clamp to a floor
    /// first); within log10_accurate()'s bound.
    static float gain_to_db_accurate(float gain) noexcept {
        return 20.0f * log10_accurate(gain);
    }

    /// 10^(dB / 20), within exp10_accurate()'s bound.
    static float db_to_gain_accurate(float db) noexcept {
        return exp10_accurate(db / 20.0f);
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

private:
    static constexpr float precise_c0 =
        6.28318527379078585274731929079414949f;
    static constexpr float precise_c1 =
        -41.3416774783915252855640244027643612f;
    static constexpr float precise_c2 =
        81.6022312427274226421465134076212909f;
    static constexpr float precise_c3 =
        -76.5749921819992128192000934020817094f;
    static constexpr float precise_c4 =
        39.7109181438058471453004860893416233f;

public:

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

/// Saturator choices for the Moog ladder's per-stage non-linearity.
///
/// - `exact`: std::tanh. The reference the others are measured against.
/// - `accurate`: `FastMath::tanh_accurate`, within 3.0e-7 (3.6e-7 without
///   fused multiply-add) of std::tanh for every input. Against the
///   exact-saturator ladder, over cutoffs of 300 Hz to 6 kHz, resonance 0 to
///   1 and sine input of 110 Hz to 7 kHz at 48 kHz, the pointwise output
///   difference is at most -124 dBFS for input up to 0 dBFS, -107 dBFS at
///   +10 dBFS drive and -102 dBFS at +20 dBFS, measured with Apple clang and
///   with GCC and glibc, with or without fused multiply-add (the worst cases
///   are high resonance, a low input and a high cutoff, where the recursion
///   amplifies the saturator's error; the exact reference's own libm tanh
///   differs by platform). Tested with about 2 dB of margin. Harmonic
///   and alias levels match to 0.001 dB and resonant ringing frequency and
///   decay are unchanged. About 2.4x the throughput of std::tanh. This is the
///   default.
/// - `realtime_efficient`: `FastMath::tanh_minimax_recursive` (the
///   `tanh_minimax` fit in Estrin form), within 3.8e-6 of std::tanh for every
///   input and continuous in value and slope where it clamps at |x| = 7.
///   Cheapest per evaluation at normal levels, including inside a
///   polyphonic voice loop; under heavy drive `accurate` can be faster,
///   because this form's division is on the recursion's critical path for
///   every argument below 7. The Lambert `FastMath::tanh` is not offered
///   here: its 9.6e-5 worst error is 25x this one's, and a resonant loop
///   amplifies saturator error.
enum class LadderSaturation { exact, accurate, realtime_efficient };

/// Selecting the default ladder saturator at build time:
///   - `PULP_SIGNAL_LADDER_SATURATION` = 0 (exact), 1 (accurate) or
///     2 (realtime_efficient) selects it explicitly;
///   - otherwise `PULP_SIGNAL_FAST_LADDER_TANH` keeps its original meaning:
///     defined to 1 selects realtime_efficient, defined to 0 selects exact
///     std::tanh (the default before the accurate saturator existed);
///   - with neither defined the default is accurate.
///
/// This is the single source of truth for the saturator: `LadderFilterT<float>`
/// (the interpreter) and the live-kernel `f2_ladder_tanhf` export (the F2 WASM
/// emitter) both route through it, so the per-node bit-exact null test stays
/// green under any setting.
#if defined(PULP_SIGNAL_LADDER_SATURATION)
static_assert(PULP_SIGNAL_LADDER_SATURATION == 0 || PULP_SIGNAL_LADDER_SATURATION == 1 ||
                  PULP_SIGNAL_LADDER_SATURATION == 2,
              "PULP_SIGNAL_LADDER_SATURATION must be 0 (exact), 1 (accurate) or "
              "2 (realtime_efficient)");
inline constexpr LadderSaturation kDefaultLadderSaturation =
    PULP_SIGNAL_LADDER_SATURATION == 0   ? LadderSaturation::exact
    : PULP_SIGNAL_LADDER_SATURATION == 2 ? LadderSaturation::realtime_efficient
                                         : LadderSaturation::accurate;
#elif defined(PULP_SIGNAL_FAST_LADDER_TANH)
inline constexpr LadderSaturation kDefaultLadderSaturation =
    PULP_SIGNAL_FAST_LADDER_TANH ? LadderSaturation::realtime_efficient : LadderSaturation::exact;
#else
inline constexpr LadderSaturation kDefaultLadderSaturation = LadderSaturation::accurate;
#endif

/// The ladder's `float` saturator for an explicit choice.
///
/// Both forms take an `auto` parameter constrained to its exact type. With a
/// typed `LadderSaturation` parameter MSVC converts the argument `true` to the
/// enumerator whose value is 1 (`accurate`) and reports the boolean call as
/// ambiguous (C2668); Clang and GCC never consider that conversion.
template <auto Saturation>
    requires std::is_same_v<decltype(Saturation), LadderSaturation>
inline float ladder_tanh_profiled(float x) {
    if constexpr (Saturation == LadderSaturation::realtime_efficient)
        return FastMath::tanh_minimax_recursive(x);
    else if constexpr (Saturation == LadderSaturation::accurate)
        return FastMath::tanh_accurate(x);
    else
        return std::tanh(x);
}

/// The previous boolean form: true is the Padé saturator, false is exact
/// std::tanh, as before LadderSaturation existed.
template <auto RealtimeEfficient>
    requires std::is_same_v<decltype(RealtimeEfficient), bool>
[[deprecated("use ladder_tanh_profiled<LadderSaturation>")]]
inline float ladder_tanh_profiled(float x) {
    return ladder_tanh_profiled<RealtimeEfficient ? LadderSaturation::realtime_efficient
                                                  : LadderSaturation::exact>(x);
}

/// The ladder's default `float` saturator.
inline float ladder_tanh(float x) {
    return ladder_tanh_profiled<kDefaultLadderSaturation>(x);
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

namespace detail {

/// `condition ? if_true : if_false` as bit operations. Same value as the
/// conditional for every input, including NaN payloads and signed zeros, but
/// with no control flow: GCC with its default -ftrapping-math will not
/// if-convert a float conditional whose arms it may sink, which leaves loops
/// over per-voice state unvectorized; this form vectorizes under GCC and
/// Clang alike.
template <typename T> inline T select_bits(bool condition, T if_true, T if_false) noexcept {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    using U = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    const U mask = U{0} - static_cast<U>(condition);
    return std::bit_cast<T>((std::bit_cast<U>(if_true) & mask) |
                            (std::bit_cast<U>(if_false) & ~mask));
}

/// snap_to_zero() as bit operations (see select_bits()).
template <typename T> inline T snap_to_zero_bits(T x) noexcept {
#if PULP_DSP_ENABLE_SNAP_TO_ZERO
    return select_bits(std::abs(x) < snap_threshold<T>(), T(0), x);
#else
    return x;
#endif
}

/// FastMath::tanh_accurate() with its clamps as bit operations; the same
/// value for every input.
inline float tanh_accurate_bits(float x) noexcept {
    const float u = std::fmin(x * x, 81.0f);
    const float r = FastMath::tanh_accurate_rational(x, u);
    const float upper = select_bits(r > 1.0f, 1.0f, r);
    return select_bits(upper < -1.0f, -1.0f, upper);
}

} // namespace detail

} // namespace pulp::signal
