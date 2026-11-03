#pragma once

/// @file indexed_control.hpp
/// Bounded scalar control utilities for caller-owned, sample-indexed workflows.
/// The caller supplies the time index on every sample; no clock or scheduler is
/// hidden in these utilities. Configure them off the audio thread.

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <span>

namespace pulp::signal {

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
        if (!std::isfinite(input))
            return output_min_;
        const auto unit =
            (std::clamp(input, input_min_, input_max_) - input_min_) / (input_max_ - input_min_);
        return output_min_ + unit * (output_max_ - output_min_);
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
        return table_[left] + (table_[left + 1] - table_[left]) * fraction;
    }

  private:
    std::span<const SampleType> table_;
};

using IndexedControl = IndexedControlT<float>;
using IndexedControl64 = IndexedControlT<double>;

} // namespace pulp::signal
