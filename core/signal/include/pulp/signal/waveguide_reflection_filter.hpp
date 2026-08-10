#pragma once

/// @file waveguide_reflection_filter.hpp
/// Passive one-pole waveguide termination.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// A passive boundary filter with
/// H(z) = g (1 - a) / (1 - a z^-1).
///
/// The sign of `g` selects open-end (negative) or rigid-end (positive)
/// reflection. For the legal ranges, |H| <= |g| < 1 at every frequency.
/// Every operation is bounded, lock-free, and allocation-free.
template <typename SampleType = float> class WaveguideReflectionFilterT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr double minimum_reflection_gain = -0.999;
    static constexpr double maximum_reflection_gain = 0.999;
    static constexpr double default_reflection_gain = -0.995;
    static constexpr double minimum_loss_pole = 0.0;
    // Leaves headroom for rate-mapped base-domain poles when this boundary is
    // evaluated inside an oversampled recursive loop.
    static constexpr double maximum_loss_pole = 0.9999;
    static constexpr double default_loss_pole = 0.20;

    void set_reflection_gain(SampleType gain) noexcept {
        if (std::isfinite(static_cast<double>(gain)))
            reflection_gain_ = std::clamp(static_cast<double>(gain), minimum_reflection_gain,
                                          maximum_reflection_gain);
    }

    void set_loss_pole(SampleType pole) noexcept {
        if (std::isfinite(static_cast<double>(pole)))
            loss_pole_ = std::clamp(static_cast<double>(pole), minimum_loss_pole,
                                    maximum_loss_pole);
    }

    [[nodiscard]] SampleType reflection_gain() const noexcept {
        return static_cast<SampleType>(reflection_gain_);
    }
    [[nodiscard]] SampleType loss_pole() const noexcept {
        return static_cast<SampleType>(loss_pole_);
    }

    [[nodiscard]] SampleType process(SampleType input) noexcept {
        const auto x = static_cast<double>(input);
        if (!std::isfinite(x))
            return finite_cast(last_output_);

        const auto output = reflection_gain_ * (1.0 - loss_pole_) * x +
                            loss_pole_ * last_output_;
        if (!std::isfinite(output)) {
            reset();
            return SampleType{};
        }
        last_output_ = snap_to_zero(output);
        return finite_cast(last_output_);
    }

    void reset() noexcept { last_output_ = 0.0; }

  private:
    [[nodiscard]] static SampleType finite_cast(double value) noexcept {
        const auto limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(value) || value > limit || value < -limit)
            return SampleType{};
        return static_cast<SampleType>(value);
    }

    double reflection_gain_ = default_reflection_gain;
    double loss_pole_ = default_loss_pole;
    double last_output_ = 0.0;
};

using WaveguideReflectionFilter = WaveguideReflectionFilterT<float>;
using WaveguideReflectionFilter64 = WaveguideReflectionFilterT<double>;

} // namespace pulp::signal
