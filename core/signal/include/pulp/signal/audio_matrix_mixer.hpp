#pragma once

/// @file audio_matrix_mixer.hpp
/// Fixed-capacity N-input to M-output audio/control matrix mixing.
///
/// A plan is prepared and validated on the control thread, then published as
/// one immutable value. The audio thread observes either the complete old plan
/// or the complete new plan at the start of a process call. Publication is
/// single-writer/single-reader; a published plan remains active until replaced.
///
/// Each cell is a signed linear gain. For output `o` and frame `f`, direct
/// summation is
///
///     y[o,f] = sum_i(x[i,f] * gain[o,i]).
///
/// `NormalizeAbsoluteSum` additionally multiplies each output by
/// `1 / max(1, sum_i(abs(gain[o,i])))`. This is a conservative headroom law:
/// when every input magnitude is at most A, the output magnitude is at most A.
/// It never boosts a row whose absolute gain sum is below unity. Neither law
/// `Direct` never clips or saturates. The normalized law clamps only floating-
/// point roundoff beyond the proven maximum input magnitude; neither law
/// sanitizes samples or applies smoothing.
///
/// RT contract: `process()` is fixed-capacity, allocation-free, lock-free, and
/// performs no I/O. It visits cells in increasing output/input order for every
/// frame, so an unchanged plan is deterministic and block-partition invariant.
/// Plan changes intentionally latch at process-call boundaries. Source and
/// destination frame regions must not overlap; invalid buffer sets fail before
/// any destination sample is written.

#include <pulp/runtime/result.hpp>
#include <pulp/runtime/triple_buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class AudioMatrixSummingLaw : std::uint8_t {
    Direct,
    NormalizeAbsoluteSum,
};

enum class AudioMatrixPlanError : std::uint8_t {
    NoInputs,
    NoOutputs,
    TooManyInputs,
    TooManyOutputs,
    WrongGainCount,
    NonfiniteGain,
    InvalidSummingLaw,
};

enum class AudioMatrixProcessStatus : std::uint8_t {
    Ok,
    Unprepared,
    InsufficientInputs,
    InsufficientOutputs,
    ShortInput,
    ShortOutput,
    OverlappingBuffers,
};

struct AudioMatrixProcessResult {
    AudioMatrixProcessStatus status = AudioMatrixProcessStatus::Unprepared;
    std::size_t frames_processed = 0;

    explicit operator bool() const noexcept {
        return status == AudioMatrixProcessStatus::Ok;
    }
};

template <typename SampleType = float,
          std::size_t MaxInputs = 16,
          std::size_t MaxOutputs = 16>
class AudioMatrixPlanT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxInputs > 0);
    static_assert(MaxOutputs > 0);

public:
    static constexpr std::size_t kMaxInputs = MaxInputs;
    static constexpr std::size_t kMaxOutputs = MaxOutputs;
    static constexpr std::size_t kMaxCells = MaxInputs * MaxOutputs;

    AudioMatrixPlanT() noexcept = default;

    /// Prepare a complete output-major matrix transactionally.
    ///
    /// `gains[o * input_count + i]` is the signed gain from input `i` to
    /// output `o`. Failure returns no partial plan.
    static runtime::Result<AudioMatrixPlanT, AudioMatrixPlanError>
    prepare(std::size_t input_count,
            std::size_t output_count,
            std::span<const SampleType> gains,
            AudioMatrixSummingLaw summing_law = AudioMatrixSummingLaw::Direct) noexcept {
        using Result = runtime::Result<AudioMatrixPlanT, AudioMatrixPlanError>;

        if (input_count == 0)
            return Result(runtime::Err(AudioMatrixPlanError::NoInputs));
        if (output_count == 0)
            return Result(runtime::Err(AudioMatrixPlanError::NoOutputs));
        if (input_count > MaxInputs)
            return Result(runtime::Err(AudioMatrixPlanError::TooManyInputs));
        if (output_count > MaxOutputs)
            return Result(runtime::Err(AudioMatrixPlanError::TooManyOutputs));
        if (gains.size() != input_count * output_count)
            return Result(runtime::Err(AudioMatrixPlanError::WrongGainCount));
        if (summing_law != AudioMatrixSummingLaw::Direct &&
            summing_law != AudioMatrixSummingLaw::NormalizeAbsoluteSum) {
            return Result(runtime::Err(AudioMatrixPlanError::InvalidSummingLaw));
        }

        AudioMatrixPlanT replacement;
        replacement.input_count_ = input_count;
        replacement.output_count_ = output_count;
        replacement.summing_law_ = summing_law;

        for (std::size_t output = 0; output < output_count; ++output) {
            long double maximum_gain = 0.0L;
            for (std::size_t input = 0; input < input_count; ++input) {
                const auto value = gains[output * input_count + input];
                if (!std::isfinite(value))
                    return Result(runtime::Err(AudioMatrixPlanError::NonfiniteGain));
                maximum_gain = std::max(
                    maximum_gain, std::abs(static_cast<long double>(value)));
            }

            long double scaled_sum = 0.0L;
            if (maximum_gain > 0.0L) {
                for (std::size_t input = 0; input < input_count; ++input) {
                    scaled_sum +=
                        std::abs(static_cast<long double>(
                            gains[output * input_count + input])) /
                        maximum_gain;
                }
            }
            const auto sample_max =
                static_cast<long double>(std::numeric_limits<SampleType>::max());
            replacement.absolute_gain_sums_[output] =
                maximum_gain > 0.0L && scaled_sum > sample_max / maximum_gain
                    ? std::numeric_limits<SampleType>::max()
                    : static_cast<SampleType>(maximum_gain * scaled_sum);

            const bool normalize =
                summing_law == AudioMatrixSummingLaw::NormalizeAbsoluteSum &&
                maximum_gain > 0.0L && maximum_gain > 1.0L / scaled_sum;
            for (std::size_t input = 0; input < input_count; ++input) {
                const auto value = gains[output * input_count + input];
                replacement.gains_[output * MaxInputs + input] = normalize
                    ? static_cast<SampleType>(
                          (static_cast<long double>(value) / maximum_gain) / scaled_sum)
                    : value;
            }
            if (normalize) {
                long double effective_sum = 0.0L;
                for (std::size_t input = 0; input < input_count; ++input) {
                    effective_sum += std::abs(static_cast<long double>(
                        replacement.gains_[output * MaxInputs + input]));
                }
                if (effective_sum > 1.0L) {
                    for (std::size_t input = 0; input < input_count; ++input) {
                        auto& gain = replacement.gains_[output * MaxInputs + input];
                        gain = std::nextafter(gain, SampleType{0});
                    }
                }
            }
        }

        return Result(runtime::Ok(replacement));
    }

    bool valid() const noexcept {
        return input_count_ > 0 && output_count_ > 0;
    }

    std::size_t input_count() const noexcept { return input_count_; }
    std::size_t output_count() const noexcept { return output_count_; }
    AudioMatrixSummingLaw summing_law() const noexcept { return summing_law_; }

    /// Effective coefficient used by process(). Under NormalizeAbsoluteSum it
    /// includes the prepared row scale; under Direct it is the authored gain.
    SampleType gain(std::size_t output, std::size_t input) const noexcept {
        if (output >= output_count_ || input >= input_count_)
            return SampleType{0};
        return gains_[output * MaxInputs + input];
    }

    SampleType absolute_gain_sum(std::size_t output) const noexcept {
        return output < output_count_ ? absolute_gain_sums_[output] : SampleType{0};
    }

private:
    template <typename, std::size_t, std::size_t>
    friend class AudioMatrixMixerT;

    std::array<SampleType, kMaxCells> gains_{};
    std::array<SampleType, MaxOutputs> absolute_gain_sums_{};
    std::size_t input_count_ = 0;
    std::size_t output_count_ = 0;
    AudioMatrixSummingLaw summing_law_ = AudioMatrixSummingLaw::Direct;
};

template <typename SampleType = float,
          std::size_t MaxInputs = 16,
          std::size_t MaxOutputs = 16>
class AudioMatrixMixerT {
    static_assert(std::is_floating_point_v<SampleType>);

public:
    using Plan = AudioMatrixPlanT<SampleType, MaxInputs, MaxOutputs>;
    using ConstBuffer = std::span<const SampleType>;
    using MutableBuffer = std::span<SampleType>;

    AudioMatrixMixerT() noexcept = default;

    explicit AudioMatrixMixerT(const Plan& initial_plan) noexcept
        : plans_(initial_plan) {}

    /// Publish a complete prepared plan from the single control-thread writer.
    /// Invalid/default plans are rejected without changing the active plan.
    bool publish(const Plan& plan) noexcept {
        if (!plan.valid())
            return false;
        plans_.write(plan);
        return true;
    }

    /// Process one block using the plan latched at call entry.
    ///
    /// Inputs and outputs may contain extra buffers; the active plan consumes
    /// only its declared counts. Every consumed span must contain `frames`
    /// samples. All consumed destination regions must be disjoint from each
    /// other and from every consumed source region.
    AudioMatrixProcessResult
    process(std::span<const ConstBuffer> inputs,
            std::span<MutableBuffer> outputs,
            std::size_t frames) noexcept {
        const Plan& plan = plans_.read();
        if (!plan.valid())
            return {AudioMatrixProcessStatus::Unprepared, 0};
        if (inputs.size() < plan.input_count_)
            return {AudioMatrixProcessStatus::InsufficientInputs, 0};
        if (outputs.size() < plan.output_count_)
            return {AudioMatrixProcessStatus::InsufficientOutputs, 0};
        if (frames == 0)
            return {AudioMatrixProcessStatus::Ok, 0};

        for (std::size_t input = 0; input < plan.input_count_; ++input) {
            if (inputs[input].size() < frames)
                return {AudioMatrixProcessStatus::ShortInput, 0};
        }
        for (std::size_t output = 0; output < plan.output_count_; ++output) {
            if (outputs[output].size() < frames)
                return {AudioMatrixProcessStatus::ShortOutput, 0};
        }
        if (buffers_overlap_(inputs, outputs, plan, frames))
            return {AudioMatrixProcessStatus::OverlappingBuffers, 0};

        for (std::size_t frame = 0; frame < frames; ++frame) {
            for (std::size_t output = 0; output < plan.output_count_; ++output) {
                const std::size_t row = output * MaxInputs;
                if (plan.summing_law_ == AudioMatrixSummingLaw::NormalizeAbsoluteSum) {
                    long double sum = 0.0L;
                    long double magnitude_bound = 0.0L;
                    for (std::size_t input = 0; input < plan.input_count_; ++input) {
                        const auto sample = static_cast<long double>(inputs[input][frame]);
                        magnitude_bound = std::max(magnitude_bound, std::abs(sample));
                        sum += sample *
                               static_cast<long double>(plan.gains_[row + input]);
                    }
                    sum = std::clamp(sum, -magnitude_bound, magnitude_bound);
                    outputs[output][frame] = static_cast<SampleType>(sum);
                } else {
                    SampleType sum = SampleType{0};
                    for (std::size_t input = 0; input < plan.input_count_; ++input)
                        sum += inputs[input][frame] * plan.gains_[row + input];
                    outputs[output][frame] = sum;
                }
            }
        }
        return {AudioMatrixProcessStatus::Ok, frames};
    }

private:
    struct AddressRange {
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
    };

    template <typename Span>
    static AddressRange range_(Span buffer, std::size_t frames) noexcept {
        const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
        return {begin, begin + frames * sizeof(SampleType)};
    }

    static bool overlaps_(AddressRange lhs, AddressRange rhs) noexcept {
        return lhs.begin < rhs.end && rhs.begin < lhs.end;
    }

    static bool buffers_overlap_(std::span<const ConstBuffer> inputs,
                                 std::span<MutableBuffer> outputs,
                                 const Plan& plan,
                                 std::size_t frames) noexcept {
        for (std::size_t output = 0; output < plan.output_count_; ++output) {
            const auto output_range = range_(outputs[output], frames);
            for (std::size_t input = 0; input < plan.input_count_; ++input) {
                if (overlaps_(output_range, range_(inputs[input], frames)))
                    return true;
            }
            for (std::size_t previous = 0; previous < output; ++previous) {
                if (overlaps_(output_range, range_(outputs[previous], frames)))
                    return true;
            }
        }
        return false;
    }

    runtime::TripleBuffer<Plan> plans_{};
};

using AudioMatrixPlan = AudioMatrixPlanT<float, 16, 16>;
using AudioMatrixPlan64 = AudioMatrixPlanT<double, 16, 16>;
using AudioMatrixMixer = AudioMatrixMixerT<float, 16, 16>;
using AudioMatrixMixer64 = AudioMatrixMixerT<double, 16, 16>;

}  // namespace pulp::signal
