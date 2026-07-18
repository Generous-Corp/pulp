#pragma once

#include <pulp/audio/sample_interpolation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace pulp::test::audio {

inline constexpr double kSamplerQualityAmplitude = 0.5;

inline std::vector<double> render_interpolated_tone_prepared(
    const pulp::audio::PreparedSampleInterpolation& interpolation,
    double source_frames_per_output,
    double source_cycles_per_sample,
    std::size_t output_frames,
    std::size_t block_frames = 64) {
    if (!(source_frames_per_output > 0.0) ||
        !(source_cycles_per_sample > 0.0 && source_cycles_per_sample < 0.5) ||
        output_frames == 0 || block_frames == 0) {
        throw std::invalid_argument("invalid interpolation render contract");
    }

    if (!interpolation.valid())
        throw std::invalid_argument("interpolation is not valid for ratio");

    std::vector<double> output(output_frames);
    std::array<float, pulp::audio::kMaximumSampleInterpolationTaps> taps{};
    for (std::size_t block_start = 0; block_start < output_frames;
         block_start += block_frames) {
        const auto block_end = std::min(output_frames, block_start + block_frames);
        for (std::size_t frame = block_start; frame < block_end; ++frame) {
            const double position = static_cast<double>(frame) * source_frames_per_output;
            const auto base = static_cast<std::int64_t>(std::floor(position));
            const float fraction = static_cast<float>(position - static_cast<double>(base));
            const auto footprint = interpolation.footprint(fraction);
            for (std::uint32_t tap = 0; tap < footprint.tap_count; ++tap) {
                const auto source_frame = base + footprint.first_offset +
                    static_cast<std::int64_t>(tap);
                taps[tap] = static_cast<float>(
                    kSamplerQualityAmplitude *
                    std::sin(2.0 * std::numbers::pi * source_cycles_per_sample *
                             static_cast<double>(source_frame)));
            }
            output[frame] = interpolation.evaluate(
                fraction, std::span<const float>(taps.data(), footprint.tap_count));
        }
    }
    return output;
}

inline std::vector<double> render_interpolated_tone(
    pulp::audio::SampleInterpolationPolicy policy,
    double source_frames_per_output,
    double source_cycles_per_sample,
    std::size_t output_frames,
    std::size_t block_frames = 64) {
    static pulp::audio::SampleSincKernelBank sinc_bank;
    static const bool sinc_bank_built =
        sinc_bank.build_dense_for_maximum_consumption(4.0);
    if (!sinc_bank_built)
        throw std::runtime_error("could not build sampler sinc bank");

    pulp::audio::PreparedSampleInterpolation interpolation{.policy = policy};
    if (policy == pulp::audio::SampleInterpolationPolicy::RatioTrackingSinc)
        interpolation.sinc = sinc_bank.view().select(source_frames_per_output);
    return render_interpolated_tone_prepared(
        interpolation, source_frames_per_output, source_cycles_per_sample,
        output_frames, block_frames);
}

inline const char* sample_interpolation_policy_cli_id(
    pulp::audio::SampleInterpolationPolicy policy) noexcept {
    using Policy = pulp::audio::SampleInterpolationPolicy;
    switch (policy) {
        case Policy::Hold: return "hold";
        case Policy::Nearest: return "nearest";
        case Policy::Linear: return "linear";
        case Policy::CubicHermite: return "cubic-hermite";
        case Policy::CubicLagrange: return "cubic-lagrange";
        case Policy::RatioTrackingSinc: return "ratio-sinc";
    }
    return "unknown";
}

} // namespace pulp::test::audio
