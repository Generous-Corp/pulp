#pragma once

#include <pulp/signal/analysis_frontends.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace pulp::signal {

/// One complete control-rate spectral-feature window.
///
/// Centroid and rolloff are in Hz. Flatness and flux are dimensionless in
/// [0, 1]. The first valid window has zero flux because it has no predecessor.
/// A silent window is emitted with finite zero-valued features and valid=false.
template <typename SampleType> struct SpectralFeatureFrameT {
    double centroid_hz = 0.0;
    double flatness = 0.0;
    double rolloff_hz = 0.0;
    double flux = 0.0;
    std::uint64_t window_start_frame = 0;
    std::uint64_t window_center_frame = 0;
    std::uint64_t ready_at_frame = 0;
    std::uint64_t sequence = 0;
    bool valid = false;
};

/// Fixed-capacity streaming centroid, flatness, rolloff, and flux analysis.
///
/// This reuses the shared StreamingAnalysisWindowT FFT owner with its symmetric
/// Hann window. prepare() is
/// control-thread work and may allocate only inside FftT. After a successful
/// prepare, reset() and process() allocate nothing. The instance and its
/// nothrow sink have one processing-thread owner; callers publish copied frames
/// across threads. Rolloff is the first bin containing 85% of spectral power.
/// Flux is the Euclidean distance between consecutive unit-sum magnitude
/// spectra, divided by sqrt(2), so gain-only changes do not become modulation.
template <typename SampleType = float, std::size_t MaxFftSize = 4096, std::size_t MaxChannels = 8>
class SpectralFeatureFrontEndT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxFftSize >= 2);
    static_assert(MaxChannels > 0);

  public:
    using Frame = SpectralFeatureFrameT<SampleType>;

    [[nodiscard]] bool prepare(const StreamingAnalysisConfig& config) {
        if (config.fft_size < 4u)
            return false;
        if (!window_.prepare(config, detail::AnalysisWindow::symmetric_hann))
            return false;
        previous_normalized_magnitudes_.fill(0.0);
        have_previous_ = false;
        sequence_ = 0;
        current_ = {};
        return true;
    }

    void reset() noexcept {
        window_.reset();
        previous_normalized_magnitudes_.fill(0.0);
        have_previous_ = false;
        sequence_ = 0;
        current_ = {};
    }

    template <typename Sink>
    [[nodiscard]] bool process(const SampleType* const* channels, std::size_t channel_count,
                               std::size_t frames, Sink&& sink) noexcept {
        static_assert(std::is_nothrow_invocable_v<Sink&, const Frame&>,
                      "analysis sinks must be noexcept");
        if (!window_.prepared() || channel_count != window_.config().channels ||
            channels == nullptr)
            return false;
        for (std::size_t channel = 0; channel < channel_count; ++channel) {
            if (channels[channel] == nullptr)
                return false;
        }

        bool finite = true;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            double mono{};
            double power{};
            if (!detail::mix_planar_frame<SampleType, MaxChannels>(channels, channel_count, frame,
                                                                   false, mono, power)) {
                reject_sample();
                finite = false;
                continue;
            }
            if (!window_.push(mono, power))
                continue;
            if (!render_frame()) {
                clear_history_after_fault();
                finite = false;
                continue;
            }
            sink(current_);
        }
        return finite;
    }

    [[nodiscard]] const Frame& current() const noexcept {
        return current_;
    }
    [[nodiscard]] std::uint32_t algorithmic_latency_samples() const noexcept {
        return window_.prepared() ? window_.config().fft_size / 2u : 0u;
    }
    [[nodiscard]] std::uint32_t startup_samples() const noexcept {
        return window_.prepared() ? window_.config().fft_size : 0u;
    }
    [[nodiscard]] std::uint64_t retained_fft_bytes() const noexcept {
        return window_.retained_fft_bytes();
    }

  private:
    void clear_history_after_fault() noexcept {
        window_.clear_history_after_fault();
        previous_normalized_magnitudes_.fill(0.0);
        have_previous_ = false;
    }

    void reject_sample() noexcept {
        window_.reject_sample();
        previous_normalized_magnitudes_.fill(0.0);
        have_previous_ = false;
    }

    [[nodiscard]] bool render_frame() noexcept {
        if (!window_.render_spectrum())
            return false;

        const auto& config = window_.config();
        const auto& spectrum = window_.spectrum();
        const auto bins = config.fft_size / 2u + 1u;
        double maximum_magnitude = 0.0;
        for (std::size_t bin = 0; bin < bins; ++bin) {
            const auto magnitude = static_cast<double>(std::abs(spectrum[bin]));
            if (!std::isfinite(magnitude))
                return false;
            maximum_magnitude = std::max(maximum_magnitude, magnitude);
        }

        Frame next{};
        if (maximum_magnitude > 0.0) {
            constexpr long double rolloff_fraction = 0.85L;
            constexpr long double flatness_floor = 1.0e-20L;
            constexpr long double inverse_sqrt_two = 0.70710678118654752440L;
            long double scaled_magnitude_sum = 0.0L;
            long double scaled_power_sum = 0.0L;
            for (std::size_t bin = 0; bin < bins; ++bin) {
                const auto scaled_magnitude = static_cast<long double>(std::abs(spectrum[bin])) /
                                              static_cast<long double>(maximum_magnitude);
                scaled_magnitude_sum += scaled_magnitude;
                scaled_power_sum += scaled_magnitude * scaled_magnitude;
            }
            if (!std::isfinite(scaled_magnitude_sum) || !std::isfinite(scaled_power_sum) ||
                scaled_magnitude_sum <= 0.0L || scaled_power_sum <= 0.0L)
                return false;

            long double weighted_frequency = 0.0L;
            long double log_sum = 0.0L;
            long double cumulative_power = 0.0L;
            long double flux_squared = 0.0L;
            bool rolloff_found = false;
            for (std::size_t bin = 0; bin < bins; ++bin) {
                const auto frequency =
                    static_cast<long double>(config.sample_rate) *
                    (static_cast<long double>(bin) / static_cast<long double>(config.fft_size));
                const auto scaled_magnitude = static_cast<long double>(std::abs(spectrum[bin])) /
                                              static_cast<long double>(maximum_magnitude);
                const auto normalized =
                    static_cast<double>(scaled_magnitude / scaled_magnitude_sum);
                weighted_frequency += frequency * static_cast<long double>(normalized);
                log_sum += std::log(std::max(static_cast<long double>(normalized), flatness_floor));
                if (have_previous_) {
                    const auto delta =
                        static_cast<long double>(normalized - previous_normalized_magnitudes_[bin]);
                    flux_squared += delta * delta;
                }
                previous_normalized_magnitudes_[bin] = normalized;
                cumulative_power += scaled_magnitude * scaled_magnitude;
                if (!rolloff_found && cumulative_power >= rolloff_fraction * scaled_power_sum) {
                    next.rolloff_hz = static_cast<double>(frequency);
                    rolloff_found = true;
                }
            }
            next.centroid_hz = static_cast<double>(weighted_frequency);
            const auto arithmetic_mean = 1.0L / static_cast<long double>(bins);
            const auto geometric_mean = std::exp(log_sum / static_cast<long double>(bins));
            next.flatness =
                static_cast<double>(std::clamp(geometric_mean / arithmetic_mean, 0.0L, 1.0L));
            next.flux = have_previous_
                            ? static_cast<double>(std::clamp(
                                  std::sqrt(flux_squared) * inverse_sqrt_two, 0.0L, 1.0L))
                            : 0.0;
            next.valid = true;
        } else {
            previous_normalized_magnitudes_.fill(0.0);
        }
        have_previous_ = next.valid;

        if (!std::isfinite(next.centroid_hz) || !std::isfinite(next.flatness) ||
            !std::isfinite(next.rolloff_hz) || !std::isfinite(next.flux))
            return false;
        next.ready_at_frame = window_.stream_frames() - 1u;
        next.window_start_frame = window_.stream_frames() - config.fft_size;
        next.window_center_frame = next.window_start_frame + config.fft_size / 2u;
        next.sequence = ++sequence_;
        current_ = next;
        return true;
    }

    detail::StreamingAnalysisWindowT<SampleType, MaxFftSize, MaxChannels> window_{};
    std::array<double, MaxFftSize / 2u + 1u> previous_normalized_magnitudes_{};
    Frame current_{};
    std::uint64_t sequence_ = 0;
    bool have_previous_ = false;
};

using SpectralFeatureFrontEnd = SpectralFeatureFrontEndT<float>;
using SpectralFeatureFrontEnd64 = SpectralFeatureFrontEndT<double>;

} // namespace pulp::signal
