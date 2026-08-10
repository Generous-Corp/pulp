#pragma once

/// @file waveguide_reed_exciter.hpp
/// Bounded memoryless single-reed valve for waveguide compositions.

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// A normalized pressure-controlled reed valve.
///
/// `bore_incident` is the arriving traveling-wave pressure proxy used by the
/// bounded Matthew/MSW-inspired scalar model; this type does not claim an
/// implicit physical pressure-junction solve. It owns no oversampler because
/// the composition that owns the recursive waveguide loop must oversample the
/// entire feedback path.
template <typename SampleType = float> class ReedExciterT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    static constexpr SampleType minimum_closing_pressure = SampleType{0.05};
    static constexpr SampleType maximum_closing_pressure = SampleType{1};
    static constexpr SampleType default_closing_pressure = SampleType{0.35};
    static constexpr SampleType minimum_flow_gain = SampleType{0.01};
    static constexpr SampleType maximum_flow_gain = SampleType{4};
    static constexpr SampleType default_flow_gain = SampleType{1};
    static constexpr SampleType minimum_bore_impedance = SampleType{1.0e-6};
    static constexpr SampleType maximum_bore_impedance = SampleType{16};
    static constexpr SampleType default_bore_impedance = SampleType{1};

    void set_closing_pressure(SampleType value) noexcept {
        set_finite_clamped(value, closing_pressure_, minimum_closing_pressure,
                           maximum_closing_pressure);
    }

    void set_flow_gain(SampleType value) noexcept {
        set_finite_clamped(value, flow_gain_, minimum_flow_gain, maximum_flow_gain);
    }

    void set_bore_impedance(SampleType value) noexcept {
        set_finite_clamped(value, bore_impedance_, minimum_bore_impedance,
                           maximum_bore_impedance);
    }

    [[nodiscard]] SampleType closing_pressure() const noexcept { return closing_pressure_; }
    [[nodiscard]] SampleType flow_gain() const noexcept { return flow_gain_; }
    [[nodiscard]] SampleType bore_impedance() const noexcept { return bore_impedance_; }

    /// Evaluates the explicit bounded valve equation.
    ///
    /// Non-finite input or an unrepresentable result returns the previous
    /// finite output without changing fallback state.
    [[nodiscard]] SampleType process(SampleType mouth_pressure,
                                     SampleType bore_incident) noexcept {
        if (!finite(mouth_pressure) || !finite(bore_incident))
            return last_finite_output_;

        const auto mouth = std::clamp(static_cast<double>(mouth_pressure), 0.0, 1.0);
        const auto incident = static_cast<double>(bore_incident);
        const auto pressure_difference = mouth - incident;
        const auto opening = std::clamp(
            1.0 - pressure_difference / static_cast<double>(closing_pressure_), 0.0, 1.0);
        const auto signed_root = pressure_difference < 0.0
                                     ? -std::sqrt(-pressure_difference)
                                     : std::sqrt(pressure_difference);
        const auto flow = static_cast<double>(flow_gain_) * opening * signed_root;
        const auto output = incident - static_cast<double>(bore_impedance_) * flow;
        const auto limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        if (!std::isfinite(output) || output > limit || output < -limit)
            return last_finite_output_;

        last_finite_output_ = static_cast<SampleType>(output);
        return last_finite_output_;
    }

    void reset() noexcept { last_finite_output_ = SampleType{}; }

  private:
    [[nodiscard]] static bool finite(SampleType value) noexcept {
        return std::isfinite(static_cast<double>(value));
    }

    static void set_finite_clamped(SampleType value, SampleType& destination,
                                   SampleType minimum, SampleType maximum) noexcept {
        if (finite(value))
            destination = std::clamp(value, minimum, maximum);
    }

    SampleType closing_pressure_ = default_closing_pressure;
    SampleType flow_gain_ = default_flow_gain;
    SampleType bore_impedance_ = default_bore_impedance;
    SampleType last_finite_output_{};
};

using ReedExciter = ReedExciterT<float>;
using ReedExciter64 = ReedExciterT<double>;

} // namespace pulp::signal
