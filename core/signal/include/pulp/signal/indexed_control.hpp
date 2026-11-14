#pragma once

/// @file indexed_control.hpp
/// Bounded scalar control utilities for caller-owned, sample-indexed workflows.
/// The caller supplies the time index on every sample; no clock or scheduler is
/// hidden in these utilities. Configure them off the audio thread.

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <limits>
#include <span>

namespace pulp::signal {

/// Replace a non-finite control sample with zero at a scalar boundary.
template <std::floating_point SampleType>
inline SampleType finite_control_sample(SampleType value) noexcept {
    return std::isfinite(value) ? value : SampleType{0};
}

/// Pure bounded scalar stages. Bounds are ordered by the caller and non-finite
/// inputs become zero, so a control lane never propagates NaN or infinity.
template <std::floating_point SampleType>
inline SampleType control_minimum(SampleType a, SampleType b) noexcept {
    return std::min(finite_control_sample(a), finite_control_sample(b));
}

template <std::floating_point SampleType>
inline SampleType control_maximum(SampleType a, SampleType b) noexcept {
    return std::max(finite_control_sample(a), finite_control_sample(b));
}

template <std::floating_point SampleType>
inline SampleType control_clamp(SampleType value, SampleType minimum, SampleType maximum) noexcept {
    const auto lo = finite_control_sample(minimum);
    const auto hi = finite_control_sample(maximum);
    const auto input = finite_control_sample(value);
    return std::clamp(input, std::min(lo, hi), std::max(lo, hi));
}

/// Pure interval mapping with endpoint clipping. An invalid input interval
/// returns the finite output minimum, matching MapClipT's safe fallback.
template <std::floating_point SampleType>
inline SampleType map_clip(SampleType value, SampleType input_min, SampleType input_max,
                           SampleType output_min, SampleType output_max) noexcept {
    input_min = finite_control_sample(input_min);
    input_max = finite_control_sample(input_max);
    output_min = finite_control_sample(output_min);
    output_max = finite_control_sample(output_max);
    if (!(input_max > input_min))
        return output_min;
    value = finite_control_sample(value);
    if (value <= input_min)
        return output_min;
    if (value >= input_max)
        return output_max;
    const auto span = input_max - input_min;
    const auto unit = std::isfinite(span)
                          ? (value - input_min) / span
                          : (value * SampleType{0.5} - input_min * SampleType{0.5}) /
                                (input_max * SampleType{0.5} - input_min * SampleType{0.5});
    return std::lerp(output_min, output_max, unit);
}

template <std::floating_point SampleType>
SampleType control_safe_divide(SampleType numerator, SampleType denominator) noexcept {
    numerator = finite_control_sample(numerator);
    denominator = finite_control_sample(denominator);
    if (denominator == SampleType{0})
        return SampleType{0};
    const long double quotient =
        static_cast<long double>(numerator) / static_cast<long double>(denominator);
    const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
    return static_cast<SampleType>(std::clamp(quotient, -limit, limit));
}

/// Pure Schmitt step. `high` latches true, `low` releases it, and the prior
/// state is supplied explicitly so the stage can be replayed without hidden
/// clock or allocation state.
template <std::floating_point SampleType>
inline bool hysteresis_step(SampleType input, SampleType high, SampleType low,
                            bool prior_high) noexcept {
    if (!std::isfinite(input) || !std::isfinite(high) || !std::isfinite(low))
        return prior_high;
    if (prior_high)
        return input > low;
    return input >= high;
}

template <std::floating_point SampleType = float> class HysteresisT {
  public:
    bool configure(SampleType high, SampleType low, bool initial = false) noexcept {
        if (!std::isfinite(high) || !std::isfinite(low) || !(high > low))
            return false;
        high_ = high;
        low_ = low;
        high_state_ = initial;
        return true;
    }
    void reset(bool high = false) noexcept {
        high_state_ = high;
    }
    bool process(SampleType input) noexcept {
        high_state_ = hysteresis_step(input, high_, low_, high_state_);
        return high_state_;
    }
    bool high() const noexcept {
        return high_state_;
    }

  private:
    SampleType high_ = SampleType{0.5};
    SampleType low_ = SampleType{0.25};
    bool high_state_ = false;
};

/// Pure one-sample integrator step. The increment is clamped to finite zero
/// before addition and the result is bounded to [minimum, maximum].
template <std::floating_point SampleType>
inline SampleType integrator_step(SampleType state, SampleType increment, SampleType minimum,
                                  SampleType maximum) noexcept {
    const auto lo = finite_control_sample(minimum);
    const auto hi = finite_control_sample(maximum);
    const auto bounded_state = control_clamp(state, lo, hi);
    const auto sum = static_cast<long double>(bounded_state) +
                     static_cast<long double>(finite_control_sample(increment));
    return static_cast<SampleType>(std::clamp(sum, static_cast<long double>(std::min(lo, hi)),
                                              static_cast<long double>(std::max(lo, hi))));
}

/// Pure linear slew step. Rates are output units per sample; a zero rate holds
/// the current value, and a non-finite target is treated as zero.
template <std::floating_point SampleType>
inline SampleType slew_step(SampleType current, SampleType target, SampleType rise_rate,
                            SampleType fall_rate) noexcept {
    current = finite_control_sample(current);
    target = finite_control_sample(target);
    rise_rate = std::max(SampleType{}, finite_control_sample(rise_rate));
    fall_rate = std::max(SampleType{}, finite_control_sample(fall_rate));
    const auto delta = target - current;
    if (delta == SampleType{})
        return target;
    const auto step = delta > SampleType{} ? rise_rate : fall_rate;
    if (step == SampleType{})
        return current;
    return delta > SampleType{} ? std::min(target, current + step)
                                : std::max(target, current - step);
}

template <std::floating_point SampleType = float> class SlewT {
  public:
    void set_rates(SampleType rise_per_sample, SampleType fall_per_sample) noexcept {
        rise_rate_ = std::max(SampleType{}, finite_control_sample(rise_per_sample));
        fall_rate_ = std::max(SampleType{}, finite_control_sample(fall_per_sample));
    }
    void reset(SampleType value = SampleType{}) noexcept {
        value_ = finite_control_sample(value);
    }
    SampleType process(SampleType target) noexcept {
        value_ = slew_step(value_, target, rise_rate_, fall_rate_);
        return value_;
    }
    SampleType value() const noexcept {
        return value_;
    }

  private:
    SampleType rise_rate_ = SampleType{1};
    SampleType fall_rate_ = SampleType{1};
    SampleType value_ = SampleType{};
};

using Hysteresis = HysteresisT<float>;
using Slew = SlewT<float>;

/// Map a finite control interval to another interval and clip outside it.
/// Invalid configuration is rejected without changing the previous mapping.
template <typename SampleType = float> class MapClipT {
  public:
    static_assert(std::floating_point<SampleType>);
    bool configure(SampleType input_min, SampleType input_max, SampleType output_min,
                   SampleType output_max) noexcept {
        if (!std::isfinite(input_min) || !std::isfinite(input_max) || !std::isfinite(output_min) ||
            !std::isfinite(output_max) || !(input_max > input_min))
            return false;
        input_min_ = input_min;
        input_max_ = input_max;
        output_min_ = output_min;
        output_max_ = output_max;
        return true;
    }

    SampleType process(SampleType input) const noexcept {
        return map_clip(input, input_min_, input_max_, output_min_, output_max_);
    }

  private:
    SampleType input_min_ = 0;
    SampleType input_max_ = 1;
    SampleType output_min_ = 0;
    SampleType output_max_ = 1;
};

using MapClip = MapClipT<float>;
using MapClip64 = MapClipT<double>;

/// Read an externally indexed control waveform. Index is in table samples and
/// may advance, pause, or reverse. The owner keeps the table alive and stable
/// until this view is replaced; a zero length table returns zero. Out-of-range
/// indices hold the nearest endpoint, and non-finite indices return zero.
template <typename SampleType = float> class IndexedControlT {
  public:
    static_assert(std::floating_point<SampleType>);
    void set_table(std::span<const SampleType> table) noexcept {
        table_ = table;
    }

    SampleType process(double index) const noexcept {
        if (table_.empty() || !std::isfinite(index))
            return SampleType{};
        if (index <= 0.0)
            return table_.front();
        const auto last = table_.size() - 1;
        if (index >= static_cast<double>(last))
            return table_.back();
        const auto left = static_cast<std::size_t>(index);
        const auto fraction = static_cast<SampleType>(index - static_cast<double>(left));
        return std::lerp(table_[left], table_[left + 1], fraction);
    }

  private:
    std::span<const SampleType> table_;
};

using IndexedControl = IndexedControlT<float>;
using IndexedControl64 = IndexedControlT<double>;

} // namespace pulp::signal
