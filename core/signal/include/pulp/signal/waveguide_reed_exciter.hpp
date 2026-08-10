#pragma once

/// @file waveguide_reed_exciter.hpp
/// Bounded nonlinear single-reed waveguide boundary.

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// A pressure-controlled single-reed valve for a waveguide boundary.
///
/// Pressures and impedance are normalized dimensionless model coordinates, not
/// calibrated pascals or acoustic ohms. The square-root flow law is the only
/// nonlinearity; callers that need anti-aliasing must place the complete
/// feedback loop containing this boundary inside an oversampled callback.
/// `bore_incident` is the reduced model's bore coordinate, not total physical
/// mouthpiece pressure: the specified convention is `reflected = incident -
/// impedance * flow`. It must not be substituted for an implicitly coupled
/// two-port pressure/flow solve. Because the signed law permits reverse flow,
/// zero mouth pressure is an exact equilibrium but not a passive closed
/// termination after a disturbance. A closed boundary requires controls for
/// which the opening clamps to zero.
/// Every operation is bounded, lock-free, and allocation-free.
template <typename SampleType = float> class ReedExciterT {
    static_assert(std::is_floating_point_v<SampleType>);

  public:
    struct Evaluation {
        SampleType pressure_difference{};
        SampleType opening{};
        SampleType flow{};
        SampleType reflected_pressure{};
        bool valid = false;
    };

    static constexpr double minimum_mouth_pressure = 0.0;
    static constexpr double maximum_mouth_pressure = 1.0;
    static constexpr double default_mouth_pressure = 0.0;
    static constexpr double minimum_closing_pressure = 0.05;
    static constexpr double maximum_closing_pressure = 1.0;
    static constexpr double default_closing_pressure = 0.35;
    static constexpr double minimum_flow_gain = 0.01;
    static constexpr double maximum_flow_gain = 4.0;
    static constexpr double default_flow_gain = 1.0;
    static constexpr double minimum_bore_impedance = 1.0e-6;
    static constexpr double default_bore_impedance = 1.0;

    void set_mouth_pressure(SampleType pressure) noexcept {
        set_bounded(pressure, mouth_pressure_, minimum_mouth_pressure, maximum_mouth_pressure);
    }

    void set_closing_pressure(SampleType pressure) noexcept {
        set_bounded(pressure, closing_pressure_, minimum_closing_pressure,
                    maximum_closing_pressure);
    }

    void set_flow_gain(SampleType gain) noexcept {
        set_bounded(gain, flow_gain_, minimum_flow_gain, maximum_flow_gain);
    }

    void set_bore_impedance(SampleType impedance) noexcept {
        const auto value = static_cast<double>(impedance);
        if (std::isfinite(value))
            bore_impedance_ = std::max(value, minimum_bore_impedance);
    }

    [[nodiscard]] SampleType mouth_pressure() const noexcept {
        return static_cast<SampleType>(mouth_pressure_);
    }
    [[nodiscard]] SampleType closing_pressure() const noexcept {
        return static_cast<SampleType>(closing_pressure_);
    }
    [[nodiscard]] SampleType flow_gain() const noexcept {
        return static_cast<SampleType>(flow_gain_);
    }
    [[nodiscard]] SampleType bore_impedance() const noexcept {
        return finite_cast(bore_impedance_);
    }

    /// Evaluates the valve curve without reading or changing processor state.
    ///
    /// Control arguments must already be in their documented legal domains.
    /// Invalid or unrepresentable inputs return `valid == false` and zeroed
    /// coordinates. This seam lets visualizers and independent tests inspect
    /// the nonlinear boundary without advancing an audio loop.
    [[nodiscard]] static Evaluation evaluate(SampleType bore_incident, SampleType mouth_pressure,
                                             SampleType closing_pressure, SampleType flow_gain,
                                             SampleType bore_impedance) noexcept {
        const auto incident = static_cast<double>(bore_incident);
        const auto mouth = static_cast<double>(mouth_pressure);
        const auto closing = static_cast<double>(closing_pressure);
        const auto gain = static_cast<double>(flow_gain);
        const auto impedance = static_cast<double>(bore_impedance);
        if (!std::isfinite(incident) || !std::isfinite(mouth) || !std::isfinite(closing) ||
            !std::isfinite(gain) || !std::isfinite(impedance) ||
            mouth < sample_bound(minimum_mouth_pressure) ||
            mouth > sample_bound(maximum_mouth_pressure) ||
            closing < sample_bound(minimum_closing_pressure) ||
            closing > sample_bound(maximum_closing_pressure) ||
            gain < sample_bound(minimum_flow_gain) || gain > sample_bound(maximum_flow_gain) ||
            impedance < sample_bound(minimum_bore_impedance))
            return {};

        const auto pressure_difference = mouth - incident;
        const auto opening = std::clamp(1.0 - pressure_difference / closing, 0.0, 1.0);
        const auto flow = std::copysign(gain * opening * std::sqrt(std::abs(pressure_difference)),
                                        pressure_difference);
        const auto reflected = incident - impedance * flow;
        if (!representable(pressure_difference) || !representable(opening) ||
            !representable(flow) || !representable(reflected))
            return {};

        return {static_cast<SampleType>(pressure_difference), static_cast<SampleType>(opening),
                static_cast<SampleType>(flow), static_cast<SampleType>(reflected), true};
    }

    /// Reflects one incoming bore-pressure wave from the nonlinear reed valve.
    ///
    /// A non-finite input or unrepresentable result returns the previous finite
    /// output without updating it, so one bad sample cannot poison a recursive
    /// waveguide loop.
    [[nodiscard]] SampleType process(SampleType bore_incident) noexcept {
        const auto incident = static_cast<double>(bore_incident);
        if (!std::isfinite(incident))
            return finite_cast(last_output_);

        const auto evaluation =
            evaluate(bore_incident, static_cast<SampleType>(mouth_pressure_),
                     static_cast<SampleType>(closing_pressure_),
                     static_cast<SampleType>(flow_gain_), static_cast<SampleType>(bore_impedance_));
        if (!evaluation.valid)
            return finite_cast(last_output_);

        last_output_ = static_cast<double>(evaluation.reflected_pressure);
        return evaluation.reflected_pressure;
    }

    void reset() noexcept {
        last_output_ = 0.0;
    }

  private:
    [[nodiscard]] static constexpr double sample_bound(double value) noexcept {
        return static_cast<double>(static_cast<SampleType>(value));
    }

    static void set_bounded(SampleType input, double& destination, double minimum,
                            double maximum) noexcept {
        const auto value = static_cast<double>(input);
        if (std::isfinite(value))
            destination = std::clamp(value, minimum, maximum);
    }

    [[nodiscard]] static bool representable(double value) noexcept {
        const auto limit = static_cast<double>(std::numeric_limits<SampleType>::max());
        return std::isfinite(value) && value <= limit && value >= -limit;
    }

    [[nodiscard]] static SampleType finite_cast(double value) noexcept {
        return representable(value) ? static_cast<SampleType>(value) : SampleType{};
    }

    double mouth_pressure_ = default_mouth_pressure;
    double closing_pressure_ = default_closing_pressure;
    double flow_gain_ = default_flow_gain;
    double bore_impedance_ = default_bore_impedance;
    double last_output_ = 0.0;
};

using ReedExciter = ReedExciterT<float>;
using ReedExciter64 = ReedExciterT<double>;

} // namespace pulp::signal
