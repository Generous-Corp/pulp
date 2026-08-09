#pragma once

/// @file headphone_crossfeed.hpp
/// Bounded speaker-like crossfeed for stereo headphone playback.

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <type_traits>

namespace pulp::signal {

/// Mixes a delayed, low-pass-filtered copy of each channel into the opposite
/// ear. The topology is a stable feed-forward path: there is no feedback and
/// the one-pole filter coefficient is always in `[0, 1)`.
///
/// `prepare()` and setters belong on the control thread. `process_sample()`,
/// `process_block()`, and `reset()` are bounded, lock-free, and allocation-free.
template <typename SampleType = float> class HeadphoneCrossfeedT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr double kMinimumSampleRate = 1000.0;
    static constexpr double kMaximumSampleRate = 768000.0;
    static constexpr double kMaximumDelayMs = 1.0;
    static constexpr double kMaximumCrossfeedGain = 0.35;

    HeadphoneCrossfeedT() noexcept {
        update_derived_parameters();
    }

    bool prepare(double sample_rate) noexcept {
        if (!std::isfinite(sample_rate) || sample_rate < kMinimumSampleRate ||
            sample_rate > kMaximumSampleRate)
            return false;
        sample_rate_ = sample_rate;
        update_derived_parameters();
        reset();
        return true;
    }

    double sample_rate() const noexcept {
        return sample_rate_;
    }

    void set_enabled(bool enabled) noexcept {
        enabled_ = enabled;
    }
    bool enabled() const noexcept {
        return enabled_;
    }

    /// Normalized crossfeed amount. At one, the opposite-channel feed is
    /// `kMaximumCrossfeedGain` before unity-at-DC normalization.
    void set_amount(double amount01) noexcept {
        if (!std::isfinite(amount01))
            return;
        amount_ = std::clamp(amount01, 0.0, 1.0);
        update_mix();
    }
    double amount() const noexcept {
        return amount_;
    }
    double crossfeed_gain() const noexcept {
        return amount_ * kMaximumCrossfeedGain;
    }

    /// Interaural delay in milliseconds, clamped to `[0, 1]`.
    void set_delay_ms(double delay_ms) noexcept {
        if (!std::isfinite(delay_ms))
            return;
        delay_ms_ = std::clamp(delay_ms, 0.0, kMaximumDelayMs);
        update_delay_samples();
    }
    double delay_ms() const noexcept {
        return delay_ms_;
    }
    double delay_samples() const noexcept {
        return delay_samples_;
    }

    /// Crossfeed low-pass cutoff. The requested value is clamped to
    /// `[100, 20000]` Hz and the effective value to 45% of sample rate.
    void set_cutoff_hz(double cutoff_hz) noexcept {
        if (!std::isfinite(cutoff_hz))
            return;
        requested_cutoff_hz_ = std::clamp(cutoff_hz, 100.0, 20000.0);
        update_filter();
    }
    double cutoff_hz() const noexcept {
        return cutoff_hz_;
    }

    /// Exact zero algorithmic latency: only the opposite-channel contribution
    /// is delayed; the direct paths are not delayed.
    static constexpr int latency_samples() noexcept {
        return 0;
    }

    /// The one-pole feed decays asymptotically whenever crossfeed is active.
    int tail_samples() const noexcept {
        return enabled_ && amount_ > 0.0 ? -1 : 0;
    }

    void reset() noexcept {
        left_delay_.fill(SampleType{0});
        right_delay_.fill(SampleType{0});
        write_position_ = 0;
        valid_samples_ = 0;
        filtered_left_ = SampleType{0};
        filtered_right_ = SampleType{0};
    }

    void process_sample(SampleType input_left, SampleType input_right, SampleType& output_left,
                        SampleType& output_right) noexcept {
        if (!std::isfinite(static_cast<double>(input_left)) ||
            !std::isfinite(static_cast<double>(input_right))) {
            reset();
            output_left = SampleType{0};
            output_right = SampleType{0};
            return;
        }

        left_delay_[write_position_] = input_left;
        right_delay_[write_position_] = input_right;
        write_position_ = (write_position_ + 1) % kDelayCapacity;
        valid_samples_ = std::min(valid_samples_ + 1, kDelayCapacity);

        const SampleType delayed_left = read_delay(left_delay_);
        const SampleType delayed_right = read_delay(right_delay_);
        filtered_left_ = snap_to_zero(filter_coefficient_ * filtered_left_ +
                                      filter_input_coefficient_ * delayed_left);
        filtered_right_ = snap_to_zero(filter_coefficient_ * filtered_right_ +
                                       filter_input_coefficient_ * delayed_right);

        if (!enabled_ || amount_ == 0.0) {
            output_left = input_left;
            output_right = input_right;
            return;
        }

        output_left = direct_weight_ * input_left + cross_weight_ * filtered_right_;
        output_right = direct_weight_ * input_right + cross_weight_ * filtered_left_;
        if (!std::isfinite(static_cast<double>(output_left)) ||
            !std::isfinite(static_cast<double>(output_right))) {
            reset();
            output_left = SampleType{0};
            output_right = SampleType{0};
        }
    }

    void process_block(const SampleType* input_left, const SampleType* input_right,
                       SampleType* output_left, SampleType* output_right,
                       std::size_t sample_count) noexcept {
        if (input_left == nullptr || input_right == nullptr || output_left == nullptr ||
            output_right == nullptr)
            return;
        for (std::size_t i = 0; i < sample_count; ++i) {
            const SampleType left = input_left[i];
            const SampleType right = input_right[i];
            process_sample(left, right, output_left[i], output_right[i]);
        }
    }

  private:
    static constexpr std::size_t kDelayCapacity =
        static_cast<std::size_t>(kMaximumSampleRate * kMaximumDelayMs / 1000.0) + 2;
    using DelayBuffer = std::array<SampleType, kDelayCapacity>;

    void update_derived_parameters() noexcept {
        update_delay_samples();
        update_filter();
        update_mix();
    }

    void update_delay_samples() noexcept {
        delay_samples_ = std::clamp(delay_ms_ * sample_rate_ / 1000.0, 0.0,
                                    static_cast<double>(kDelayCapacity - 2));
    }

    void update_filter() noexcept {
        cutoff_hz_ = std::min(requested_cutoff_hz_, 0.45 * sample_rate_);
        filter_coefficient_ = static_cast<SampleType>(
            std::exp(-2.0 * std::numbers::pi * cutoff_hz_ / sample_rate_));
        filter_input_coefficient_ = SampleType{1} - filter_coefficient_;
    }

    void update_mix() noexcept {
        const double gain = crossfeed_gain();
        const double denominator = 1.0 + gain;
        direct_weight_ = static_cast<SampleType>(1.0 / denominator);
        cross_weight_ = static_cast<SampleType>(gain / denominator);
    }

    SampleType read_integer(const DelayBuffer& buffer, std::size_t delay) const noexcept {
        if (delay >= valid_samples_)
            return SampleType{0};
        const std::size_t index =
            (write_position_ + kDelayCapacity - 1 - delay) % kDelayCapacity;
        return buffer[index];
    }

    SampleType read_delay(const DelayBuffer& buffer) const noexcept {
        const auto younger = static_cast<std::size_t>(std::floor(delay_samples_));
        const SampleType fraction =
            static_cast<SampleType>(delay_samples_ - static_cast<double>(younger));
        const SampleType a = read_integer(buffer, younger);
        const SampleType b = read_integer(buffer, younger + 1);
        return (SampleType{1} - fraction) * a + fraction * b;
    }

    double sample_rate_ = 48000.0;
    double amount_ = 0.5;
    double delay_ms_ = 0.25;
    double delay_samples_ = 12.0;
    double requested_cutoff_hz_ = 700.0;
    double cutoff_hz_ = 700.0;
    bool enabled_ = true;

    SampleType filter_coefficient_{};
    SampleType filter_input_coefficient_{};
    SampleType direct_weight_{};
    SampleType cross_weight_{};
    DelayBuffer left_delay_{};
    DelayBuffer right_delay_{};
    std::size_t write_position_ = 0;
    std::size_t valid_samples_ = 0;
    SampleType filtered_left_{};
    SampleType filtered_right_{};
};

using HeadphoneCrossfeed = HeadphoneCrossfeedT<float>;
using HeadphoneCrossfeed64 = HeadphoneCrossfeedT<double>;

} // namespace pulp::signal
