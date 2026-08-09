#pragma once

/// @file cross_feedback_multitap_delay.hpp
/// Bounded stereo multitap delay with a provably stable cross-feedback law.

#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pulp::signal {

/// A wet-only stereo multitap delay with normalized tap feedback.
///
/// Tap configuration and `prepare()` belong on the control thread. After
/// preparation, processing and reset are bounded, lock-free, and allocation-free.
/// The feedback matrix has absolute row sum `abs(feedback_gain())`, which is
/// clamped below one. Normalized feedback-tap weights and linear interpolation
/// keep the complete recursive path's infinity norm below one as well.
template <typename SampleType = float> class CrossFeedbackMultitapDelayT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr std::size_t kMaxTaps = 8;
    static constexpr double kMinimumSampleRate = 1000.0;
    static constexpr double kMaximumSampleRate = 768000.0;
    static constexpr double kMaximumDelayMs = 10000.0;
    static constexpr double kMaximumFeedbackGain = 0.95;

    struct Tap {
        double delay_ms = 250.0;
        double level = 1.0;
        double pan = 0.0;
        double stereo_width = 1.0;
        double feedback_weight = 1.0;
    };

    CrossFeedbackMultitapDelayT() noexcept {
        update_all_taps();
    }

    [[nodiscard]] bool prepare(double sample_rate, double maximum_delay_ms = 2000.0) {
        if (!std::isfinite(sample_rate) || sample_rate < kMinimumSampleRate ||
            sample_rate > kMaximumSampleRate || !std::isfinite(maximum_delay_ms) ||
            maximum_delay_ms < 1000.0 / sample_rate ||
            maximum_delay_ms > kMaximumDelayMs)
            return false;

        const double required = std::ceil(sample_rate * maximum_delay_ms / 1000.0) + 1.0;
        if (required > static_cast<double>(std::numeric_limits<int>::max() - 1))
            return false;
        const int maximum_delay_samples = static_cast<int>(required);

        DelayLineT<SampleType> replacement_left;
        DelayLineT<SampleType> replacement_right;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#endif
            replacement_left.prepare(maximum_delay_samples);
            replacement_right.prepare(maximum_delay_samples);
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#endif

        left_history_ = std::move(replacement_left);
        right_history_ = std::move(replacement_right);
        sample_rate_ = sample_rate;
        maximum_delay_ms_ = maximum_delay_ms;
        prepared_ = true;
        update_all_taps();
        reset();
        return true;
    }

    bool prepared() const noexcept {
        return prepared_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }
    double maximum_delay_ms() const noexcept {
        return maximum_delay_ms_;
    }
    std::size_t retained_bytes() const noexcept {
        if (!prepared_)
            return 0;
        const auto samples = static_cast<std::size_t>(left_history_.max_delay() + 1);
        return 2u * samples * sizeof(SampleType);
    }

    void set_active_tap_count(std::size_t count) noexcept {
        active_tap_count_ = std::min(count, kMaxTaps);
        update_feedback_weights();
    }
    std::size_t active_tap_count() const noexcept {
        return active_tap_count_;
    }

    [[nodiscard]] bool set_tap(std::size_t index, Tap tap) noexcept {
        if (index >= kMaxTaps || !std::isfinite(tap.delay_ms) ||
            !std::isfinite(tap.level) || !std::isfinite(tap.pan) ||
            !std::isfinite(tap.stereo_width) || !std::isfinite(tap.feedback_weight))
            return false;
        if (prepared_)
            tap.delay_ms = std::clamp(tap.delay_ms, minimum_delay_ms(), maximum_delay_ms_);
        else
            tap.delay_ms = std::clamp(tap.delay_ms, 1000.0 / kMaximumSampleRate,
                                      kMaximumDelayMs);
        tap.level = std::clamp(tap.level, -1.0, 1.0);
        tap.pan = std::clamp(tap.pan, -1.0, 1.0);
        tap.stereo_width = std::clamp(tap.stereo_width, 0.0, 1.0);
        tap.feedback_weight = std::clamp(tap.feedback_weight, -1.0, 1.0);
        taps_[index] = tap;
        if (prepared_)
            update_tap(index);
        update_feedback_weights();
        return true;
    }

    Tap tap(std::size_t index) const noexcept {
        return index < kMaxTaps ? taps_[index] : Tap{};
    }

    void set_feedback_gain(double gain) noexcept {
        if (std::isfinite(gain))
            feedback_gain_ = std::clamp(gain, -kMaximumFeedbackGain, kMaximumFeedbackGain);
    }
    double feedback_gain() const noexcept {
        return feedback_gain_;
    }

    /// Zero is independent same-channel feedback; one is pure cross-channel
    /// feedback. Intermediate values form a convex stereo feedback matrix.
    void set_cross_feedback(double amount01) noexcept {
        if (std::isfinite(amount01))
            cross_feedback_ = std::clamp(amount01, 0.0, 1.0);
    }
    double cross_feedback() const noexcept {
        return cross_feedback_;
    }

    double worst_case_feedback_gain() const noexcept {
        return std::abs(feedback_gain_);
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }

    int tail_samples() const noexcept {
        if (!prepared_ || active_tap_count_ == 0)
            return 0;
        bool has_feedback_route = false;
        bool has_audible_route = false;
        double latest_delay = 0.0;
        for (std::size_t i = 0; i < active_tap_count_; ++i) {
            has_feedback_route |= derived_[i].normalized_feedback_weight != 0.0;
            if (taps_[i].level != 0.0) {
                has_audible_route = true;
                latest_delay = std::max(latest_delay, derived_[i].delay_samples);
            }
        }
        if (!has_audible_route)
            return 0;
        if (feedback_gain_ != 0.0 && has_feedback_route)
            return -1;
        return static_cast<int>(std::ceil(latest_delay));
    }

    void reset() noexcept {
        left_history_.discard_history();
        right_history_.discard_history();
    }

    void process_sample(SampleType input_left, SampleType input_right, SampleType& output_left,
                        SampleType& output_right) noexcept {
        output_left = SampleType{};
        output_right = SampleType{};
        if (!prepared_)
            return;
        if (!std::isfinite(static_cast<double>(input_left)) ||
            !std::isfinite(static_cast<double>(input_right))) {
            reset();
            return;
        }

        long double wet_left = 0.0L;
        long double wet_right = 0.0L;
        long double feedback_left = 0.0L;
        long double feedback_right = 0.0L;

        for (std::size_t i = 0; i < active_tap_count_; ++i) {
            const auto& tap = taps_[i];
            const auto& derived = derived_[i];
            const auto history_delay = static_cast<SampleType>(derived.delay_samples - 1.0);
            const SampleType delayed_left = left_history_.read(history_delay);
            const SampleType delayed_right = right_history_.read(history_delay);

            wet_left += static_cast<long double>(tap.level) *
                        (derived.left_to_left * delayed_left +
                         derived.right_to_left * delayed_right);
            wet_right += static_cast<long double>(tap.level) *
                         (derived.left_to_right * delayed_left +
                          derived.right_to_right * delayed_right);
            feedback_left += derived.normalized_feedback_weight * delayed_left;
            feedback_right += derived.normalized_feedback_weight * delayed_right;
        }

        const long double self = 1.0L - static_cast<long double>(cross_feedback_);
        const long double cross = static_cast<long double>(cross_feedback_);
        const long double gain = static_cast<long double>(feedback_gain_);
        const long double write_left =
            static_cast<long double>(input_left) + gain * (self * feedback_left + cross * feedback_right);
        const long double write_right =
            static_cast<long double>(input_right) + gain * (self * feedback_right + cross * feedback_left);
        const long double limit = static_cast<long double>(std::numeric_limits<SampleType>::max());

        if (!std::isfinite(wet_left) || !std::isfinite(wet_right) ||
            !std::isfinite(write_left) || !std::isfinite(write_right) ||
            std::abs(wet_left) > limit || std::abs(wet_right) > limit ||
            std::abs(write_left) > limit || std::abs(write_right) > limit) {
            reset();
            return;
        }

        output_left = snap_to_zero(static_cast<SampleType>(wet_left));
        output_right = snap_to_zero(static_cast<SampleType>(wet_right));
        left_history_.push(snap_to_zero(static_cast<SampleType>(write_left)));
        right_history_.push(snap_to_zero(static_cast<SampleType>(write_right)));
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
    struct DerivedTap {
        double delay_samples = 12000.0;
        double left_to_left = 1.0;
        double left_to_right = 0.0;
        double right_to_left = 0.0;
        double right_to_right = 1.0;
        double normalized_feedback_weight = 1.0;
    };

    double minimum_delay_ms() const noexcept {
        return 1000.0 / sample_rate_;
    }

    void update_tap(std::size_t index) noexcept {
        auto& tap = taps_[index];
        auto& derived = derived_[index];
        tap.delay_ms = std::clamp(tap.delay_ms, minimum_delay_ms(), maximum_delay_ms_);
        // A sample-exact millisecond value can reconstruct a few ULPs to either
        // side of its integer. Canonicalize only that representational fringe
        // so delay indexing and tail metadata agree at exact-sample settings.
        const double reconstructed = tap.delay_ms * sample_rate_ / 1000.0;
        const double nearest_integer = std::round(reconstructed);
        const double integer_tolerance =
            8.0 * std::numeric_limits<double>::epsilon() *
            std::max(1.0, std::abs(reconstructed));
        const double canonical =
            std::abs(reconstructed - nearest_integer) <= integer_tolerance
                ? nearest_integer
                : reconstructed;
        derived.delay_samples = std::max(1.0, canonical);

        const double left_position = std::clamp(tap.pan - tap.stereo_width, -1.0, 1.0);
        const double right_position = std::clamp(tap.pan + tap.stereo_width, -1.0, 1.0);
        const auto gains = [](double position) {
            const double angle = (position + 1.0) * std::numbers::pi / 4.0;
            return std::array<double, 2>{std::cos(angle), std::sin(angle)};
        };
        const auto left_gains = gains(left_position);
        const auto right_gains = gains(right_position);
        derived.left_to_left = left_gains[0];
        derived.left_to_right = left_gains[1];
        derived.right_to_left = right_gains[0];
        derived.right_to_right = right_gains[1];
    }

    void update_feedback_weights() noexcept {
        double sum = 0.0;
        for (std::size_t i = 0; i < active_tap_count_; ++i)
            sum += std::abs(taps_[i].feedback_weight);
        const double denominator = std::max(1.0, sum);
        for (std::size_t i = 0; i < kMaxTaps; ++i)
            derived_[i].normalized_feedback_weight =
                i < active_tap_count_ ? taps_[i].feedback_weight / denominator : 0.0;
    }

    void update_all_taps() noexcept {
        for (std::size_t i = 0; i < kMaxTaps; ++i)
            update_tap(i);
        update_feedback_weights();
    }

    double sample_rate_ = 48000.0;
    double maximum_delay_ms_ = 2000.0;
    double feedback_gain_ = 0.0;
    double cross_feedback_ = 1.0;
    std::size_t active_tap_count_ = 1;
    bool prepared_ = false;
    std::array<Tap, kMaxTaps> taps_{};
    std::array<DerivedTap, kMaxTaps> derived_{};
    DelayLineT<SampleType> left_history_{};
    DelayLineT<SampleType> right_history_{};
};

using CrossFeedbackMultitapDelay = CrossFeedbackMultitapDelayT<float>;
using CrossFeedbackMultitapDelay64 = CrossFeedbackMultitapDelayT<double>;

} // namespace pulp::signal
