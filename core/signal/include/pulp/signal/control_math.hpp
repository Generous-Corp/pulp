#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>

namespace pulp::signal {

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

template <std::floating_point SampleType>
SampleType control_min(SampleType a, SampleType b) noexcept {
    return std::min(finite_control(a), finite_control(b));
}

template <std::floating_point SampleType>
SampleType control_max(SampleType a, SampleType b) noexcept {
    return std::max(finite_control(a), finite_control(b));
}

/// Per-sample bounded accumulation. Failed configuration leaves the prior
/// range and state untouched; reset and process allocate no memory.
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
        const long double sum =
            static_cast<long double>(state_) + static_cast<long double>(finite_control(increment));
        state_ = static_cast<SampleType>(std::clamp(sum, static_cast<long double>(minimum_),
                                                    static_cast<long double>(maximum_)));
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
