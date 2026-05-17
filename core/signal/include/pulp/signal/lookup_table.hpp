#pragma once

/// @file lookup_table.hpp
/// Pre-calculated function table for fast waveshaper and math approximations.

#include <vector>
#include <functional>
#include <cmath>
#include <algorithm>

namespace pulp::signal {

/// Pre-computed function lookup table with linear interpolation.
///
/// Maps an input range [min, max] to pre-computed output values,
/// using linear interpolation between table entries for smooth results.
///
/// @code
/// // Fast tanh approximation with 1024 entries
/// LookupTable tanh_table(1024, -4.0f, 4.0f, [](float x) { return std::tanh(x); });
/// float out = tanh_table.process(input);
///
/// // Sine wavetable
/// LookupTable sine(4096, 0.0f, 1.0f, [](float phase) {
///     return std::sin(phase * 2.0f * 3.14159265f);
/// });
/// @endcode
class LookupTable {
public:
    LookupTable() = default;

    /// Construct with a generator function.
    /// @param size Number of table entries.
    /// @param min_input Minimum input value.
    /// @param max_input Maximum input value.
    /// @param generator Function that maps input to output.
    LookupTable(int size, float min_input, float max_input,
                std::function<float(float)> generator)
        : min_(min_input), max_(max_input)
    {
        if (size <= 0) return;

        table_.resize(static_cast<size_t>(size));

        if (size == 1 || max_ == min_) {
            std::fill(table_.begin(), table_.end(), generator(min_));
            inv_range_ = 0.0f;
            return;
        }

        if (max_ < min_)
            std::swap(min_, max_);

        inv_range_ = static_cast<float>(size - 1) / (max_ - min_);

        for (int i = 0; i < size; ++i) {
            float x = min_ + (max_ - min_) * static_cast<float>(i) / static_cast<float>(size - 1);
            table_[static_cast<size_t>(i)] = generator(x);
        }
    }

    /// Look up a value with linear interpolation.
    float process(float input) const {
        if (table_.empty()) return input;

        float index = (std::clamp(input, min_, max_) - min_) * inv_range_;
        int i0 = static_cast<int>(index);
        int i1 = std::min(i0 + 1, static_cast<int>(table_.size()) - 1);
        float frac = index - static_cast<float>(i0);

        return table_[static_cast<size_t>(i0)] * (1.0f - frac)
             + table_[static_cast<size_t>(i1)] * frac;
    }

    /// Process a buffer in-place.
    void process(float* data, int num_samples) const {
        for (int i = 0; i < num_samples; ++i) {
            data[i] = process(data[i]);
        }
    }

    /// Direct table access (no interpolation).
    float operator[](int index) const {
        if (table_.empty()) return 0.0f;

        return table_[static_cast<size_t>(std::clamp(index, 0,
            static_cast<int>(table_.size()) - 1))];
    }

    int size() const { return static_cast<int>(table_.size()); }
    bool empty() const { return table_.empty(); }

private:
    std::vector<float> table_;
    float min_ = 0.0f;
    float max_ = 1.0f;
    float inv_range_ = 1.0f;
};

} // namespace pulp::signal
