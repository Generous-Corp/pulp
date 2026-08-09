#pragma once

#include <type_traits>

namespace pulp::signal::detail {

template <typename SampleType> struct SchroederAllpassStep {
    SampleType write;
    SampleType output;
};

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

/// Evaluates one Schroeder allpass recurrence while leaving delay storage,
/// interpolation, and non-finite recovery to the owning processor.
template <typename SampleType>
[[nodiscard]] constexpr SchroederAllpassStep<SampleType>
schroeder_allpass_step(SampleType input, SampleType delayed, SampleType gain) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const SampleType write = schroeder_allpass_write(input, delayed, gain);
    return {write, schroeder_allpass_output(delayed, write, gain)};
}

/// Evaluates the equivalent output-state recurrence used by delay lines whose
/// coefficient may change while stored history remains live.
template <typename SampleType>
[[nodiscard]] constexpr SchroederAllpassStep<SampleType>
schroeder_allpass_output_state_step(SampleType input, SampleType delayed,
                                    SampleType gain) noexcept {
    static_assert(std::is_floating_point_v<SampleType>);
    const SampleType output = delayed - gain * input;
    return {input + gain * output, output};
}

}  // namespace pulp::signal::detail
