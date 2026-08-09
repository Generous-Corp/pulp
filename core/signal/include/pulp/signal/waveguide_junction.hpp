#pragma once

/// @file waveguide_junction.hpp
/// Fixed-capacity lossless waveguide scattering junction.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace pulp::signal {

/// A bounded N-port pressure-wave scattering junction.
///
/// The characteristic impedances are positive dimensionless ratios. `scatter`
/// permits exact input/output aliasing and uses double/long-double arithmetic
/// internally. Every operation is bounded, lock-free, and allocation-free.
template <typename SampleType = float, std::size_t MaxPorts = 4> class WaveguideJunctionT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxPorts >= 2);
    static_assert(MaxPorts <= 4);

  public:
    static constexpr double minimum_impedance = 1.0e-6;

    WaveguideJunctionT() { impedances_.fill(1.0); }

    void set_port_count(std::size_t count) noexcept {
        if (count >= 2 && count <= MaxPorts)
            port_count_ = count;
    }

    [[nodiscard]] std::size_t port_count() const noexcept { return port_count_; }

    void set_impedance(std::size_t port_index, SampleType impedance) noexcept {
        if (port_index >= MaxPorts)
            return;
        const auto value = static_cast<double>(impedance);
        if (!std::isfinite(value))
            return;
        impedances_[port_index] = std::max(value, minimum_impedance);
    }

    [[nodiscard]] SampleType impedance(std::size_t port_index) const noexcept {
        return port_index < MaxPorts ? static_cast<SampleType>(impedances_[port_index])
                                     : SampleType{};
    }

    /// Scatters one incoming pressure wave per active port.
    ///
    /// `count` must equal the configured port count. Invalid arguments return
    /// false without writing through unknown pointers. A non-finite incoming
    /// sample returns the previous finite output vector and does not update it.
    [[nodiscard]] bool scatter(const SampleType* incoming, SampleType* outgoing,
                               std::size_t count) noexcept {
        if (incoming == nullptr || outgoing == nullptr || count != port_count_)
            return false;

        std::array<long double, MaxPorts> input{};
        long double weighted_pressure = 0.0L;
        long double reciprocal_sum = 0.0L;
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = static_cast<long double>(incoming[i]);
            if (!std::isfinite(value)) {
                publish_previous(outgoing, count);
                return false;
            }
            input[i] = value;
            const auto reciprocal = 1.0L / static_cast<long double>(impedances_[i]);
            weighted_pressure += value * reciprocal;
            reciprocal_sum += reciprocal;
        }
        if (!(reciprocal_sum > 0.0L) || !std::isfinite(weighted_pressure) ||
            !std::isfinite(reciprocal_sum)) {
            publish_previous(outgoing, count);
            return false;
        }

        const auto junction_pressure = 2.0L * weighted_pressure / reciprocal_sum;
        std::array<SampleType, MaxPorts> replacement{};
        for (std::size_t i = 0; i < count; ++i) {
            const auto value = junction_pressure - input[i];
            if (!finite_sample(value)) {
                publish_previous(outgoing, count);
                return false;
            }
            replacement[i] = static_cast<SampleType>(value);
        }
        for (std::size_t i = 0; i < count; ++i) {
            outgoing[i] = replacement[i];
            last_output_[i] = replacement[i];
        }
        return true;
    }

    void reset() noexcept { last_output_.fill(SampleType{}); }

  private:
    [[nodiscard]] static bool finite_sample(long double value) noexcept {
        const auto limit = static_cast<long double>(std::numeric_limits<SampleType>::max());
        return std::isfinite(value) && value <= limit && value >= -limit;
    }

    void publish_previous(SampleType* outgoing, std::size_t count) const noexcept {
        for (std::size_t i = 0; i < count; ++i)
            outgoing[i] = last_output_[i];
    }

    std::array<double, MaxPorts> impedances_{};
    std::array<SampleType, MaxPorts> last_output_{};
    std::size_t port_count_ = 2;
};

using WaveguideJunction = WaveguideJunctionT<float>;
using WaveguideJunction64 = WaveguideJunctionT<double>;

} // namespace pulp::signal
