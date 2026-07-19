#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>
#include <pulp/audio/sample_sinc_kernel.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace pulp::audio {

enum class SampleHeritageSrcStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidConfiguration,
    SizeOverflow,
    AllocationFailed,
    InvalidDimensions,
    InputFrameMismatch,
    HistoryUnavailable,
};

struct SampleHeritageSrcPlan {
    SampleHeritageSrcStatus status = SampleHeritageSrcStatus::NotPrepared;
    std::size_t output_frames = 0;
    std::size_t input_frames = 0;

    bool valid() const noexcept { return status == SampleHeritageSrcStatus::Ok; }
};

/// Bounded causal streaming sample-rate converter for the heritage pipeline.
///
/// The immutable production sampler sinc bank supplies the coefficients. The
/// converter delays its interpolation centre by one half-width, zero-extends
/// only the prehistory before source frame zero, and retains exactly one tap
/// neighbourhood per channel between calls. plan() is observational; process()
/// advances only after every dimension and exact-input check succeeds.
class SampleHeritageCausalSrc {
public:
    SampleHeritageCausalSrc() = default;
    SampleHeritageCausalSrc(const SampleHeritageCausalSrc&) = delete;
    SampleHeritageCausalSrc& operator=(const SampleHeritageCausalSrc&) = delete;
    SampleHeritageCausalSrc(SampleHeritageCausalSrc&&) = delete;
    SampleHeritageCausalSrc& operator=(SampleHeritageCausalSrc&&) = delete;

    SampleHeritageSrcStatus prepare(double source_frames_per_output,
                                    std::size_t channel_count,
                                    SampleSincKernelSelection selection,
                                    bool identity) noexcept {
        release();
        if (!(source_frames_per_output > 0.0) ||
            !std::isfinite(source_frames_per_output) || channel_count == 0 ||
            channel_count > kSampleHeritageMaximumChannels ||
            (!identity && !selection.valid())) {
            return SampleHeritageSrcStatus::InvalidConfiguration;
        }
        identity_ = identity;
        ratio_ = source_frames_per_output;
        channels_ = channel_count;
        selection_ = selection;
        if (!identity_) {
            tap_count_ = selection.wider.tap_count();
            history_capacity_ = tap_count_;
            half_width_ = selection.wider.half_width();
            if (tap_count_ == 0 ||
                channels_ > std::numeric_limits<std::size_t>::max() / tap_count_) {
                release();
                return SampleHeritageSrcStatus::SizeOverflow;
            }
            try {
                history_.assign(channels_ * tap_count_, 0.0f);
            } catch (...) {
                release();
                return SampleHeritageSrcStatus::AllocationFailed;
            }
            const auto integer = std::floor(ratio_);
            if (integer > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                release();
                return SampleHeritageSrcStatus::InvalidConfiguration;
            }
            step_integer_ = static_cast<std::int64_t>(integer);
            step_fraction_ = ratio_ - integer;
        }
        prepared_ = true;
        reset();
        return SampleHeritageSrcStatus::Ok;
    }

    SampleHeritageSrcStatus prepare_variable(
        double default_source_frames_per_output,
        double minimum_source_frames_per_output,
        double maximum_source_frames_per_output,
        std::size_t channel_count,
        SampleSincKernelBankView bank) noexcept {
        if (!bank.valid() || !(minimum_source_frames_per_output > 0.0) ||
            !std::isfinite(minimum_source_frames_per_output) ||
            minimum_source_frames_per_output > default_source_frames_per_output ||
            !(maximum_source_frames_per_output > 0.0) ||
            !std::isfinite(maximum_source_frames_per_output) ||
            default_source_frames_per_output > maximum_source_frames_per_output ||
            !bank.select(maximum_source_frames_per_output).valid())
            return SampleHeritageSrcStatus::InvalidConfiguration;
        const auto selection = bank.select(default_source_frames_per_output);
        const auto status = prepare(default_source_frames_per_output, channel_count,
                                    selection, false);
        if (status != SampleHeritageSrcStatus::Ok) return status;
        variable_bank_ = bank;
        maximum_ratio_ = maximum_source_frames_per_output;
        minimum_ratio_ = minimum_source_frames_per_output;
        fixed_latency_output_frames_ = std::ceil(
            static_cast<double>(half_width_) / minimum_source_frames_per_output);
        const auto history_frames = std::ceil(
            fixed_latency_output_frames_ * maximum_source_frames_per_output) +
            static_cast<double>(tap_count_) + 2.0;
        if (!std::isfinite(history_frames) ||
            history_frames >
                static_cast<double>(std::numeric_limits<std::size_t>::max())) {
            release();
            return SampleHeritageSrcStatus::SizeOverflow;
        }
        history_capacity_ = static_cast<std::size_t>(history_frames);
        if (channels_ > std::numeric_limits<std::size_t>::max() /
                            history_capacity_) {
            release();
            return SampleHeritageSrcStatus::SizeOverflow;
        }
        try {
            history_.assign(channels_ * history_capacity_, 0.0f);
        } catch (...) {
            release();
            return SampleHeritageSrcStatus::AllocationFailed;
        }
        variable_ratio_ = true;
        return SampleHeritageSrcStatus::Ok;
    }

    void release() noexcept {
        std::vector<float>().swap(history_);
        selection_ = {};
        variable_bank_ = {};
        ratio_ = 0.0;
        maximum_ratio_ = 0.0;
        minimum_ratio_ = 0.0;
        fixed_latency_output_frames_ = 0.0;
        step_fraction_ = 0.0;
        step_integer_ = 0;
        channels_ = 0;
        tap_count_ = 0;
        history_capacity_ = 0;
        half_width_ = 0;
        prepared_ = false;
        identity_ = false;
        variable_ratio_ = false;
        started_ = false;
        accepted_frames_ = 0;
        center_index_ = 0;
        center_fraction_ = 0.0;
        history_head_ = 0;
        history_count_ = 0;
        history_start_ = 0;
        acquisition_index_ = 0;
        acquisition_fraction_ = 0.0;
        active_ratio_ = 0.0;
    }

    void reset() noexcept {
        if (!prepared_) return;
        std::fill(history_.begin(), history_.end(), 0.0f);
        accepted_frames_ = 0;
        center_index_ = identity_ ? 0 : -static_cast<std::int64_t>(half_width_);
        center_fraction_ = 0.0;
        history_head_ = 0;
        history_count_ = 0;
        history_start_ = 0;
        started_ = false;
        acquisition_index_ = 0;
        acquisition_fraction_ = 0.0;
        active_ratio_ = 0.0;
    }

    bool prepared() const noexcept { return prepared_; }
    bool identity() const noexcept { return identity_; }
    double ratio() const noexcept { return ratio_; }
    std::uint32_t half_width() const noexcept { return half_width_; }

    SampleHeritageSrcPlan plan(std::size_t output_frames) const noexcept {
        return plan(output_frames, ratio_);
    }

    SampleHeritageSrcPlan plan(std::size_t output_frames,
                               double source_frames_per_output) const noexcept {
        SampleHeritageSrcPlan result;
        result.output_frames = output_frames;
        if (!prepared_) return result;
        if (!(source_frames_per_output > 0.0) ||
            !std::isfinite(source_frames_per_output) ||
            (!variable_ratio_ && source_frames_per_output != ratio_) ||
            (variable_ratio_ && source_frames_per_output > maximum_ratio_)) {
            result.status = SampleHeritageSrcStatus::InvalidConfiguration;
            return result;
        }
        result.status = SampleHeritageSrcStatus::Ok;
        if (identity_) {
            result.input_frames = output_frames;
            return result;
        }
        if (variable_ratio_) {
            auto center = center_index_;
            auto fraction = center_fraction_;
            if (!adjust_position_for_ratio(source_frames_per_output, center,
                                           fraction)) {
                result.status = SampleHeritageSrcStatus::SizeOverflow;
                return result;
            }
            auto acquisition = acquisition_index_;
            auto acquisition_fraction = acquisition_fraction_;
            for (std::size_t frame = 0; frame < output_frames; ++frame) {
                if (!advance(acquisition, acquisition_fraction,
                             source_frames_per_output)) {
                    result.status = SampleHeritageSrcStatus::SizeOverflow;
                    return result;
                }
            }
            auto required_through = static_cast<std::uint64_t>(acquisition);
            if (acquisition_fraction > 0.0) ++required_through;
            if (required_through > accepted_frames_) {
                const auto needed = required_through - accepted_frames_;
                if (needed > std::numeric_limits<std::size_t>::max()) {
                    result.status = SampleHeritageSrcStatus::SizeOverflow;
                    return result;
                }
                result.input_frames = static_cast<std::size_t>(needed);
            }
            return result;
        }
        auto center = center_index_;
        auto fraction = center_fraction_;
        initial_position(source_frames_per_output, center, fraction);
        if (!adjust_position_for_ratio(source_frames_per_output, center,
                                       fraction)) {
            result.status = SampleHeritageSrcStatus::SizeOverflow;
            return result;
        }
        std::int64_t highest = -1;
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            if (center > std::numeric_limits<std::int64_t>::max() -
                             static_cast<std::int64_t>(half_width_)) {
                result.status = SampleHeritageSrcStatus::SizeOverflow;
                return result;
            }
            highest = center + static_cast<std::int64_t>(half_width_);
            if (!advance(center, fraction, source_frames_per_output)) {
                result.status = SampleHeritageSrcStatus::SizeOverflow;
                return result;
            }
        }
        if (highest < 0) return result;
        const auto required_through = static_cast<std::uint64_t>(highest) + 1u;
        if (required_through > accepted_frames_) {
            const auto needed = required_through - accepted_frames_;
            if (needed > std::numeric_limits<std::size_t>::max()) {
                result.status = SampleHeritageSrcStatus::SizeOverflow;
                return result;
            }
            result.input_frames = static_cast<std::size_t>(needed);
        }
        return result;
    }

    SampleHeritageSrcStatus process(const BufferView<const float>& input,
                                    BufferView<float> output) noexcept {
        return process(input, output, ratio_);
    }

    SampleHeritageSrcStatus process(const BufferView<const float>& input,
                                    BufferView<float> output,
                                    double source_frames_per_output) noexcept {
        const auto expected = plan(output.num_samples(), source_frames_per_output);
        if (!expected.valid()) return expected.status;
        if (input.num_channels() != channels_ ||
            output.num_channels() != channels_) {
            return SampleHeritageSrcStatus::InvalidDimensions;
        }
        if (input.num_samples() != expected.input_frames)
            return SampleHeritageSrcStatus::InputFrameMismatch;

        if (identity_) {
            for (std::size_t channel = 0; channel < channels_; ++channel)
                std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                          output.channel(channel).begin());
            return SampleHeritageSrcStatus::Ok;
        }
        if (input.num_samples() >
            std::numeric_limits<std::uint64_t>::max() - accepted_frames_) {
            return SampleHeritageSrcStatus::SizeOverflow;
        }

        std::array<float, 128> taps{};
        auto center = center_index_;
        auto fraction = center_fraction_;
        initial_position(source_frames_per_output, center, fraction);
        if (!adjust_position_for_ratio(source_frames_per_output, center,
                                       fraction))
            return SampleHeritageSrcStatus::SizeOverflow;
        const auto new_input_start = accepted_frames_;
        if (output.num_samples() != 0) {
            const auto first_required = center +
                static_cast<std::int64_t>(selection_.wider.first_offset());
            if (first_required >= 0 &&
                static_cast<std::uint64_t>(first_required) < new_input_start &&
                static_cast<std::uint64_t>(first_required) < history_start_) {
                return SampleHeritageSrcStatus::HistoryUnavailable;
            }
        }
        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                for (std::size_t tap = 0; tap < tap_count_; ++tap) {
                    const auto source_index = center +
                        static_cast<std::int64_t>(selection_.wider.first_offset()) +
                        static_cast<std::int64_t>(tap);
                    taps[tap] = source_sample(channel, source_index, input,
                                              new_input_start);
                }
                const auto selection = variable_ratio_
                    ? variable_bank_.select(source_frames_per_output)
                    : selection_;
                if (!selection.valid())
                    return SampleHeritageSrcStatus::InvalidConfiguration;
                output.channel(channel)[frame] = selection.apply(
                    std::span<const float>(taps.data(), tap_count_), fraction);
            }
            if (!advance(center, fraction, source_frames_per_output))
                return SampleHeritageSrcStatus::SizeOverflow;
        }

        append_history(input);
        accepted_frames_ += input.num_samples();
        center_index_ = center;
        center_fraction_ = fraction;
        if (variable_ratio_) {
            for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
                if (!advance(acquisition_index_, acquisition_fraction_,
                             source_frames_per_output))
                    return SampleHeritageSrcStatus::SizeOverflow;
            }
        }
        started_ = true;
        active_ratio_ = source_frames_per_output;
        return SampleHeritageSrcStatus::Ok;
    }

private:
    void initial_position(double ratio, std::int64_t& center,
                          double& fraction) const noexcept {
        if (!variable_ratio_ || started_) return;
        const auto position = -fixed_latency_output_frames_ * ratio;
        const auto integer = std::floor(position);
        center = static_cast<std::int64_t>(integer);
        fraction = position - integer;
    }

    bool adjust_position_for_ratio(double ratio, std::int64_t& center,
                                   double& fraction) const noexcept {
        if (!variable_ratio_ || !started_ || ratio == active_ratio_) return true;
        const auto position = static_cast<long double>(acquisition_index_) +
            acquisition_fraction_ -
            static_cast<long double>(fixed_latency_output_frames_) * ratio;
        const auto integer = std::floor(position);
        if (integer < static_cast<long double>(
                          std::numeric_limits<std::int64_t>::min()) ||
            integer > static_cast<long double>(
                          std::numeric_limits<std::int64_t>::max()))
            return false;
        center = static_cast<std::int64_t>(integer);
        fraction = static_cast<double>(position - integer);
        return true;
    }

    bool advance(std::int64_t& center, double& fraction,
                 double ratio) const noexcept {
        const auto integer_part = std::floor(ratio);
        if (integer_part >
            static_cast<double>(std::numeric_limits<std::int64_t>::max()))
            return false;
        const auto step_integer = static_cast<std::int64_t>(integer_part);
        const auto step_fraction = ratio - integer_part;
        auto next_fraction = fraction + step_fraction;
        std::int64_t carry = 0;
        if (next_fraction >= 1.0) {
            next_fraction -= 1.0;
            carry = 1;
        }
        if (step_integer > std::numeric_limits<std::int64_t>::max() - carry ||
            center > std::numeric_limits<std::int64_t>::max() -
                         (step_integer + carry)) {
            return false;
        }
        center += step_integer + carry;
        fraction = next_fraction;
        return true;
    }

    float source_sample(std::size_t channel,
                        std::int64_t source_index,
                        const BufferView<const float>& input,
                        std::uint64_t new_input_start) const noexcept {
        if (source_index < 0) return 0.0f;
        const auto absolute = static_cast<std::uint64_t>(source_index);
        if (absolute >= new_input_start) {
            const auto offset = absolute - new_input_start;
            if (offset < input.num_samples())
                return input.channel(channel)[static_cast<std::size_t>(offset)];
            return 0.0f;
        }
        if (absolute < history_start_ ||
            absolute - history_start_ >= history_count_) {
            return 0.0f;
        }
        const auto offset = static_cast<std::size_t>(absolute - history_start_);
        const auto slot = (history_head_ + offset) % history_capacity_;
        return history_[channel * history_capacity_ + slot];
    }

    void append_history(const BufferView<const float>& input) noexcept {
        for (std::size_t frame = 0; frame < input.num_samples(); ++frame) {
            std::size_t slot = 0;
            if (history_count_ < history_capacity_) {
                slot = (history_head_ + history_count_) % history_capacity_;
                ++history_count_;
            } else {
                slot = history_head_;
                history_head_ = (history_head_ + 1) % history_capacity_;
                ++history_start_;
            }
            for (std::size_t channel = 0; channel < channels_; ++channel)
                history_[channel * history_capacity_ + slot] =
                    input.channel(channel)[frame];
        }
    }

    std::vector<float> history_;
    SampleSincKernelSelection selection_{};
    SampleSincKernelBankView variable_bank_{};
    double ratio_ = 0.0;
    double maximum_ratio_ = 0.0;
    double minimum_ratio_ = 0.0;
    double fixed_latency_output_frames_ = 0.0;
    double step_fraction_ = 0.0;
    std::int64_t step_integer_ = 0;
    std::size_t channels_ = 0;
    std::size_t tap_count_ = 0;
    std::size_t history_capacity_ = 0;
    std::uint32_t half_width_ = 0;
    bool prepared_ = false;
    bool identity_ = false;
    bool variable_ratio_ = false;
    bool started_ = false;
    std::uint64_t accepted_frames_ = 0;
    std::int64_t center_index_ = 0;
    double center_fraction_ = 0.0;
    std::size_t history_head_ = 0;
    std::size_t history_count_ = 0;
    std::uint64_t history_start_ = 0;
    std::int64_t acquisition_index_ = 0;
    double acquisition_fraction_ = 0.0;
    double active_ratio_ = 0.0;
};

}  // namespace pulp::audio
