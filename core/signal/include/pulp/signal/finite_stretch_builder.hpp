#pragma once

/// @file finite_stretch_builder.hpp
/// Resumable, allocation-free-after-prepare driver for a bounded finite
/// RealtimePitchTimeProcessor time-stretch stream.

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::signal {

enum class FiniteStretchPrepareStatus {
    prepared,
    invalid_mode,
    invalid_input,
    invalid_output,
    invalid_output_layout,
    invalid_target,
    invalid_ratio,
    unrepresentable_capacity,
    processor_prepare_failed,
};

enum class FiniteStretchStepStatus { progress, complete, failed };

enum class FiniteStretchFailure {
    none,
    invalid_ratio,
    output_too_short,
    output_too_long,
    processor_protocol_error,
};

enum class FiniteStretchWorkUnit { none, feed, drain, finalize };
enum class FiniteStretchWorkOutcome { none, advanced, backpressure };

enum class FiniteStretchCounterGeometryStatus {
    representable,
    input_counter_overflow,
    output_counter_overflow,
};

inline FiniteStretchCounterGeometryStatus checked_finite_stretch_counter_geometry(
    const RealtimePitchTimeConfig& config, std::uint64_t input_frames) noexcept {
    const bool quality = config.quality == PitchTimeQuality::quality;
    const auto fft_size = static_cast<std::uint64_t>(
        config.fft_size > 0 ? config.fft_size : (quality ? 4096 : 1024));
    const auto analysis_hop = static_cast<std::uint64_t>(
        config.analysis_hop > 0 ? config.analysis_hop : (quality ? 512 : 256));
    const auto signed_max = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    std::uint64_t eof_padding = 0;
    std::uint64_t padded_input = 0;
    if (!checked_capacity_product(analysis_hop, 2u, signed_max, eof_padding)
        || !checked_capacity_sum(fft_size, eof_padding, signed_max, eof_padding)
        || !checked_capacity_sum(input_frames, eof_padding, signed_max, padded_input))
        return FiniteStretchCounterGeometryStatus::input_counter_overflow;
    const long double maximum_hop_value =
        std::ceil(static_cast<long double>(config.max_time_ratio)
                  * static_cast<long double>(analysis_hop)) + 1.0L;
    if (!std::isfinite(maximum_hop_value)
        || maximum_hop_value > static_cast<long double>(std::numeric_limits<int>::max()))
        return FiniteStretchCounterGeometryStatus::output_counter_overflow;
    std::uint64_t frame_count = 0;
    std::uint64_t maximum_output = 0;
    if (!checked_capacity_sum(padded_input / std::max<std::uint64_t>(analysis_hop, 1u),
                             2u, signed_max, frame_count)
        || !checked_capacity_product(frame_count,
                                     static_cast<std::uint64_t>(maximum_hop_value),
                                     signed_max, maximum_output))
        return FiniteStretchCounterGeometryStatus::output_counter_overflow;
    return FiniteStretchCounterGeometryStatus::representable;
}

/// Called for the analysis frame ending at `input_frame`. Positions after EOF
/// are clamped to the finite input length, so finalization holds the endpoint
/// ratio. The callback must be allocation-free and noexcept.
using FiniteStretchRatioAtInputFrame =
    float (*)(void* context, std::uint64_t input_frame) noexcept;

template <typename SampleType = float>
struct FiniteStretchConfigT {
    double sample_rate = 48000.0;
    RealtimePitchTimeConfig processor;
    const SampleType* const* input = nullptr;
    std::uint64_t input_frames = 0;
    SampleType* const* output = nullptr;
    std::uint64_t output_capacity_frames = 0;
    std::uint64_t target_frames = 0;
    float constant_time_ratio = 1.0f;
    FiniteStretchRatioAtInputFrame ratio_at_input_frame = nullptr;
    void* ratio_context = nullptr;
    std::uint64_t target_max_bytes = kTargetAddressMaximumBytes;
};

template <typename SampleType = float>
class FiniteStretchBuilderT {
public:
    using Config = FiniteStretchConfigT<SampleType>;

    FiniteStretchPrepareStatus prepare(const Config& config) {
        prepare_status_ = FiniteStretchPrepareStatus::invalid_input;
        processor_prepare_status_ = PitchTimePrepareStatus::invalid_sample_rate;
        input_consumed_ = 0;
        output_written_ = 0;
        pending_ = {};
        drain_before_pending_ = false;
        done_ = false;
        failure_ = FiniteStretchFailure::none;
        last_work_unit_ = FiniteStretchWorkUnit::none;
        last_work_outcome_ = FiniteStretchWorkOutcome::none;
        prepare_status_ = validate(config);
        if (prepare_status_ != FiniteStretchPrepareStatus::prepared) return prepare_status_;

        processor_prepare_status_ =
            processor_.prepare(config.sample_rate, config.processor, config.target_max_bytes);
        if (processor_prepare_status_ != PitchTimePrepareStatus::prepared) {
            prepare_status_ = FiniteStretchPrepareStatus::processor_prepare_failed;
            return prepare_status_;
        }

        config_ = config;
        return prepare_status_;
    }

    FiniteStretchStepStatus step() noexcept {
        last_work_unit_ = FiniteStretchWorkUnit::none;
        last_work_outcome_ = FiniteStretchWorkOutcome::none;
        if (prepare_status_ != FiniteStretchPrepareStatus::prepared
            || failure_ != FiniteStretchFailure::none)
            return FiniteStretchStepStatus::failed;
        if (done_) return FiniteStretchStepStatus::complete;

        if (drain_before_pending_) return drain_one();
        if (pending_.kind != PendingKind::none) return execute_pending();
        if (input_consumed_ < config_.input_frames) return begin_feed();
        return finalize_one();
    }

    FiniteStretchPrepareStatus prepare_status() const noexcept { return prepare_status_; }
    PitchTimePrepareStatus processor_prepare_status() const noexcept {
        return processor_prepare_status_;
    }
    FiniteStretchFailure failure() const noexcept { return failure_; }
    FiniteStretchWorkUnit last_work_unit() const noexcept { return last_work_unit_; }
    FiniteStretchWorkOutcome last_work_outcome() const noexcept { return last_work_outcome_; }
    std::uint64_t input_frames_consumed() const noexcept { return input_consumed_; }
    std::uint64_t output_frames_written() const noexcept { return output_written_; }
    bool complete() const noexcept { return done_; }

private:
    static FiniteStretchPrepareStatus validate(const Config& config) noexcept {
        if (config.processor.mode != PitchTimeMode::time_stretch)
            return FiniteStretchPrepareStatus::invalid_mode;
        if (config.processor.channels < 1
            || config.processor.channels > kRealtimePitchTimeMaximumChannels)
            return FiniteStretchPrepareStatus::invalid_input;
        if (config.input_frames > 0 && config.input == nullptr)
            return FiniteStretchPrepareStatus::invalid_input;
        if (config.target_frames > config.output_capacity_frames)
            return FiniteStretchPrepareStatus::invalid_target;
        if (config.output_capacity_frames > 0 && config.output == nullptr)
            return FiniteStretchPrepareStatus::invalid_output;
        if (config.ratio_at_input_frame == nullptr
            && std::isfinite(config.processor.max_time_ratio)
            && config.processor.max_time_ratio >= 1.0f) {
            const float minimum = 1.0f / config.processor.max_time_ratio;
            if (!std::isfinite(config.constant_time_ratio)
                || config.constant_time_ratio < minimum
                || config.constant_time_ratio > config.processor.max_time_ratio)
                return FiniteStretchPrepareStatus::invalid_ratio;
        }
        for (int channel = 0; channel < config.processor.channels; ++channel) {
            if (config.input_frames > 0 && config.input[channel] == nullptr)
                return FiniteStretchPrepareStatus::invalid_input;
            if (config.output_capacity_frames > 0 && config.output[channel] == nullptr)
                return FiniteStretchPrepareStatus::invalid_output;
        }
        const auto maximum_index = static_cast<std::uint64_t>(
            std::min(std::numeric_limits<std::size_t>::max(),
                     static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())));
        if (config.input_frames > maximum_index || config.output_capacity_frames > maximum_index
            || config.target_frames
                   > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return FiniteStretchPrepareStatus::unrepresentable_capacity;
        // Input and output are independent caller-owned planar allocations.
        // Validate each channel span, not their aggregate, against the target
        // addressability ceiling.
        std::uint64_t input_bytes = 0;
        std::uint64_t output_bytes = 0;
        if (!checked_allocation_bytes<SampleType>(config.input_frames,
                                                  config.target_max_bytes, &input_bytes)
            || !checked_allocation_bytes<SampleType>(config.output_capacity_frames,
                                                     config.target_max_bytes, &output_bytes))
            return FiniteStretchPrepareStatus::unrepresentable_capacity;
        if (std::isfinite(config.processor.max_time_ratio)
            && config.processor.max_time_ratio >= 1.0f
            && checked_finite_stretch_counter_geometry(config.processor, config.input_frames)
                   != FiniteStretchCounterGeometryStatus::representable)
            return FiniteStretchPrepareStatus::unrepresentable_capacity;

        const auto range_representable = [](std::uintptr_t address,
                                            std::uint64_t bytes) noexcept {
            std::uint64_t end = 0;
            return checked_capacity_sum(address, bytes,
                                        std::numeric_limits<std::uintptr_t>::max(), end);
        };
        const auto ranges_overlap = [](std::uintptr_t first, std::uint64_t first_bytes,
                                       std::uintptr_t second, std::uint64_t second_bytes) noexcept {
            if (first_bytes == 0 || second_bytes == 0) return false;
            std::uint64_t first_end = 0;
            std::uint64_t second_end = 0;
            if (!checked_capacity_sum(first, first_bytes,
                                      std::numeric_limits<std::uintptr_t>::max(), first_end)
                || !checked_capacity_sum(second, second_bytes,
                                         std::numeric_limits<std::uintptr_t>::max(), second_end))
                return true;
            return first < second_end && second < first_end;
        };
        for (int channel = 0; channel < config.processor.channels; ++channel) {
            if (input_bytes > 0
                && !range_representable(reinterpret_cast<std::uintptr_t>(config.input[channel]),
                                        input_bytes))
                return FiniteStretchPrepareStatus::invalid_output_layout;
            if (output_bytes > 0
                && !range_representable(
                    reinterpret_cast<std::uintptr_t>(config.output[channel]), output_bytes))
                return FiniteStretchPrepareStatus::invalid_output_layout;
        }
        for (int out_channel = 0; output_bytes > 0 && out_channel < config.processor.channels;
             ++out_channel) {
            const auto out_address = reinterpret_cast<std::uintptr_t>(config.output[out_channel]);
            for (int input_channel = 0;
                 input_bytes > 0 && input_channel < config.processor.channels; ++input_channel)
                if (ranges_overlap(out_address, output_bytes,
                                   reinterpret_cast<std::uintptr_t>(config.input[input_channel]),
                                   input_bytes))
                    return FiniteStretchPrepareStatus::invalid_output_layout;
            for (int prior = 0; prior < out_channel; ++prior)
                if (ranges_overlap(out_address, output_bytes,
                                   reinterpret_cast<std::uintptr_t>(config.output[prior]),
                                   output_bytes))
                    return FiniteStretchPrepareStatus::invalid_output_layout;
        }
        return FiniteStretchPrepareStatus::prepared;
    }

    bool resolve_ratio(std::uint64_t boundary, float& resolved) noexcept {
        const auto position = std::min(boundary, config_.input_frames);
        const float ratio = config_.ratio_at_input_frame != nullptr
                                ? config_.ratio_at_input_frame(config_.ratio_context, position)
                                : config_.constant_time_ratio;
        const float minimum = 1.0f / config_.processor.max_time_ratio;
        if (!std::isfinite(ratio) || ratio < minimum || ratio > config_.processor.max_time_ratio) {
            failure_ = FiniteStretchFailure::invalid_ratio;
            return false;
        }
        resolved = ratio;
        return true;
    }

    FiniteStretchStepStatus begin_feed() noexcept {
        const int until_boundary = processor_.samples_until_next_analysis_frame();
        if (until_boundary <= 0) return fail(FiniteStretchFailure::processor_protocol_error);
        const auto remaining = config_.input_frames - input_consumed_;
        pending_.kind = PendingKind::feed;
        pending_.count = static_cast<int>(std::min<std::uint64_t>(
            remaining, static_cast<std::uint64_t>(
                           std::min(config_.processor.max_block, until_boundary))));
        if (pending_.count <= 0) return fail(FiniteStretchFailure::processor_protocol_error);
        pending_.apply_ratio = pending_.count == until_boundary;
        const auto boundary = input_consumed_ + static_cast<std::uint64_t>(pending_.count);
        if (pending_.apply_ratio && !resolve_ratio(boundary, pending_.ratio))
            return FiniteStretchStepStatus::failed;
        return execute_pending();
    }

    FiniteStretchStepStatus execute_pending() noexcept {
        if (pending_.apply_ratio) processor_.set_time_ratio(pending_.ratio);
        if (pending_.kind == PendingKind::finalize) return execute_finalize();
        if (pending_.kind != PendingKind::feed)
            return fail(FiniteStretchFailure::processor_protocol_error);
        for (int channel = 0; channel < config_.processor.channels; ++channel)
            input_ptrs_[static_cast<std::size_t>(channel)] =
                config_.input[channel] + static_cast<std::size_t>(input_consumed_);
        last_work_unit_ = FiniteStretchWorkUnit::feed;
        const auto status = processor_.feed(input_ptrs_.data(), pending_.count);
        if (status == PitchTimeStreamFeedStatus::accepted) {
            input_consumed_ += static_cast<std::uint64_t>(pending_.count);
            pending_ = {};
            last_work_outcome_ = FiniteStretchWorkOutcome::advanced;
            return FiniteStretchStepStatus::progress;
        }
        if (status == PitchTimeStreamFeedStatus::backpressure) {
            drain_before_pending_ = true;
            last_work_outcome_ = FiniteStretchWorkOutcome::backpressure;
            return FiniteStretchStepStatus::progress;
        }
        return fail(FiniteStretchFailure::processor_protocol_error);
    }

    FiniteStretchStepStatus drain_one() noexcept {
        const int available = processor_.available_stretched();
        if (available <= 0) {
            drain_before_pending_ = false;
            return FiniteStretchStepStatus::progress;
        }
        const auto remaining = config_.target_frames - output_written_;
        if (remaining == 0) return fail(FiniteStretchFailure::output_too_long);
        const int take = static_cast<int>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(
                std::min(available, config_.processor.max_block)),
            remaining));
        for (int channel = 0; channel < config_.processor.channels; ++channel)
            output_ptrs_[static_cast<std::size_t>(channel)] =
                config_.output[channel] + static_cast<std::size_t>(output_written_);
        last_work_unit_ = FiniteStretchWorkUnit::drain;
        const int read = processor_.read_stretched(output_ptrs_.data(), take);
        if (read != take) return fail(FiniteStretchFailure::processor_protocol_error);
        last_work_outcome_ = FiniteStretchWorkOutcome::advanced;
        output_written_ += static_cast<std::uint64_t>(read);
        drain_before_pending_ = processor_.available_stretched() > 0;
        if (static_cast<std::uint64_t>(available) > remaining)
            return fail(FiniteStretchFailure::output_too_long);
        return FiniteStretchStepStatus::progress;
    }

    FiniteStretchStepStatus finalize_one() noexcept {
        const int until_boundary = processor_.samples_until_next_analysis_frame();
        if (until_boundary <= 0) return fail(FiniteStretchFailure::processor_protocol_error);
        const auto plan = processor_.plan_finalize(
            std::min(config_.processor.max_block, until_boundary));
        if (plan.status == PitchTimeStreamFinalizePlanStatus::complete) {
            if (output_written_ != config_.target_frames)
                return fail(FiniteStretchFailure::output_too_short);
            done_ = true;
            return FiniteStretchStepStatus::complete;
        }
        if (plan.status == PitchTimeStreamFinalizePlanStatus::needs_drain) {
            if (processor_.available_stretched() <= 0)
                return fail(FiniteStretchFailure::processor_protocol_error);
            drain_before_pending_ = true;
            return drain_one();
        }
        if (plan.status != PitchTimeStreamFinalizePlanStatus::ready)
            return fail(FiniteStretchFailure::processor_protocol_error);
        pending_.kind = PendingKind::finalize;
        pending_.count = plan.samples;
        pending_.apply_ratio = pending_.count == until_boundary;
        const auto boundary = input_consumed_ + static_cast<std::uint64_t>(pending_.count);
        if (pending_.apply_ratio && !resolve_ratio(boundary, pending_.ratio))
            return FiniteStretchStepStatus::failed;
        return execute_pending();
    }

    FiniteStretchStepStatus execute_finalize() noexcept {
        last_work_unit_ = FiniteStretchWorkUnit::finalize;
        const auto status = processor_.finalize(pending_.count);
        if (status == PitchTimeStreamFinalizeStatus::complete) {
            pending_ = {};
            last_work_outcome_ = FiniteStretchWorkOutcome::advanced;
            if (processor_.available_stretched() != 0)
                return fail(FiniteStretchFailure::processor_protocol_error);
            if (output_written_ != config_.target_frames)
                return fail(FiniteStretchFailure::output_too_short);
            done_ = true;
            return FiniteStretchStepStatus::complete;
        }
        if (status == PitchTimeStreamFinalizeStatus::draining) {
            pending_ = {};
            last_work_outcome_ = FiniteStretchWorkOutcome::advanced;
            drain_before_pending_ = processor_.available_stretched() > 0;
            return FiniteStretchStepStatus::progress;
        }
        return fail(FiniteStretchFailure::processor_protocol_error);
    }

    FiniteStretchStepStatus fail(FiniteStretchFailure failure) noexcept {
        failure_ = failure;
        return FiniteStretchStepStatus::failed;
    }

    Config config_ {};
    RealtimePitchTimeProcessorT<SampleType> processor_;
    std::array<const SampleType*, kRealtimePitchTimeMaximumChannels> input_ptrs_ {};
    std::array<SampleType*, kRealtimePitchTimeMaximumChannels> output_ptrs_ {};
    std::uint64_t input_consumed_ = 0;
    std::uint64_t output_written_ = 0;
    enum class PendingKind { none, feed, finalize };
    struct PendingWork {
        PendingKind kind = PendingKind::none;
        int count = 0;
        float ratio = 1.0f;
        bool apply_ratio = false;
    };
    PendingWork pending_ {};
    bool drain_before_pending_ = false;
    bool done_ = false;
    FiniteStretchPrepareStatus prepare_status_ = FiniteStretchPrepareStatus::invalid_input;
    PitchTimePrepareStatus processor_prepare_status_ = PitchTimePrepareStatus::invalid_sample_rate;
    FiniteStretchFailure failure_ = FiniteStretchFailure::none;
    FiniteStretchWorkUnit last_work_unit_ = FiniteStretchWorkUnit::none;
    FiniteStretchWorkOutcome last_work_outcome_ = FiniteStretchWorkOutcome::none;
};

using FiniteStretchConfig = FiniteStretchConfigT<float>;
using FiniteStretchConfig64 = FiniteStretchConfigT<double>;
using FiniteStretchBuilder = FiniteStretchBuilderT<float>;
using FiniteStretchBuilder64 = FiniteStretchBuilderT<double>;

} // namespace pulp::signal
