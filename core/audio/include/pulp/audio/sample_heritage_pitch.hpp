#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_heritage_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace pulp::audio {

enum class SampleHeritagePitchStatus : std::uint8_t {
    Ok,
    NotPrepared,
    InvalidConfiguration,
    SizeOverflow,
    AllocationFailed,
    InvalidDimensions,
    InputFrameMismatch,
    HistoryUnavailable,
};

struct SampleHeritagePitchPlan {
    SampleHeritagePitchStatus status = SampleHeritagePitchStatus::NotPrepared;
    std::size_t output_frames = 0;
    std::size_t input_frames = 0;

    bool valid() const noexcept { return status == SampleHeritagePitchStatus::Ok; }
};

/// Streaming pitch-family mechanism for one sampler voice.
///
/// Variable-clock playback changes the rate of the machine clock and therefore
/// copies machine-domain samples without interpolation. Drop/repeat and early
/// linear playback keep the machine clock fixed and move through the source at
/// the requested factor using zero-order selection or linear interpolation.
/// The profile clock ratio remains an independent multiplier in either route;
/// factor is the note/source pitch factor supplied by the sampler at runtime.
/// prepare() allocates the bounded two-frame history used by fixed-clock modes;
/// plan() and process() do not allocate.
class SampleHeritagePitchProcessor {
public:
    SampleHeritagePitchProcessor() = default;
    SampleHeritagePitchProcessor(const SampleHeritagePitchProcessor&) = delete;
    SampleHeritagePitchProcessor& operator=(const SampleHeritagePitchProcessor&) = delete;
    SampleHeritagePitchProcessor(SampleHeritagePitchProcessor&&) = delete;
    SampleHeritagePitchProcessor& operator=(SampleHeritagePitchProcessor&&) = delete;

    SampleHeritagePitchStatus prepare(SampleHeritagePitchFamily family,
                                      double factor,
                                      std::size_t channel_count) noexcept {
        release();
        if (!valid_family(family) || !std::isfinite(factor) ||
            factor < kMinimumFactor || factor > kMaximumFactor ||
            channel_count == 0 || channel_count > kSampleHeritageMaximumChannels) {
            return SampleHeritagePitchStatus::InvalidConfiguration;
        }

        family_ = family;
        factor_ = factor;
        channels_ = channel_count;
        exact_bypass_ = factor == 1.0;
        fixed_clock_ = family != SampleHeritagePitchFamily::VariableClock;

        if (fixed_clock_) {
            try {
                history_.assign(channels_ * kHistoryFrames, 0.0f);
            } catch (...) {
                release();
                return SampleHeritagePitchStatus::AllocationFailed;
            }
            set_step(factor_);
        }

        prepared_ = true;
        reset();
        return SampleHeritagePitchStatus::Ok;
    }

    void release() noexcept {
        std::vector<float>().swap(history_);
        family_ = SampleHeritagePitchFamily::VariableClock;
        factor_ = 0.0;
        step_fraction_ = 0.0;
        step_integer_ = 0;
        channels_ = 0;
        prepared_ = false;
        exact_bypass_ = false;
        fixed_clock_ = false;
        accepted_frames_ = 0;
        source_index_ = 0;
        source_fraction_ = 0.0;
        history_count_ = 0;
        history_start_ = 0;
    }

    void reset() noexcept {
        if (!prepared_) return;
        std::fill(history_.begin(), history_.end(), 0.0f);
        accepted_frames_ = 0;
        source_index_ = 0;
        source_fraction_ = 0.0;
        history_count_ = 0;
        history_start_ = 0;
    }

    bool prepared() const noexcept { return prepared_; }
    bool exact_bypass() const noexcept { return exact_bypass_; }
    bool fixed_clock_processing() const noexcept {
        return fixed_clock_ && !exact_bypass_;
    }
    SampleHeritagePitchFamily family() const noexcept { return family_; }
    double factor() const noexcept { return factor_; }

    /// Updates note pitch without reallocating or discontinuously resetting the
    /// source cursor. A fresh/reset factor-one fixed-clock processor takes the
    /// exact copy path; after another factor has displaced the cursor, factor
    /// one continues through the indexed path until reset.
    SampleHeritagePitchStatus set_factor(double factor) noexcept {
        if (!prepared_) return SampleHeritagePitchStatus::NotPrepared;
        if (!std::isfinite(factor) || factor < kMinimumFactor ||
            factor > kMaximumFactor) {
            return SampleHeritagePitchStatus::InvalidConfiguration;
        }
        factor_ = factor;
        if (fixed_clock_) set_step(factor_);
        exact_bypass_ = factor_ == 1.0 &&
            (!fixed_clock_ ||
             (source_index_ >= 0 && source_fraction_ == 0.0 &&
              static_cast<std::uint64_t>(source_index_) == accepted_frames_));
        return SampleHeritagePitchStatus::Ok;
    }

    /// Multiplier applied to the voice machine clock.
    double machine_clock_multiplier() const noexcept {
        return prepared_ && family_ == SampleHeritagePitchFamily::VariableClock
            ? factor_
            : 1.0;
    }

    /// Source advance per fixed-rate machine frame.
    double source_frames_per_machine_frame() const noexcept {
        return prepared_ && family_ != SampleHeritagePitchFamily::VariableClock
            ? factor_
            : 1.0;
    }

    SampleHeritagePitchPlan plan(std::size_t output_frames) const noexcept {
        SampleHeritagePitchPlan result;
        result.output_frames = output_frames;
        if (!prepared_) return result;
        result.status = SampleHeritagePitchStatus::Ok;
        if (!fixed_clock_processing()) {
            result.input_frames = output_frames;
            return result;
        }

        auto index = source_index_;
        auto fraction = source_fraction_;
        std::int64_t highest = -1;
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            auto required = index;
            if (family_ == SampleHeritagePitchFamily::EarlyLinear && fraction > 0.0) {
                if (required == std::numeric_limits<std::int64_t>::max()) {
                    result.status = SampleHeritagePitchStatus::SizeOverflow;
                    return result;
                }
                ++required;
            }
            highest = std::max(highest, required);
            if (!advance(index, fraction)) {
                result.status = SampleHeritagePitchStatus::SizeOverflow;
                return result;
            }
        }
        if (highest < 0) return result;
        const auto required_through = static_cast<std::uint64_t>(highest) + 1u;
        if (required_through > accepted_frames_) {
            const auto needed = required_through - accepted_frames_;
            if (needed > std::numeric_limits<std::size_t>::max()) {
                result.status = SampleHeritagePitchStatus::SizeOverflow;
                return result;
            }
            result.input_frames = static_cast<std::size_t>(needed);
        }
        return result;
    }

    SampleHeritagePitchStatus process(const BufferView<const float>& input,
                                      BufferView<float> output) noexcept {
        const auto expected = plan(output.num_samples());
        if (!expected.valid()) return expected.status;
        if (input.num_channels() != channels_ || output.num_channels() != channels_)
            return SampleHeritagePitchStatus::InvalidDimensions;
        if (input.num_samples() != expected.input_frames)
            return SampleHeritagePitchStatus::InputFrameMismatch;

        if (!fixed_clock_processing()) {
            if (fixed_clock_ &&
                (input.num_samples() >
                     std::numeric_limits<std::uint64_t>::max() - accepted_frames_ ||
                 input.num_samples() > static_cast<std::size_t>(
                     std::numeric_limits<std::int64_t>::max() - source_index_))) {
                return SampleHeritagePitchStatus::SizeOverflow;
            }
            for (std::size_t channel = 0; channel < channels_; ++channel)
                std::copy(input.channel(channel).begin(), input.channel(channel).end(),
                          output.channel(channel).begin());
            if (fixed_clock_) {
                append_history(input);
                accepted_frames_ += input.num_samples();
                source_index_ += static_cast<std::int64_t>(input.num_samples());
            }
            return SampleHeritagePitchStatus::Ok;
        }
        if (input.num_samples() >
            std::numeric_limits<std::uint64_t>::max() - accepted_frames_) {
            return SampleHeritagePitchStatus::SizeOverflow;
        }

        const auto new_input_start = accepted_frames_;
        auto index = source_index_;
        auto fraction = source_fraction_;
        if (output.num_samples() != 0 && index >= 0 &&
            static_cast<std::uint64_t>(index) < new_input_start &&
            static_cast<std::uint64_t>(index) < history_start_) {
            return SampleHeritagePitchStatus::HistoryUnavailable;
        }

        for (std::size_t frame = 0; frame < output.num_samples(); ++frame) {
            for (std::size_t channel = 0; channel < channels_; ++channel) {
                const auto first = source_sample(channel, index, input, new_input_start);
                if (family_ == SampleHeritagePitchFamily::EarlyLinear &&
                    fraction > 0.0) {
                    const auto second = source_sample(channel, index + 1, input,
                                                      new_input_start);
                    output.channel(channel)[frame] = static_cast<float>(
                        first + (second - first) * fraction);
                } else {
                    output.channel(channel)[frame] = first;
                }
            }
            if (!advance(index, fraction))
                return SampleHeritagePitchStatus::SizeOverflow;
        }

        append_history(input);
        accepted_frames_ += input.num_samples();
        source_index_ = index;
        source_fraction_ = fraction;
        return SampleHeritagePitchStatus::Ok;
    }

    static constexpr double kMinimumFactor = 0.015625;
    static constexpr double kMaximumFactor = 64.0;

private:
    static constexpr std::size_t kHistoryFrames = 2;

    static bool valid_family(SampleHeritagePitchFamily family) noexcept {
        return family == SampleHeritagePitchFamily::VariableClock ||
               family == SampleHeritagePitchFamily::DropRepeat ||
               family == SampleHeritagePitchFamily::EarlyLinear;
    }

    void set_step(double factor) noexcept {
        const auto integer = std::floor(factor);
        step_integer_ = static_cast<std::int64_t>(integer);
        step_fraction_ = factor - integer;
    }

    bool advance(std::int64_t& index, double& fraction) const noexcept {
        auto next_fraction = fraction + step_fraction_;
        std::int64_t carry = 0;
        if (next_fraction >= 1.0) {
            next_fraction -= 1.0;
            carry = 1;
        }
        if (step_integer_ > std::numeric_limits<std::int64_t>::max() - carry ||
            index > std::numeric_limits<std::int64_t>::max() -
                        (step_integer_ + carry)) {
            return false;
        }
        index += step_integer_ + carry;
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
        const auto slot = static_cast<std::size_t>(absolute - history_start_);
        return history_[channel * kHistoryFrames + slot];
    }

    void append_history(const BufferView<const float>& input) noexcept {
        const auto retained = std::min(kHistoryFrames, input.num_samples());
        const auto previous_needed = kHistoryFrames - retained;
        const auto previous_retained = std::min(previous_needed, history_count_);
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            auto* destination = history_.data() + channel * kHistoryFrames;
            if (previous_retained != 0) {
                const auto previous_offset = history_count_ - previous_retained;
                std::move(destination + previous_offset,
                          destination + history_count_, destination);
            }
            const auto source_offset = input.num_samples() - retained;
            std::copy(input.channel(channel).begin() +
                          static_cast<std::ptrdiff_t>(source_offset),
                      input.channel(channel).end(),
                      destination + previous_retained);
        }
        history_count_ = previous_retained + retained;
        history_start_ = accepted_frames_ + input.num_samples() - history_count_;
    }

    std::vector<float> history_;
    SampleHeritagePitchFamily family_ = SampleHeritagePitchFamily::VariableClock;
    double factor_ = 0.0;
    double step_fraction_ = 0.0;
    std::int64_t step_integer_ = 0;
    std::size_t channels_ = 0;
    bool prepared_ = false;
    bool exact_bypass_ = false;
    bool fixed_clock_ = false;
    std::uint64_t accepted_frames_ = 0;
    std::int64_t source_index_ = 0;
    double source_fraction_ = 0.0;
    std::size_t history_count_ = 0;
    std::uint64_t history_start_ = 0;
};

}  // namespace pulp::audio
