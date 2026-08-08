#pragma once

/// @file modulation_curve.hpp
/// A shared, allocation-free curve vocabulary for modulation segments.
///
/// Curves interpolate values expressed in the caller's real unit. They never
/// normalize or clamp endpoints. Progress is normalized and clamped to [0, 1].
/// Non-finite progress is treated as the segment start; non-finite endpoints
/// return the finite endpoint when exactly one is finite, or zero otherwise.
///
/// The exponential and logarithmic shapes reuse the stage law from
/// `mod_tools.hpp`, so existing envelopes and new breakpoint sources agree on
/// what those musical labels mean. All functions are pure and allocate no
/// memory.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/mod_tools.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace pulp::signal {

enum class ModulationCurveShape : std::uint8_t {
    linear,
    exponential,
    logarithmic,
    smoothstep,
    hold,
};

/// Curve strength is dimensionless and legal in [0, 1]. Zero continuously
/// reduces exponential/logarithmic curves to linear. It is ignored by the
/// other shapes. Non-finite strength is sanitized to zero.
struct ModulationCurve {
    ModulationCurveShape shape = ModulationCurveShape::linear;
    float strength = 1.0f;
};

inline ModulationCurve sanitize_modulation_curve(ModulationCurve curve) noexcept {
    if (!std::isfinite(curve.strength))
        curve.strength = 0.0f;
    curve.strength = std::clamp(curve.strength, 0.0f, 1.0f);
    return curve;
}

/// Returns normalized travel from the start of a segment to its end.
/// `rising` selects the musical direction used by exponential/logarithmic
/// labels; it does not constrain the caller's endpoint values.
namespace detail {

template <typename SampleType>
inline SampleType precise_stage_curve(SampleType progress, SampleType curve) noexcept {
    if constexpr (std::is_same_v<SampleType, float>) {
        return stage_curve(progress, curve);
    } else {
        const SampleType k = static_cast<SampleType>(kCurveSpan) * curve;
        if (std::abs(k) < SampleType{1.0e-8})
            return progress;
        return -std::expm1(-k * progress) / -std::expm1(-k);
    }
}

} // namespace detail

template <typename SampleType>
inline SampleType modulation_curve_progress(SampleType progress, ModulationCurve curve,
                                            bool rising) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const SampleType p = std::isfinite(progress)
                             ? std::clamp(progress, SampleType{0}, SampleType{1})
                             : SampleType{0};
    curve = sanitize_modulation_curve(curve);

    switch (curve.shape) {
    case ModulationCurveShape::linear:
        return p;
    case ModulationCurveShape::smoothstep:
        return p * p * (SampleType{3} - SampleType{2} * p);
    case ModulationCurveShape::hold:
        return p >= 1.0f ? 1.0f : 0.0f;
    case ModulationCurveShape::exponential:
        // A rising exponential is slow-then-fast; a falling exponential
        // moves quickly toward its floor and then tails.
        return detail::precise_stage_curve(
            p, static_cast<SampleType>(rising ? -curve.strength : curve.strength));
    case ModulationCurveShape::logarithmic:
        return detail::precise_stage_curve(
            p, static_cast<SampleType>(rising ? curve.strength : -curve.strength));
    }
    return p;
}

template <typename SampleType, typename ProgressType>
inline SampleType interpolate_modulation_curve(SampleType start, SampleType end,
                                               ProgressType progress,
                                               ModulationCurve curve = {}) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(std::is_floating_point_v<ProgressType>);
    const double a = static_cast<double>(start);
    const double b = static_cast<double>(end);
    if (!std::isfinite(a))
        return std::isfinite(b) ? end : SampleType{0};
    if (!std::isfinite(b))
        return start;

    const SampleType shaped =
        modulation_curve_progress(static_cast<SampleType>(progress), curve, b >= a);
    const SampleType result = std::lerp(start, end, shaped);
    return snap_to_zero(result);
}

} // namespace pulp::signal
