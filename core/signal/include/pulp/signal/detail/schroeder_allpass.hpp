#pragma once

#include <type_traits>

/// The two halves of one Schroeder allpass recurrence,
/// `v[n] = x[n] + g*v[n-d]` and `y[n] = v[n-d] - g*v[n]`, as pure functions.
/// Delay storage, interpolation, denormal snapping, and non-finite recovery
/// stay with the owning processor, which is why this carries no state.
namespace pulp::signal::detail {

template <typename SampleType>
[[nodiscard]] constexpr SampleType schroeder_allpass_write(
    SampleType input, SampleType delayed, SampleType gain) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    return input + gain * delayed;
}

template <typename SampleType>
[[nodiscard]] constexpr SampleType schroeder_allpass_output(
    SampleType delayed, SampleType write, SampleType gain) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    return delayed - gain * write;
}

}  // namespace pulp::signal::detail
