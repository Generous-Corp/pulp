#pragma once

#include <pulp/signal/sinc_resampler.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>

namespace pulp::audio {

/// Immutable, allocation-free read kernel after control-thread construction.
///
/// `cutoff` is normalized to the source Nyquist. Construction allocates the
/// phase table and is not audio-thread safe; read() is bounded and RT-safe.
class PreparedSampleRateConversion {
  public:
    explicit PreparedSampleRateConversion(double cutoff) {
        kernel_.build(32, 512, 10.0, cutoff);
    }

    float read(std::span<const float> source, double position) const noexcept {
        if (source.empty() ||
            source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !std::isfinite(position))
            return 0.0f;
        position = std::clamp(position, 0.0, static_cast<double>(source.size() - 1));
        return kernel_.read(source.data(), static_cast<int>(source.size()), position);
    }

  private:
    signal::SincResampler kernel_;
};

/// Prepared reconstruction kernels for a runtime-varying read rate.
///
/// Construction is control-thread only. `read()` chooses the narrowest
/// prebuilt cutoff that does not exceed the source-position step, so host-tempo
/// playback can decimate without allocating or rebuilding a filter on the audio
/// thread. Steps beyond the finite prepared bank fail closed to silence instead
/// of selecting a filter whose cutoff would permit aliases.
class PreparedVariableRateConversion {
  public:
    static constexpr std::size_t kLinearCutoffLevels = 32;
    static constexpr std::size_t kStepsPerOctave = 8;
    static constexpr std::size_t kCutoffLevels = kLinearCutoffLevels + 19 * kStepsPerOctave + 1;
    static constexpr double kMaximumSourceFramesPerOutputFrame = 1'048'576.0;

    PreparedVariableRateConversion() {
        for (std::size_t level = 0; level < kernels_.size(); ++level) {
            const auto cutoff =
                level <= kLinearCutoffLevels
                    ? 1.0 - static_cast<double>(level) /
                                (2.0 * static_cast<double>(kLinearCutoffLevels))
                    : std::exp2(-1.0 - static_cast<double>(level - kLinearCutoffLevels) /
                                           static_cast<double>(kStepsPerOctave));
            kernels_[level].build(16, 128, 10.0, cutoff);
        }
    }

    float read(std::span<const float> source, double position,
               double source_frames_per_output_frame) const noexcept {
        if (source.empty() ||
            source.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !std::isfinite(position) || !std::isfinite(source_frames_per_output_frame) ||
            source_frames_per_output_frame <= 0.0 ||
            source_frames_per_output_frame > kMaximumSourceFramesPerOutputFrame)
            return 0.0f;
        position = std::clamp(position, 0.0, static_cast<double>(source.size() - 1));
        const auto required_cutoff = std::min(1.0, 1.0 / source_frames_per_output_frame);
        const auto level =
            required_cutoff >= 0.5
                ? static_cast<std::size_t>(std::ceil((1.0 - required_cutoff) * 2.0 *
                                                     static_cast<double>(kLinearCutoffLevels)))
                : kLinearCutoffLevels +
                      static_cast<std::size_t>(std::ceil(std::log2(0.5 / required_cutoff) *
                                                         static_cast<double>(kStepsPerOctave)));
        if (level >= kernels_.size())
            return 0.0f;
        return kernels_[level].read(source.data(), static_cast<int>(source.size()), position);
    }

  private:
    std::array<signal::SincResampler, kCutoffLevels> kernels_;
};

} // namespace pulp::audio
