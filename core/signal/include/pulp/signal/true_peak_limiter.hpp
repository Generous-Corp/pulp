#pragma once

/// @file true_peak_limiter.hpp
/// Fixed-capacity, look-ahead true-peak limiter for interleaved audio.

#include <pulp/signal/dynamics_core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>

namespace pulp::signal {

template <typename SampleType = float, std::size_t MaxChannels = 8> class TruePeakLimiterT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxChannels > 0);

    enum class ChannelLink { linked, independent };

    struct Params {
        double ceiling_dbtp = -1.0;
        double lookahead_ms = 5.0;
        double release_ms = 100.0;
        ChannelLink channel_link = ChannelLink::linked;
    };

    static constexpr std::size_t interpolation_factor() noexcept {
        return 4;
    }
    static constexpr std::size_t interpolation_taps() noexcept {
        return 129;
    }
    static constexpr int detector_latency_samples() noexcept {
        return static_cast<int>((interpolation_taps() - 1) / 2);
    }
    static constexpr double detector_guard_db() noexcept {
        return 0.20;
    }
    static constexpr double maximum_supported_sample_rate() noexcept {
        return 384000.0;
    }
    static constexpr double maximum_lookahead_ms() noexcept {
        return 20.0;
    }

    bool prepare(double sample_rate, std::size_t channels, Params params = {}) {
        if (!(std::isfinite(sample_rate) && sample_rate >= 8000.0 &&
              sample_rate <= maximum_supported_sample_rate()) ||
            channels == 0 || channels > MaxChannels || !valid_params(params))
            return false;

        sample_rate_ = sample_rate;
        channels_ = channels;
        params_ = sanitized(params);
        lookahead_samples_ =
            static_cast<int>(std::ceil(params_.lookahead_ms * 0.001 * sample_rate_));
        latency_samples_ = detector_latency_samples() + lookahead_samples_;
        delay_.assign((static_cast<std::size_t>(latency_samples_) + 1) * channels_, SampleType{0});
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            peak_values_[channel].assign(static_cast<std::size_t>(lookahead_samples_) + 2, 0.0);
            peak_indices_[channel].assign(static_cast<std::size_t>(lookahead_samples_) + 2, 0);
        }
        design_interpolator();
        update_control_coefficients();
        prepared_ = true;
        reset();
        return true;
    }

    void reset() noexcept {
        for (auto& history : histories_)
            history.fill(0.0);
        std::fill(delay_.begin(), delay_.end(), SampleType{0});
        history_write_ = 0;
        delay_write_frame_ = 0;
        frame_index_ = 0;
        fault_count_ = 0;
        for (std::size_t channel = 0; channel < MaxChannels; ++channel) {
            queue_head_[channel] = 0;
            queue_size_[channel] = 0;
            gain_[channel] = 1.0;
            gain_reduction_db_[channel] = 0.0;
        }
    }

    bool set_ceiling_dbtp(double dbtp) noexcept {
        if (!std::isfinite(dbtp))
            return false;
        const double clamped = std::clamp(dbtp, -24.0, 0.0);
        if (clamped == params_.ceiling_dbtp)
            return true;
        params_.ceiling_dbtp = clamped;
        update_control_coefficients();
        return true;
    }

    bool set_release_ms(double milliseconds) noexcept {
        if (!std::isfinite(milliseconds))
            return false;
        const double clamped = std::clamp(milliseconds, 5.0, 2000.0);
        if (clamped == params_.release_ms)
            return true;
        params_.release_ms = clamped;
        update_control_coefficients();
        return true;
    }

    double ceiling_dbtp() const noexcept {
        return params_.ceiling_dbtp;
    }
    double lookahead_ms() const noexcept {
        return params_.lookahead_ms;
    }
    double release_ms() const noexcept {
        return params_.release_ms;
    }
    ChannelLink channel_link() const noexcept {
        return params_.channel_link;
    }
    std::size_t channel_count() const noexcept {
        return prepared_ ? channels_ : 0;
    }
    int latency_samples() const noexcept {
        return prepared_ ? latency_samples_ : 0;
    }
    int tail_samples() const noexcept {
        return latency_samples();
    }
    bool prepared() const noexcept {
        return prepared_;
    }
    std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    /// Current attenuation as a non-negative dB magnitude.
    double gain_reduction_db(std::size_t channel = 0) const noexcept {
        if (!prepared_ || channel >= channels_)
            return 0.0;
        return gain_reduction_db_[params_.channel_link == ChannelLink::linked ? 0 : channel];
    }

    /// Reconstruct the four detector phases for one channel without changing
    /// limiter state. This exposes the detector stage to tests and visualizers.
    std::array<double, interpolation_factor()>
    detector_phases(std::span<const double, interpolation_taps()> newest_to_oldest) const noexcept {
        std::array<double, interpolation_factor()> result{};
        for (std::size_t phase = 0; phase < interpolation_factor(); ++phase)
            for (std::size_t tap = 0; tap < interpolation_taps(); ++tap)
                result[phase] += interpolation_[phase][tap] * newest_to_oldest[tap];
        return result;
    }

    bool process_frame(std::span<const SampleType> input, std::span<SampleType> output) noexcept {
        if (!prepared_ || input.size() != channels_ || output.size() != channels_)
            return false;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            if (!std::isfinite(static_cast<double>(input[channel]))) {
                ++fault_count_;
                reset_after_fault();
                std::fill(output.begin(), output.end(), SampleType{0});
                return false;
            }
        }

        std::array<double, MaxChannels> detected{};
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            histories_[channel][history_write_] = static_cast<double>(input[channel]);
            detected[channel] = interpolated_peak(channel);
        }
        if (++history_write_ == interpolation_taps())
            history_write_ = 0;

        if (params_.channel_link == ChannelLink::linked) {
            const double peak = *std::max_element(detected.begin(), detected.begin() + channels_);
            update_gain(0, peak);
            for (std::size_t channel = 1; channel < channels_; ++channel)
                gain_reduction_db_[channel] = gain_reduction_db_[0];
        } else {
            for (std::size_t channel = 0; channel < channels_; ++channel)
                update_gain(channel, detected[channel]);
        }

        const std::size_t delay_frames = static_cast<std::size_t>(latency_samples_) + 1;
        const std::size_t read_frame =
            (delay_write_frame_ + delay_frames - static_cast<std::size_t>(latency_samples_)) %
            delay_frames;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            delay_[delay_write_frame_ * channels_ + channel] = input[channel];
            const double gain = gain_[params_.channel_link == ChannelLink::linked ? 0 : channel];
            const double sample =
                static_cast<double>(delay_[read_frame * channels_ + channel]) * gain;
            output[channel] =
                std::isfinite(sample) ? static_cast<SampleType>(sample) : SampleType{0};
        }
        delay_write_frame_ = (delay_write_frame_ + 1) % delay_frames;
        ++frame_index_;
        return true;
    }

    bool process_interleaved(const SampleType* input, SampleType* output, std::size_t frames,
                             std::size_t channels) noexcept {
        if (!prepared_ || input == nullptr || output == nullptr || channels != channels_)
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::span<const SampleType> in(input + frame * channels_, channels_);
            const std::span<SampleType> out(output + frame * channels_, channels_);
            if (!process_frame(in, out))
                return false;
        }
        return true;
    }

  private:
    static constexpr double pi() noexcept {
        return 3.141592653589793238462643383279502884;
    }

    static bool valid_params(const Params& params) noexcept {
        return std::isfinite(params.ceiling_dbtp) && std::isfinite(params.lookahead_ms) &&
               std::isfinite(params.release_ms) && params.ceiling_dbtp >= -24.0 &&
               params.ceiling_dbtp <= 0.0 && params.lookahead_ms >= 0.0 &&
               params.lookahead_ms <= maximum_lookahead_ms() && params.release_ms >= 5.0 &&
               params.release_ms <= 2000.0;
    }

    static Params sanitized(Params params) noexcept {
        params.ceiling_dbtp = std::clamp(params.ceiling_dbtp, -24.0, 0.0);
        params.lookahead_ms = std::clamp(params.lookahead_ms, 0.0, maximum_lookahead_ms());
        params.release_ms = std::clamp(params.release_ms, 5.0, 2000.0);
        return params;
    }

    static double bessel_i0(double x) noexcept {
        double sum = 1.0;
        double term = 1.0;
        const double quarter = x * x * 0.25;
        for (int k = 1; k <= 24; ++k) {
            term *= quarter / static_cast<double>(k * k);
            sum += term;
        }
        return sum;
    }

    void design_interpolator() noexcept {
        constexpr double beta = 10.5;
        const double denominator = bessel_i0(beta);
        const double center = static_cast<double>(detector_latency_samples());
        for (std::size_t phase = 0; phase < interpolation_factor(); ++phase) {
            const double fraction =
                static_cast<double>(phase) / static_cast<double>(interpolation_factor());
            double sum = 0.0;
            for (std::size_t tap = 0; tap < interpolation_taps(); ++tap) {
                const double offset = static_cast<double>(tap) - center + fraction;
                const double sinc =
                    std::abs(offset) < 1.0e-15 ? 1.0 : std::sin(pi() * offset) / (pi() * offset);
                const double position = (static_cast<double>(tap) - center) / center;
                const double window =
                    bessel_i0(beta * std::sqrt(std::max(0.0, 1.0 - position * position))) /
                    denominator;
                interpolation_[phase][tap] = sinc * window;
                sum += interpolation_[phase][tap];
            }
            for (double& coefficient : interpolation_[phase])
                coefficient /= sum;
        }
    }

    double interpolated_peak(std::size_t channel) const noexcept {
        double peak = 0.0;
        for (std::size_t phase = 0; phase < interpolation_factor(); ++phase) {
            double value = 0.0;
            for (std::size_t tap = 0; tap < interpolation_taps(); ++tap) {
                const std::size_t index =
                    (history_write_ + interpolation_taps() - tap) % interpolation_taps();
                value += interpolation_[phase][tap] * histories_[channel][index];
            }
            peak = std::max(peak, std::abs(value));
        }
        return peak;
    }

    void update_gain(std::size_t lane, double peak) noexcept {
        auto& values = peak_values_[lane];
        auto& indices = peak_indices_[lane];
        const std::size_t capacity = values.size();
        while (queue_size_[lane] > 0) {
            const std::size_t back = (queue_head_[lane] + queue_size_[lane] - 1) % capacity;
            if (values[back] > peak)
                break;
            --queue_size_[lane];
        }
        const std::size_t write = (queue_head_[lane] + queue_size_[lane]) % capacity;
        values[write] = peak;
        indices[write] = frame_index_;
        ++queue_size_[lane];
        const std::uint64_t oldest =
            frame_index_ > static_cast<std::uint64_t>(lookahead_samples_)
                ? frame_index_ - static_cast<std::uint64_t>(lookahead_samples_)
                : 0;
        while (queue_size_[lane] > 0 && indices[queue_head_[lane]] < oldest) {
            queue_head_[lane] = (queue_head_[lane] + 1) % capacity;
            --queue_size_[lane];
        }

        const double future_peak = queue_size_[lane] == 0 ? 0.0 : values[queue_head_[lane]];
        const double required = future_peak > safe_ceiling_ ? safe_ceiling_ / future_peak : 1.0;
        if (required < gain_[lane]) {
            gain_[lane] = required;
        } else {
            gain_[lane] = required + release_retain_ * (gain_[lane] - required);
        }
        gain_[lane] = std::clamp(gain_[lane], std::numeric_limits<double>::min(), 1.0);
        gain_reduction_db_[lane] = -20.0 * std::log10(gain_[lane]);
    }

    void reset_after_fault() noexcept {
        const auto faults = fault_count_;
        reset();
        fault_count_ = faults;
    }

    void update_control_coefficients() noexcept {
        safe_ceiling_ = std::pow(10.0, (params_.ceiling_dbtp - detector_guard_db()) / 20.0);
        release_retain_ = dynamics::one_pole_retain(params_.release_ms * 0.001, sample_rate_);
    }

    Params params_{};
    double sample_rate_ = 0.0;
    double safe_ceiling_ = 1.0;
    double release_retain_ = 0.0;
    std::size_t channels_ = 0;
    int lookahead_samples_ = 0;
    int latency_samples_ = 0;
    bool prepared_ = false;
    std::uint64_t frame_index_ = 0;
    std::uint64_t fault_count_ = 0;
    std::size_t history_write_ = 0;
    std::size_t delay_write_frame_ = 0;
    std::array<std::array<double, interpolation_taps()>, interpolation_factor()> interpolation_{};
    std::array<std::array<double, interpolation_taps()>, MaxChannels> histories_{};
    std::vector<SampleType> delay_{};
    std::array<std::vector<double>, MaxChannels> peak_values_{};
    std::array<std::vector<std::uint64_t>, MaxChannels> peak_indices_{};
    std::array<std::size_t, MaxChannels> queue_head_{};
    std::array<std::size_t, MaxChannels> queue_size_{};
    std::array<double, MaxChannels> gain_{};
    std::array<double, MaxChannels> gain_reduction_db_{};
};

using TruePeakLimiter = TruePeakLimiterT<float>;
using TruePeakLimiter64 = TruePeakLimiterT<double>;

} // namespace pulp::signal
