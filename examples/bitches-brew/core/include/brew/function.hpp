#pragma once

// Function's transfer curve: the math that maps an incoming control voltage to
// an outgoing one.
//
// One definition, used by three callers — the audio callback, the editor's
// graph, and the tests. The graph therefore cannot draw a curve the DSP does not
// produce, and a test cannot pass against a transform the plug-in does not run.
//
// The curve shapes here are OUR design, chosen because they are the obvious
// single-parameter family for a bipolar signal, not because they match any
// particular product. `y = sign(x) * |x|^k` is odd-symmetric (so a bipolar CV
// keeps its polarity), monotone (so it never folds), passes through the origin
// and through ±1 for every k (so full scale stays full scale), and degenerates to
// the identity at k = 1. Raising k bends the response toward the origin; lowering
// it bends away. Nothing else about the shape is claimed.

#include <brew/cv.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples::brew {

/// The transfer shapes. Parameter values are persisted, so: append only.
enum class Curve : int {
    linear = 0,
    exponential = 1,
    logarithmic = 2,
    absolute = 3,
};

inline constexpr int kCurveCount = 4;

/// Coerce a parameter float to a Curve, clamping rather than wrapping — an
/// out-of-range value from a host must not silently select a different shape.
[[nodiscard]] inline Curve curve_from_param(float v) noexcept {
    const int i = static_cast<int>(std::lround(v));
    if (i < 0) return Curve::linear;
    if (i >= kCurveCount) return static_cast<Curve>(kCurveCount - 1);
    return static_cast<Curve>(i);
}

/// The exponent a curve applies. `amount` only shapes the two bent curves;
/// `linear` and `absolute` are unbent by definition.
///
/// Exponential and logarithmic are reciprocals of each other, so the same
/// `amount` bends the response by the same factor in either direction.
[[nodiscard]] inline double curve_exponent(Curve c, double amount) noexcept {
    if (!(amount > 0.0)) return 1.0;
    switch (c) {
        case Curve::exponential: return amount;
        case Curve::logarithmic: return 1.0 / amount;
        case Curve::linear:
        case Curve::absolute: return 1.0;
    }
    return 1.0;
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
[[nodiscard]] inline double apply_curve(Curve c, double amount, double x) noexcept {
    const double y = odd_power(x, curve_exponent(c, amount));
    return c == Curve::absolute ? std::abs(y) : y;
}

/// Everything Function does to one sample. Grouped so the editor can evaluate the
/// curve without reaching into the processor's parameter store sample by sample.
struct FunctionSettings {
    float in_scale = 1.0f;
    float in_offset = 0.0f;
    Curve curve = Curve::linear;
    float amount = 2.0f;
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
