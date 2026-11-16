#pragma once

/// @file control_math.hpp
/// Canonical finite scalar Control operations. Every scalar Control stage in
/// Pulp (the graph Control nodes, the indexed Control utilities, and their
/// pure step forms) computes through these functions, so a value has one
/// definition no matter which surface produced it.
///
/// Shared contract: a non-finite input (NaN or either infinity) is replaced
/// with zero before it is used, so a Control lane never propagates NaN or
/// infinity. Nothing here allocates, locks, or keeps hidden time state.

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>

namespace pulp::signal {

/// Replace a non-finite Control sample with zero. Signed zero is preserved.
template <std::floating_point SampleType> SampleType finite_control(SampleType value) noexcept {
    return std::isfinite(value) ? value : SampleType{0};
}

/// A zero denominator yields zero. A finite quotient that exceeds the sample
/// type's range saturates at its finite endpoint.
template <std::floating_point SampleType>
SampleType safe_control_divide(SampleType numerator, SampleType denominator) noexcept {
    numerator = finite_control(numerator);
    denominator = finite_control(denominator);
    if (denominator == SampleType{0})
        return SampleType{0};
    const long double quotient =
        static_cast<long double>(numerator) / static_cast<long double>(denominator);
    const long double limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
    return static_cast<SampleType>(std::clamp(quotient, -limit, limit));
}

/// The smaller finite operand. When the operands compare equal (including
/// -0 against +0) the first operand is returned, as std::min does.
template <std::floating_point SampleType>
SampleType control_min(SampleType a, SampleType b) noexcept {
    return std::min(finite_control(a), finite_control(b));
}

/// The larger finite operand. When the operands compare equal (including
/// -0 against +0) the first operand is returned, as std::max does.
template <std::floating_point SampleType>
SampleType control_max(SampleType a, SampleType b) noexcept {
    return std::max(finite_control(a), finite_control(b));
}

/// Clamp a finite value to a finite interval. Reversed bounds are reordered
/// rather than refused, so the pure form never has undefined behaviour.
template <std::floating_point SampleType>
SampleType control_clamp(SampleType value, SampleType minimum, SampleType maximum) noexcept {
    const auto lo = finite_control(minimum);
    const auto hi = finite_control(maximum);
    return std::clamp(finite_control(value), std::min(lo, hi), std::max(lo, hi));
}

/// Pure one-sample bounded accumulation. The state is first bounded to the
/// interval, the finite increment is added in extended precision, and the sum
/// saturates at the interval endpoints. Reversed bounds are reordered.
template <std::floating_point SampleType>
SampleType integrator_step(SampleType state, SampleType increment, SampleType minimum,
                           SampleType maximum) noexcept {
    const auto lo = finite_control(minimum);
    const auto hi = finite_control(maximum);
    const auto bounded_state = control_clamp(state, lo, hi);
    const auto sum = static_cast<long double>(bounded_state) +
                     static_cast<long double>(finite_control(increment));
    return static_cast<SampleType>(std::clamp(sum, static_cast<long double>(std::min(lo, hi)),
                                              static_cast<long double>(std::max(lo, hi))));
}

/// Per-sample bounded accumulation over integrator_step(). Unlike the pure
/// step, configuration refuses reversed or non-finite bounds; a failed
/// configuration leaves the prior range and state untouched. Reset and
/// process allocate no memory.
template <std::floating_point SampleType = float> class ControlIntegratorT {
  public:
    bool configure(SampleType minimum, SampleType maximum,
                   SampleType initial = SampleType{0}) noexcept {
        if (!std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(initial) ||
            minimum > maximum)
            return false;
        minimum_ = minimum;
        maximum_ = maximum;
        initial_ = std::clamp(initial, minimum_, maximum_);
        state_ = initial_;
        return true;
    }

    void reset() noexcept {
        state_ = initial_;
    }

    SampleType process(SampleType increment) noexcept {
        state_ = integrator_step(state_, increment, minimum_, maximum_);
        return state_;
    }

    SampleType value() const noexcept {
        return state_;
    }

  private:
    SampleType minimum_ = SampleType{0};
    SampleType maximum_ = SampleType{1};
    SampleType initial_ = SampleType{0};
    SampleType state_ = SampleType{0};
};

using ControlIntegrator = ControlIntegratorT<float>;
using ControlIntegrator64 = ControlIntegratorT<double>;

} // namespace pulp::signal
