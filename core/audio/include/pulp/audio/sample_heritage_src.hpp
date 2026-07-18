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

    void release() noexcept {
        std::vector<float>().swap(history_);
        selection_ = {};
        ratio_ = 0.0;
        step_fraction_ = 0.0;
        step_integer_ = 0;
        channels_ = 0;
        tap_count_ = 0;
        half_width_ = 0;
        prepared_ = false;
        identity_ = false;
        accepted_frames_ = 0;
        center_index_ = 0;
        center_fraction_ = 0.0;
        history_head_ = 0;
        history_count_ = 0;
        history_start_ = 0;
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
    }

    bool prepared() const noexcept { return prepared_; }
    bool identity() const noexcept { return identity_; }
    double ratio() const noexcept { return ratio_; }
    std::uint32_t half_width() const noexcept { return half_width_; }

    SampleHeritageSrcPlan plan(std::size_t output_frames) const noexcept {
        SampleHeritageSrcPlan result;
        result.output_frames = output_frames;
        if (!prepared_) return result;
        result.status = SampleHeritageSrcStatus::Ok;
        if (identity_) {
            result.input_frames = output_frames;
            return result;
        }
        auto center = center_index_;
        auto fraction = center_fraction_;
        std::int64_t highest = -1;
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            if (center > std::numeric_limits<std::int64_t>::max() -
                             static_cast<std::int64_t>(half_width_)) {
                result.status = SampleHeritageSrcStatus::SizeOverflow;
                return result;
            }
            highest = center + static_cast<std::int64_t>(half_width_);
            if (!advance(center, fraction)) {
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
        const auto expected = plan(output.num_samples());
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
                output.channel(channel)[frame] = selection_.apply(
                    std::span<const float>(taps.data(), tap_count_), fraction);
            }
            if (!advance(center, fraction))
                return SampleHeritageSrcStatus::SizeOverflow;
        }

        append_history(input);
        accepted_frames_ += input.num_samples();
        center_index_ = center;
        center_fraction_ = fraction;
        return SampleHeritageSrcStatus::Ok;
    }

private:
    bool advance(std::int64_t& center, double& fraction) const noexcept {
        auto next_fraction = fraction + step_fraction_;
        std::int64_t carry = 0;
        if (next_fraction >= 1.0) {
            next_fraction -= 1.0;
            carry = 1;
        }
        if (step_integer_ > std::numeric_limits<std::int64_t>::max() - carry ||
            center > std::numeric_limits<std::int64_t>::max() -
                         (step_integer_ + carry)) {
            return false;
        }
        center += step_integer_ + carry;
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
        const auto slot = (history_head_ + offset) % tap_count_;
        return history_[channel * tap_count_ + slot];
    }

    void append_history(const BufferView<const float>& input) noexcept {
        for (std::size_t frame = 0; frame < input.num_samples(); ++frame) {
            std::size_t slot = 0;
            if (history_count_ < tap_count_) {
                slot = (history_head_ + history_count_) % tap_count_;
                ++history_count_;
            } else {
                slot = history_head_;
                history_head_ = (history_head_ + 1) % tap_count_;
                ++history_start_;
            }
            for (std::size_t channel = 0; channel < channels_; ++channel)
                history_[channel * tap_count_ + slot] = input.channel(channel)[frame];
        }
    }

    std::vector<float> history_;
    SampleSincKernelSelection selection_{};
    double ratio_ = 0.0;
    double step_fraction_ = 0.0;
    std::int64_t step_integer_ = 0;
    std::size_t channels_ = 0;
    std::size_t tap_count_ = 0;
    std::uint32_t half_width_ = 0;
    bool prepared_ = false;
    bool identity_ = false;
    std::uint64_t accepted_frames_ = 0;
    std::int64_t center_index_ = 0;
    double center_fraction_ = 0.0;
    std::size_t history_head_ = 0;
    std::size_t history_count_ = 0;
    std::uint64_t history_start_ = 0;
};

}  // namespace pulp::audio
