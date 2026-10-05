#pragma once

/// @file tempo_delay_wrapper.hpp
/// Prepared stereo tempo delay with bounded feedback and click-free retiming.

#include <pulp/signal/fractional_delay.hpp>
#include <pulp/signal/tempo_delay.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::signal {

enum class TempoDelayWrapperInterpolation : std::uint8_t {
    lagrange3,
    lagrange5,
    thiran1,
};

enum class TempoDelayWrapperError : std::uint8_t {
    none,
    not_prepared,
    invalid_sample_rate,
    invalid_capacity,
    unsupported_interpolation,
    invalid_delay,
    invalid_tempo,
    invalid_division,
    out_of_range,
    invalid_feedback,
    invalid_crossfeed,
    non_finite_input,
};

struct TempoDelayWrapperProcessResult {
    TempoDelayWrapperError error = TempoDelayWrapperError::none;
    std::size_t processed_frames = 0;
    std::size_t fault_count = 0;

    constexpr explicit operator bool() const noexcept {
        return error == TempoDelayWrapperError::none;
    }
};

/// A wet-only, stereo delay that owns its histories and feedback routes.
///
/// Preparation is the only allocating operation. The audio path is bounded,
/// noexcept, and partition invariant: each sample reads the old history,
/// crossfades fixed old/new read heads during a retime, then commits the input
/// plus the bounded feedback route. Tempo and division updates are transactional
/// and can be published by a control owner before the next audio block.
///
/// Delay changes use two fixed read heads and an amplitude crossfade. They never
/// sweep one read head through a changing delay, so a tempo move cannot become a
/// pitch glide. A delay has zero direct-path latency. With feedback disabled its
/// tail is one configured delay; with feedback enabled it is mathematically
/// unbounded and `tail_samples()` returns -1.
class TempoDelayWrapper {
  public:
    static constexpr double kMaximumFeedback = 0.98;
    static constexpr std::size_t kMinimumCapacitySamples = 3;

    TempoDelayWrapper() = default;
    TempoDelayWrapper(const TempoDelayWrapper&) = delete;
    TempoDelayWrapper& operator=(const TempoDelayWrapper&) = delete;

    [[nodiscard]] TempoDelayWrapperError prepare(
        double sample_rate, std::size_t maximum_delay_samples,
        TempoDelayWrapperInterpolation interpolation = TempoDelayWrapperInterpolation::lagrange3,
        std::uint32_t transition_samples = 64) {
        if (!std::isfinite(sample_rate) || !(sample_rate > 0.0) ||
            sample_rate > kMaximumTempoDelaySampleRate)
            return TempoDelayWrapperError::invalid_sample_rate;
        if (maximum_delay_samples < kMinimumCapacitySamples)
            return TempoDelayWrapperError::invalid_capacity;
        if (!is_supported(interpolation))
            return TempoDelayWrapperError::unsupported_interpolation;

        FractionalDelayHistoryT<float> left;
        FractionalDelayHistoryT<float> right;
        if (!left.prepare(maximum_delay_samples) || !right.prepare(maximum_delay_samples))
            return TempoDelayWrapperError::invalid_capacity;

        left_history_ = std::move(left);
        right_history_ = std::move(right);
        sample_rate_ = sample_rate;
        maximum_delay_samples_ = maximum_delay_samples;
        interpolation_ = interpolation;
        transition_samples_ = transition_samples;
        current_left_delay_samples_ = 2.0;
        current_right_delay_samples_ = 2.0;
        target_left_delay_samples_ = 2.0;
        target_right_delay_samples_ = 2.0;
        transition_remaining_ = 0;
        initialized_ = false;
        feedback_ = 0.0;
        crossfeed_ = 0.0;
        prepared_ = true;
        return TempoDelayWrapperError::none;
    }

    [[nodiscard]] TempoDelayWrapperError set_delay_samples(double samples) noexcept {
        if (!prepared_)
            return TempoDelayWrapperError::not_prepared;
        if (!std::isfinite(samples))
            return TempoDelayWrapperError::invalid_delay;
        const auto minimum = static_cast<double>(minimum_delay_samples(interpolation_));
        if (samples < minimum)
            return TempoDelayWrapperError::invalid_delay;
        if (samples > static_cast<double>(maximum_delay_samples_))
            return TempoDelayWrapperError::out_of_range;
        adopt_target(samples, samples);
        return TempoDelayWrapperError::none;
    }

    [[nodiscard]] TempoDelayWrapperError set_delay_samples(double left_samples,
                                                           double right_samples) noexcept {
        if (!prepared_)
            return TempoDelayWrapperError::not_prepared;
        const auto left_status = validate_delay(left_samples);
        const auto right_status = validate_delay(right_samples);
        if (left_status != TempoDelayWrapperError::none)
            return left_status;
        if (right_status != TempoDelayWrapperError::none)
            return right_status;
        adopt_target(left_samples, right_samples);
        return TempoDelayWrapperError::none;
    }

    [[nodiscard]] TempoDelayWrapperError set_tempo(timebase::BeatDivision division,
                                                   double beats_per_minute) noexcept {
        if (!prepared_)
            return TempoDelayWrapperError::not_prepared;
        const auto result = tempo_delay_samples(division, beats_per_minute, sample_rate_);
        if (!result) {
            if (result.error == TempoDelayError::invalid_tempo)
                return TempoDelayWrapperError::invalid_tempo;
            if (result.error == TempoDelayError::invalid_division)
                return TempoDelayWrapperError::invalid_division;
            if (result.error == TempoDelayError::out_of_range)
                return TempoDelayWrapperError::out_of_range;
            return TempoDelayWrapperError::invalid_sample_rate;
        }
        return set_delay_samples(result.samples);
    }

    [[nodiscard]] TempoDelayWrapperError set_feedback(double feedback) noexcept {
        if (!std::isfinite(feedback) || feedback < 0.0 || feedback > kMaximumFeedback)
            return TempoDelayWrapperError::invalid_feedback;
        feedback_ = feedback;
        return TempoDelayWrapperError::none;
    }

    [[nodiscard]] TempoDelayWrapperError set_crossfeed(double crossfeed) noexcept {
        if (!std::isfinite(crossfeed) || crossfeed < 0.0 || crossfeed > 1.0)
            return TempoDelayWrapperError::invalid_crossfeed;
        crossfeed_ = crossfeed;
        return TempoDelayWrapperError::none;
    }

    [[nodiscard]] TempoDelayWrapperProcessResult process(float* left, float* right,
                                                         std::size_t frame_count) noexcept {
        TempoDelayWrapperProcessResult result;
        if (!prepared_) {
            result.error = TempoDelayWrapperError::not_prepared;
            return result;
        }
        if (frame_count == 0)
            return result;
        if (left == nullptr || right == nullptr) {
            result.error = TempoDelayWrapperError::invalid_capacity;
            return result;
        }

        for (std::size_t index = 0; index < frame_count; ++index) {
            const auto left_read = read(left_history_, current_left_delay_samples_);
            const auto right_read = read(right_history_, current_right_delay_samples_);
            if (!left_read || !right_read) {
                result.error = left_read.status == FractionalDelayStatus::invalid_argument ||
                                       right_read.status == FractionalDelayStatus::invalid_argument
                                   ? TempoDelayWrapperError::unsupported_interpolation
                                   : TempoDelayWrapperError::invalid_delay;
                return result;
            }

            float wet_left = left_read.sample;
            float wet_right = right_read.sample;
            if (transition_remaining_ != 0) {
                const auto next_left = read(left_history_, target_left_delay_samples_);
                const auto next_right = read(right_history_, target_right_delay_samples_);
                if (!next_left || !next_right) {
                    result.error = TempoDelayWrapperError::invalid_delay;
                    return result;
                }
                const auto elapsed = transition_samples_ - transition_remaining_ + 1U;
                const auto amount =
                    transition_samples_ == 0
                        ? 1.0f
                        : static_cast<float>(elapsed) / static_cast<float>(transition_samples_);
                wet_left += amount * (next_left.sample - wet_left);
                wet_right += amount * (next_right.sample - wet_right);
                --transition_remaining_;
                if (transition_remaining_ == 0) {
                    current_left_delay_samples_ = target_left_delay_samples_;
                    current_right_delay_samples_ = target_right_delay_samples_;
                }
            }

            const float input_left = left[index];
            const float input_right = right[index];
            if (!std::isfinite(input_left) || !std::isfinite(input_right)) {
                ++result.fault_count;
                if (result.error == TempoDelayWrapperError::none)
                    result.error = TempoDelayWrapperError::non_finite_input;
            }
            const float safe_left = std::isfinite(input_left) ? input_left : 0.0f;
            const float safe_right = std::isfinite(input_right) ? input_right : 0.0f;
            const float feedback_left =
                feedback_ * ((1.0f - crossfeed_) * wet_left + crossfeed_ * wet_right);
            const float feedback_right =
                feedback_ * ((1.0f - crossfeed_) * wet_right + crossfeed_ * wet_left);
            left[index] = wet_left;
            right[index] = wet_right;
            const auto left_status = left_history_.push(safe_left + feedback_left);
            const auto right_status = right_history_.push(safe_right + feedback_right);
            if (left_status != FractionalDelayStatus::ok ||
                right_status != FractionalDelayStatus::ok)
                ++result.fault_count;
            ++result.processed_frames;
        }
        return result;
    }

    void reset() noexcept {
        if (!prepared_)
            return;
        left_history_.reset();
        right_history_.reset();
        current_left_delay_samples_ = initialized_ ? target_left_delay_samples_ : 2.0;
        current_right_delay_samples_ = initialized_ ? target_right_delay_samples_ : 2.0;
        transition_remaining_ = 0;
    }

    [[nodiscard]] bool prepared() const noexcept {
        return prepared_;
    }
    [[nodiscard]] bool initialized() const noexcept {
        return initialized_;
    }
    [[nodiscard]] double sample_rate() const noexcept {
        return prepared_ ? sample_rate_ : 0.0;
    }
    [[nodiscard]] std::size_t maximum_delay_samples() const noexcept {
        return prepared_ ? maximum_delay_samples_ : 0;
    }
    [[nodiscard]] double current_delay_samples() const noexcept {
        return prepared_ ? current_left_delay_samples_ : 0.0;
    }
    [[nodiscard]] double target_delay_samples() const noexcept {
        return prepared_ ? target_left_delay_samples_ : 0.0;
    }
    [[nodiscard]] double current_right_delay_samples() const noexcept {
        return prepared_ ? current_right_delay_samples_ : 0.0;
    }
    [[nodiscard]] double target_right_delay_samples() const noexcept {
        return prepared_ ? target_right_delay_samples_ : 0.0;
    }
    [[nodiscard]] bool transition_active() const noexcept {
        return transition_remaining_ != 0;
    }
    [[nodiscard]] double feedback() const noexcept {
        return feedback_;
    }
    [[nodiscard]] double crossfeed() const noexcept {
        return crossfeed_;
    }
    [[nodiscard]] TempoDelayWrapperInterpolation interpolation() const noexcept {
        return interpolation_;
    }
    [[nodiscard]] static constexpr int latency_samples() noexcept {
        return 0;
    }
    [[nodiscard]] int tail_samples() const noexcept {
        return feedback_ > 0.0 ? -1
                               : static_cast<int>(std::max(target_left_delay_samples_,
                                                           target_right_delay_samples_));
    }

  private:
    static constexpr bool is_supported(TempoDelayWrapperInterpolation interpolation) noexcept {
        return interpolation == TempoDelayWrapperInterpolation::lagrange3 ||
               interpolation == TempoDelayWrapperInterpolation::lagrange5;
    }

    static constexpr std::size_t
    minimum_delay_samples(TempoDelayWrapperInterpolation interpolation) noexcept {
        return interpolation == TempoDelayWrapperInterpolation::lagrange5 ? 3u : 2u;
    }

    [[nodiscard]] FractionalDelaySampleResult<float>
    read(const FractionalDelayHistoryT<float>& history, double delay) const noexcept {
        return interpolation_ == TempoDelayWrapperInterpolation::lagrange5
                   ? history.read_lagrange5_at(delay)
                   : history.read_lagrange3_at(delay);
    }

    [[nodiscard]] TempoDelayWrapperError validate_delay(double samples) const noexcept {
        if (!std::isfinite(samples))
            return TempoDelayWrapperError::invalid_delay;
        if (samples < static_cast<double>(minimum_delay_samples(interpolation_)))
            return TempoDelayWrapperError::invalid_delay;
        if (samples > static_cast<double>(maximum_delay_samples_))
            return TempoDelayWrapperError::out_of_range;
        return TempoDelayWrapperError::none;
    }

    void adopt_target(double left_samples, double right_samples) noexcept {
        if (!initialized_) {
            initialized_ = true;
            current_left_delay_samples_ = left_samples;
            current_right_delay_samples_ = right_samples;
            target_left_delay_samples_ = left_samples;
            target_right_delay_samples_ = right_samples;
            transition_remaining_ = 0;
            return;
        }
        if (left_samples == target_left_delay_samples_ &&
            right_samples == target_right_delay_samples_)
            return;
        target_left_delay_samples_ = left_samples;
        target_right_delay_samples_ = right_samples;
        if (transition_samples_ == 0) {
            current_left_delay_samples_ = left_samples;
            current_right_delay_samples_ = right_samples;
            transition_remaining_ = 0;
            return;
        }
        transition_remaining_ = transition_samples_;
    }

    FractionalDelayHistoryT<float> left_history_;
    FractionalDelayHistoryT<float> right_history_;
    double sample_rate_ = 0.0;
    std::size_t maximum_delay_samples_ = 0;
    double current_left_delay_samples_ = 2.0;
    double current_right_delay_samples_ = 2.0;
    double target_left_delay_samples_ = 2.0;
    double target_right_delay_samples_ = 2.0;
    double feedback_ = 0.0;
    double crossfeed_ = 0.0;
    std::uint32_t transition_samples_ = 64;
    std::uint32_t transition_remaining_ = 0;
    TempoDelayWrapperInterpolation interpolation_ = TempoDelayWrapperInterpolation::lagrange3;
    bool prepared_ = false;
    bool initialized_ = false;
};

} // namespace pulp::signal
