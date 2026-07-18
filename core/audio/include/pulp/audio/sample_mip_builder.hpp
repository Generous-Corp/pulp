#pragma once

#include <pulp/signal/windowed_sinc_design.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace pulp::audio {

inline std::vector<double> design_sample_mip_decimator() {
    constexpr double stopband_db = 140.0;
    const auto beta = signal::kaiser_beta_for_stopband(stopband_db);
    const auto taps = signal::kaiser_length_for_transition(stopband_db, 0.025);
    return signal::design_windowed_sinc(taps, 0.2375, beta);
}

inline void decimate_sample_mip_2x(const float* input,
                                   std::uint64_t input_frames,
                                   float* output,
                                   std::uint64_t output_frames,
                                   const std::vector<double>& coefficients) noexcept {
    if (input == nullptr || output == nullptr || input_frames == 0 ||
        coefficients.empty() || (coefficients.size() & 1u) == 0u) {
        return;
    }
    const auto radius = static_cast<std::int64_t>(coefficients.size() / 2);
    const auto last = static_cast<std::int64_t>(input_frames - 1);
    for (std::uint64_t frame = 0; frame < output_frames; ++frame) {
        const auto center = static_cast<std::int64_t>(frame * 2);
        double sum = static_cast<double>(input[std::clamp(
            center, std::int64_t{0}, last)]) * coefficients[static_cast<std::size_t>(radius)];
        for (std::int64_t tap = 1; tap <= radius; ++tap) {
            const auto before = std::clamp(center - tap, std::int64_t{0}, last);
            const auto after = std::clamp(center + tap, std::int64_t{0}, last);
            sum += (static_cast<double>(input[before]) +
                    static_cast<double>(input[after])) *
                   coefficients[static_cast<std::size_t>(radius + tap)];
        }
        output[frame] = static_cast<float>(sum);
    }
}

} // namespace pulp::audio
