#pragma once

// Function's transfer curves: the math that maps an incoming control voltage to
// an outgoing one.
//
// One definition, used by three callers — the audio callback, the editor's graph,
// and the tests. The graph therefore cannot draw a curve the DSP does not
// produce, and a test cannot pass against a transform the plug-in does not run.
//
// Five curves, and they are not all the same kind of thing.
//
// `exponential` and `logarithmic` are the conventional ones: `2^x - 1` and
// `1 + log2(x)`. They are what a patch written against any other CV utility
// expects, so they are the defaults. They are also badly behaved on a bipolar
// signal — `2^x - 1` is not odd-symmetric, so it shifts a symmetric LFO's centre;
// and the logarithm is undefined for negative input, so it flattens half the
// range to zero and its positive half runs to negative infinity near the origin.
// Both facts are inherent to the functions, not to this implementation.
//
// `power` is ours: `y = sign(x) * |x|^k`. It is the obvious single-parameter
// family for a bipolar signal. Odd-symmetric, so polarity survives. Monotone, so
// it never folds. Fixed at the origin and at both rails for every `k`, so the
// Amount knob bends the middle of the response without ever moving where full
// scale lands. `k` and `1/k` are exact inverses, so one knob spans both
// directions. Reach for it when the signal swings both ways; reach for the
// conventional pair when you are reproducing someone else's patch.

#include <brew/cv.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// The transfer shapes. Parameter values are persisted, so: append only.
enum class Curve : int {
    linear = 0,
    exponential = 1,   ///< 2^x - 1
    logarithmic = 2,   ///< 1 + log2(x), zero for non-positive input
    absolute = 3,      ///< |x|
    power = 4,         ///< sign(x) * |x|^Amount
};

inline constexpr int kCurveCount = 5;

/// The exponent range of the `power` curve. `k` and `1/k` bend by the same factor
/// in opposite directions, so one knob covers both and 1.0 is the identity.
inline constexpr float kMinPower = 0.125f;
inline constexpr float kMaxPower = 8.0f;

/// Coerce a parameter float to a Curve, clamping rather than wrapping — an
/// out-of-range value from a host must not silently select a different shape.
[[nodiscard]] inline Curve curve_from_param(float v) noexcept {
    const int i = static_cast<int>(std::lround(v));
    if (i < 0) return Curve::linear;
    if (i >= kCurveCount) return static_cast<Curve>(kCurveCount - 1);
    return static_cast<Curve>(i);
}

/// True when this curve reads the Amount knob. The editor greys the knob out for
/// the others rather than letting a user turn a control that does nothing.
[[nodiscard]] inline constexpr bool curve_uses_amount(Curve c) noexcept {
    return c == Curve::power;
}

/// `sign(x) * |x|^k`, with the identity short-circuited.
///
/// The `k == 1` branch is defensive rather than load-bearing: `std::pow(v, 1.0)`
/// returns `v` exactly on every libm we build against. It is here so a bit-exact
/// pass-through does not depend on that being true, since "the default settings
/// are a wire" is a property this plug-in is expected to hold.
[[nodiscard]] inline double odd_power(double x, double k) noexcept {
    if (k == 1.0) return x;
    const double m = std::pow(std::abs(x), k);
    return x < 0.0 ? -m : m;
}

/// Apply a curve to a value already known to lie in [-1, +1].
///
/// The result is clamped back into range because two of these curves leave it:
/// `2^x - 1` reaches only -0.5 at the bottom (it does not overshoot), but
/// `1 + log2(x)` dives to negative infinity as `x` approaches zero from above.
/// Clamping there is what makes the logarithm usable at all.
[[nodiscard]] inline double apply_curve(Curve c, double amount, double x) noexcept {
    switch (c) {
        case Curve::linear:
            return x;
        case Curve::exponential:
            return std::exp2(x) - 1.0;
        case Curve::logarithmic:
            // Documented as zero for negative input. Zero itself is folded in
            // here too: log2(0) is negative infinity, and a curve that returns
            // -inf at the origin is not a curve anyone can patch.
            if (!(x > 0.0)) return 0.0;
            return std::clamp(1.0 + std::log2(x), -1.0, 1.0);
        case Curve::absolute:
            return std::abs(x);
        case Curve::power:
            return odd_power(x, std::clamp(amount, static_cast<double>(kMinPower),
                                           static_cast<double>(kMaxPower)));
    }
    return x;
}

/// Everything Function does to one sample. Grouped so the editor can evaluate the
/// curve without reaching into the processor's parameter store sample by sample.
struct FunctionSettings {
    float in_scale = 1.0f;
    float in_offset = 0.0f;
    Curve curve = Curve::linear;
    float amount = 1.0f;
    float out_offset = 0.0f;
    float out_scale = 1.0f;
    bool invert = false;
};

/// Input scale and offset, then the curve, then output offset and the suite's
/// shared output stage.
///
/// The clamp before the curve is not cosmetic. `in_scale` can push a signal past
/// full scale, and `|x|^k` on a value greater than one runs *away* from the rail
/// instead of toward it — an input at 1.2 with k = 2 lands at 1.44, and a
/// negative output offset would then carry that excess all the way to the jack
/// rather than having it absorbed by the final clamp. Clamping first means the
/// curve only ever sees the range it is defined on.
[[nodiscard]] inline float function_transfer(float x,
                                             const FunctionSettings& s) noexcept {
    const double scaled = static_cast<double>(x) * static_cast<double>(s.in_scale) +
                          static_cast<double>(s.in_offset);
    const double clamped = std::clamp(scaled, -1.0, 1.0);
    const double shaped = apply_curve(s.curve, static_cast<double>(s.amount), clamped);
    return resolve_output(static_cast<float>(shaped) + s.out_offset, s.out_scale,
                          s.invert);
}

}  // namespace pulp::examples::brew
