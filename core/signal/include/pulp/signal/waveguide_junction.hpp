#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace pulp::signal {

/// A memoryless, fixed-capacity acoustic scattering junction.
///
/// Each configured port has a positive characteristic impedance. `scatter()`
/// computes the shared junction pressure and the outgoing traveling wave at
/// every port. Configuration and processing are allocation-free and may be
/// used on the audio thread.
template <typename SampleType = float, std::size_t MaxPorts = 4>
class WaveguideJunctionT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxPorts >= 2);
    static_assert(MaxPorts <= 4);

  public:
    using AccumulatorType = std::common_type_t<SampleType, double>;

    static constexpr std::size_t maximum_ports = MaxPorts;
    static constexpr SampleType minimum_impedance = static_cast<SampleType>(1.0e-6);

    constexpr WaveguideJunctionT() noexcept { impedances_.fill(SampleType{1}); }

    /// Selects the active port count. Invalid counts leave the configuration
    /// unchanged, so a bad control event cannot make the next audio call unsafe.
    constexpr void set_port_count(std::size_t count) noexcept {
        if (count >= 2 && count <= MaxPorts)
            port_count_ = count;
    }

    [[nodiscard]] constexpr std::size_t port_count() const noexcept { return port_count_; }

    /// Sets a port's characteristic impedance. Non-positive values clamp to a
    /// finite floor; non-finite values and invalid indices retain the last
    /// valid setting.
    void set_impedance(std::size_t port, SampleType impedance) noexcept {
        if (port >= MaxPorts || !std::isfinite(impedance))
            return;
        impedances_[port] = std::max(impedance, minimum_impedance);
    }

    [[nodiscard]] constexpr SampleType impedance(std::size_t port) const noexcept {
        return port < MaxPorts ? impedances_[port] : SampleType{};
    }

    /// Scatters `n` incoming pressure waves into `n` outgoing waves.
    ///
    /// `n` must equal the configured port count. Exact in-place operation is
    /// supported. A null input, count mismatch, non-finite sample, or
    /// unrepresentable result fails closed by clearing the bounded output.
    void scatter(const SampleType* incoming, SampleType* outgoing, std::size_t n) const noexcept {
        if (outgoing == nullptr)
            return;

        const auto clear_output = [&] {
            std::fill_n(outgoing, std::min(n, MaxPorts), SampleType{});
        };
        if (incoming == nullptr || n != port_count_ || n < 2 || n > MaxPorts) {
            clear_output();
            return;
        }

        AccumulatorType scale = AccumulatorType{};
        AccumulatorType reference_impedance =
            static_cast<AccumulatorType>(impedances_[0]);
        for (std::size_t port = 0; port < n; ++port) {
            if (!std::isfinite(incoming[port])) {
                clear_output();
                return;
            }
            scale = std::max(
                scale, std::abs(static_cast<AccumulatorType>(incoming[port])));
            reference_impedance = std::min(
                reference_impedance,
                static_cast<AccumulatorType>(impedances_[port]));
        }

        AccumulatorType total_weight = AccumulatorType{};
        AccumulatorType normalized_pressure = AccumulatorType{};
        for (std::size_t port = 0; port < n; ++port) {
            const auto weight = reference_impedance /
                                static_cast<AccumulatorType>(impedances_[port]);
            total_weight += weight;
            if (scale > AccumulatorType{})
                normalized_pressure += (static_cast<AccumulatorType>(incoming[port]) /
                                        scale) *
                                       weight;
        }
        const auto normalized_junction_pressure =
            AccumulatorType{2} * normalized_pressure / total_weight;
        if (!std::isfinite(normalized_junction_pressure)) {
            clear_output();
            return;
        }

        for (std::size_t port = 0; port < n; ++port) {
            const auto normalized_incoming =
                scale > AccumulatorType{}
                    ? static_cast<AccumulatorType>(incoming[port]) / scale
                    : AccumulatorType{};
            const auto sample = scale * (normalized_junction_pressure - normalized_incoming);
            if (!std::isfinite(sample)) {
                clear_output();
                return;
            }
            outgoing[port] = static_cast<SampleType>(sample);
            if (!std::isfinite(outgoing[port])) {
                clear_output();
                return;
            }
        }
    }

    /// There is no delay or recursive state to clear. Configuration survives
    /// reset so callers can treat this like other prepared signal primitives.
    constexpr void reset() noexcept {}

  private:
    std::array<SampleType, MaxPorts> impedances_{};
    std::size_t port_count_ = 2;
};

using WaveguideJunction = WaveguideJunctionT<float>;
using WaveguideJunction64 = WaveguideJunctionT<double>;

} // namespace pulp::signal
