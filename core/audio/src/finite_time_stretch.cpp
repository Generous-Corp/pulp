#include <pulp/audio/finite_time_stretch.hpp>

#include <pulp/audio/sample_rate_conversion.hpp>
#include <pulp/signal/finite_stretch_builder.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pulp::audio {
namespace {

bool checked_multiply(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t limit,
                      std::uint64_t& result) noexcept {
    if (lhs != 0 && rhs > limit / lhs)
        return false;
    result = lhs * rhs;
    return true;
}

bool checked_add(std::uint64_t amount, std::uint64_t limit, std::uint64_t& total) noexcept {
    if (total > limit || amount > limit - total)
        return false;
    total += amount;
    return true;
}

FiniteTimeStretchPrepareStatus
map_prepare_status(signal::FiniteStretchPrepareStatus status) noexcept {
    using Source = signal::FiniteStretchPrepareStatus;
    switch (status) {
    case Source::prepared:
        return FiniteTimeStretchPrepareStatus::Prepared;
    case Source::invalid_ratio:
        return FiniteTimeStretchPrepareStatus::InvalidRatio;
    case Source::unrepresentable_capacity:
        return FiniteTimeStretchPrepareStatus::CapacityExceeded;
    case Source::processor_prepare_failed:
        return FiniteTimeStretchPrepareStatus::ProcessorPrepareFailed;
    case Source::invalid_target:
        return FiniteTimeStretchPrepareStatus::InvalidTarget;
    case Source::invalid_mode:
    case Source::invalid_input:
    case Source::invalid_output:
    case Source::invalid_output_layout:
        return FiniteTimeStretchPrepareStatus::ProcessorPrepareFailed;
    }
    return FiniteTimeStretchPrepareStatus::ProcessorPrepareFailed;
}

FiniteTimeStretchFailure map_failure(signal::FiniteStretchFailure failure) noexcept {
    using Source = signal::FiniteStretchFailure;
    switch (failure) {
    case Source::none:
        return FiniteTimeStretchFailure::None;
    case Source::invalid_ratio:
        return FiniteTimeStretchFailure::InvalidRatio;
    case Source::output_too_short:
        return FiniteTimeStretchFailure::OutputTooShort;
    case Source::output_too_long:
        return FiniteTimeStretchFailure::OutputTooLong;
    case Source::processor_protocol_error:
        return FiniteTimeStretchFailure::ProcessorProtocolError;
    }
    return FiniteTimeStretchFailure::ProcessorProtocolError;
}

} // namespace

struct FiniteTimeStretchJob::Impl {
    static float ratio_trampoline(void* context, std::uint64_t input_frame) noexcept {
        auto& self = *static_cast<Impl*>(context);
        return self.config.ratio_at_input_frame(self.config.ratio_context, input_frame,
                                                self.timeline_input_frames);
    }

    FiniteTimeStretchPrepareStatus prepare(FiniteTimeStretchConfig next) {
        config = std::move(next);
        prepare_result = FiniteTimeStretchPrepareStatus::InvalidSource;
        failure_result = FiniteTimeStretchFailure::None;
        current_stage = FiniteTimeStretchStage::Failed;
        materialized_frames = 0;
        converted_frames = 0;
        timeline_input_frames = 0;
        converter.reset();
        converter_builder.reset();
        input.clear();
        output.clear();
        input_planes.clear();
        output_planes.clear();
        sealed_mutable.reset();
        sealed.reset();

        if (!config.source || config.source->sample_rate == 0 || config.source->channels.empty() ||
            config.source->num_frames() == 0)
            return prepare_result;
        const auto source_frames = config.source->num_frames();
        const auto channels = config.source->channels.size();
        if (config.source_start > source_frames || config.source_frame_count == 0 ||
            config.source_frame_count > source_frames - config.source_start ||
            channels > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            !std::all_of(
                config.source->channels.begin(), config.source->channels.end(),
                [source_frames](const auto& channel) { return channel.size() == source_frames; })) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidSlice;
            return prepare_result;
        }
        if (config.timeline_sample_rate == 0) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidSampleRate;
            return prepare_result;
        }
        if (config.max_block_frames == 0 ||
            config.max_block_frames > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidBlockSize;
            return prepare_result;
        }
        if (config.target_frame_count == 0) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidTarget;
            return prepare_result;
        }
        if (!std::isfinite(config.max_time_ratio) || config.max_time_ratio < 1.0f ||
            (config.ratio_at_input_frame == nullptr &&
             (!std::isfinite(config.constant_time_ratio) ||
              config.constant_time_ratio < 1.0f / config.max_time_ratio ||
              config.constant_time_ratio > config.max_time_ratio))) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidRatio;
            return prepare_result;
        }

        const auto scaled = static_cast<long double>(config.source_frame_count) *
                            static_cast<long double>(config.timeline_sample_rate) /
                            static_cast<long double>(config.source->sample_rate);
        if (!std::isfinite(scaled) || scaled <= 0.0L ||
            scaled > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
            prepare_result = FiniteTimeStretchPrepareStatus::InvalidSampleRate;
            return prepare_result;
        }
        timeline_input_frames = static_cast<std::uint64_t>(std::ceil(scaled));
        if (timeline_input_frames > config.max_input_frames ||
            config.target_frame_count > config.max_output_frames ||
            timeline_input_frames >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
            config.target_frame_count >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
            return prepare_result;
        }
        std::uint64_t input_samples = 0;
        std::uint64_t output_samples = 0;
        std::uint64_t input_bytes = 0;
        std::uint64_t output_bytes = 0;
        std::uint64_t artifact_bytes = 0;
        std::uint64_t input_channel_bytes = 0;
        std::uint64_t output_channel_bytes = 0;
        std::uint64_t input_plane_bytes = 0;
        std::uint64_t output_plane_bytes = 0;
        std::uint64_t input_overhead = 0;
        std::uint64_t output_overhead = 0;
        std::uint64_t artifact_channel_bytes = 0;
        std::uint64_t artifact_overhead = 0;
        if (!checked_multiply(timeline_input_frames, channels, config.max_input_bytes,
                              input_samples) ||
            !checked_multiply(config.target_frame_count, channels, config.max_output_bytes,
                              output_samples) ||
            !checked_multiply(input_samples, sizeof(double), config.max_input_bytes, input_bytes) ||
            !checked_multiply(output_samples, sizeof(double), config.max_output_bytes,
                              output_bytes) ||
            !checked_multiply(output_samples, sizeof(float), config.max_artifact_bytes,
                              artifact_bytes) ||
            !checked_multiply(channels, sizeof(std::vector<double>), config.max_input_bytes,
                              input_channel_bytes) ||
            !checked_multiply(channels, sizeof(std::vector<double>), config.max_output_bytes,
                              output_channel_bytes) ||
            !checked_multiply(channels, sizeof(const double*), config.max_input_bytes,
                              input_plane_bytes) ||
            !checked_multiply(channels, sizeof(double*), config.max_output_bytes,
                              output_plane_bytes) ||
            !checked_multiply(channels + 2u, 8u * sizeof(void*), config.max_input_bytes,
                              input_overhead) ||
            !checked_multiply(channels + 2u, 8u * sizeof(void*), config.max_output_bytes,
                              output_overhead) ||
            !checked_multiply(channels, sizeof(std::vector<float>), config.max_artifact_bytes,
                              artifact_channel_bytes) ||
            !checked_multiply(channels + 2u, 8u * sizeof(void*), config.max_artifact_bytes,
                              artifact_overhead) ||
            !checked_add(sizeof(AudioFileData), config.max_artifact_bytes, artifact_bytes) ||
            !checked_add(input_channel_bytes, config.max_input_bytes, input_bytes) ||
            !checked_add(input_plane_bytes, config.max_input_bytes, input_bytes) ||
            !checked_add(input_overhead, config.max_input_bytes, input_bytes) ||
            !checked_add(output_channel_bytes, config.max_output_bytes, output_bytes) ||
            !checked_add(output_plane_bytes, config.max_output_bytes, output_bytes) ||
            !checked_add(output_overhead, config.max_output_bytes, output_bytes) ||
            !checked_add(artifact_channel_bytes, config.max_artifact_bytes, artifact_bytes) ||
            !checked_add(artifact_overhead, config.max_artifact_bytes, artifact_bytes)) {
            prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
            return prepare_result;
        }

#if defined(__cpp_exceptions)
        try {
#endif
            input.assign(channels,
                         std::vector<double>(static_cast<std::size_t>(timeline_input_frames)));
            output.assign(channels,
                          std::vector<double>(static_cast<std::size_t>(config.target_frame_count)));
            sealed_mutable = std::make_shared<AudioFileData>();
            sealed_mutable->channels.assign(
                channels, std::vector<float>(static_cast<std::size_t>(config.target_frame_count)));
            sealed_mutable->sample_rate = config.timeline_sample_rate;
            input_planes.reserve(channels);
            output_planes.reserve(channels);
            for (std::size_t channel = 0; channel < channels; ++channel) {
                input_planes.push_back(input[channel].data());
                output_planes.push_back(output[channel].data());
            }
#if defined(__cpp_exceptions)
        } catch (const std::bad_alloc&) {
            prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
            return prepare_result;
        } catch (const std::length_error&) {
            prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
            return prepare_result;
        }
#endif

        if (config.source->sample_rate != config.timeline_sample_rate) {
            if (PreparedSampleRateConversion::estimated_prepared_bytes() >
                config.max_sample_rate_converter_bytes) {
                prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
                return prepare_result;
            }
            const auto cutoff = std::min(1.0, static_cast<double>(config.timeline_sample_rate) /
                                                  static_cast<double>(config.source->sample_rate));
#if defined(__cpp_exceptions)
            try {
#endif
                converter_builder = std::make_unique<SampleRateConversionBuilder>(cutoff);
#if defined(__cpp_exceptions)
            } catch (const std::bad_alloc&) {
                prepare_result = FiniteTimeStretchPrepareStatus::CapacityExceeded;
                return prepare_result;
            }
#endif
            if (!converter_builder->valid()) {
                prepare_result = FiniteTimeStretchPrepareStatus::SampleRateConverterPrepareFailed;
                return prepare_result;
            }
        }

        prepare_result = FiniteTimeStretchPrepareStatus::Prepared;
        current_stage = converter_builder ? FiniteTimeStretchStage::PrepareSampleRateConverter
                                          : FiniteTimeStretchStage::MaterializeTimelineInput;
        return prepare_result;
    }

    FiniteTimeStretchStepStatus step() noexcept {
        if (current_stage == FiniteTimeStretchStage::Complete)
            return FiniteTimeStretchStepStatus::Complete;
        if (current_stage == FiniteTimeStretchStage::Failed ||
            prepare_result != FiniteTimeStretchPrepareStatus::Prepared)
            return FiniteTimeStretchStepStatus::Failed;
        if (current_stage == FiniteTimeStretchStage::PrepareSampleRateConverter) {
#if defined(__cpp_exceptions)
            try {
#endif
                if (!converter_builder || !converter_builder->step())
                    return FiniteTimeStretchStepStatus::Progress;
                converter = converter_builder->take();
#if defined(__cpp_exceptions)
            } catch (const std::bad_alloc&) {
                failure_result = FiniteTimeStretchFailure::AllocationFailed;
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            } catch (const std::length_error&) {
                failure_result = FiniteTimeStretchFailure::AllocationFailed;
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            }
#endif
            converter_builder.reset();
            if (!converter) {
                failure_result = FiniteTimeStretchFailure::ProcessorProtocolError;
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            }
            current_stage = FiniteTimeStretchStage::MaterializeTimelineInput;
            return FiniteTimeStretchStepStatus::Progress;
        }
        if (current_stage == FiniteTimeStretchStage::MaterializeTimelineInput) {
            const auto remaining = timeline_input_frames - materialized_frames;
            const auto count = std::min<std::uint64_t>(remaining, config.max_block_frames);
            for (std::size_t channel = 0; channel < input.size(); ++channel) {
                const auto source =
                    std::span<const float>(config.source->channels[channel])
                        .subspan(static_cast<std::size_t>(config.source_start),
                                 static_cast<std::size_t>(config.source_frame_count));
                for (std::uint64_t offset = 0; offset < count; ++offset) {
                    const auto destination_frame = materialized_frames + offset;
                    const auto source_position = static_cast<double>(destination_frame) *
                                                 static_cast<double>(config.source->sample_rate) /
                                                 static_cast<double>(config.timeline_sample_rate);
                    input[channel][static_cast<std::size_t>(destination_frame)] =
                        converter ? converter->read(source, source_position)
                                  : source[static_cast<std::size_t>(std::min<std::uint64_t>(
                                        destination_frame, config.source_frame_count - 1u))];
                }
            }
            materialized_frames += count;
            if (materialized_frames != timeline_input_frames)
                return FiniteTimeStretchStepStatus::Progress;

            current_stage = FiniteTimeStretchStage::PrepareStretch;
            return FiniteTimeStretchStepStatus::Progress;
        }

        if (current_stage == FiniteTimeStretchStage::PrepareStretch) {
            signal::FiniteStretchConfig64 stretch_config;
            stretch_config.sample_rate = config.timeline_sample_rate;
            stretch_config.processor.mode = signal::PitchTimeMode::time_stretch;
            stretch_config.processor.quality = signal::PitchTimeQuality::quality;
            stretch_config.processor.channels = static_cast<int>(input.size());
            stretch_config.processor.max_block = static_cast<int>(config.max_block_frames);
            stretch_config.processor.max_time_ratio = config.max_time_ratio;
            stretch_config.input = input_planes.data();
            stretch_config.input_frames = timeline_input_frames;
            stretch_config.output = output_planes.data();
            stretch_config.output_capacity_frames = config.target_frame_count;
            stretch_config.target_frames = config.target_frame_count;
            stretch_config.constant_time_ratio = config.constant_time_ratio;
            stretch_config.ratio_at_input_frame =
                config.ratio_at_input_frame ? &Impl::ratio_trampoline : nullptr;
            stretch_config.ratio_context = this;
            stretch_config.target_max_bytes = config.max_scratch_allocation_bytes;
            signal::FiniteStretchPrepareStatus status;
#if defined(__cpp_exceptions)
            try {
#endif
                status = stretcher.prepare(stretch_config);
#if defined(__cpp_exceptions)
            } catch (const std::bad_alloc&) {
                failure_result = FiniteTimeStretchFailure::AllocationFailed;
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            } catch (const std::length_error&) {
                failure_result = FiniteTimeStretchFailure::AllocationFailed;
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            }
#endif
            if (status != signal::FiniteStretchPrepareStatus::prepared) {
                prepare_result =
                    status == signal::FiniteStretchPrepareStatus::processor_prepare_failed &&
                            stretcher.processor_prepare_status() ==
                                signal::PitchTimePrepareStatus::unrepresentable_capacity
                        ? FiniteTimeStretchPrepareStatus::CapacityExceeded
                        : map_prepare_status(status);
                current_stage = FiniteTimeStretchStage::Failed;
                return FiniteTimeStretchStepStatus::Failed;
            }
            current_stage = FiniteTimeStretchStage::Stretch;
            return FiniteTimeStretchStepStatus::Progress;
        }

        if (current_stage == FiniteTimeStretchStage::ConvertOutput) {
            const auto remaining = config.target_frame_count - converted_frames;
            const auto count = std::min<std::uint64_t>(remaining, config.max_block_frames);
            for (std::size_t channel = 0; channel < output.size(); ++channel) {
                for (std::uint64_t offset = 0; offset < count; ++offset) {
                    const auto frame = converted_frames + offset;
                    const auto value = output[channel][static_cast<std::size_t>(frame)];
                    const auto converted = static_cast<float>(value);
                    if (!std::isfinite(value) || !std::isfinite(converted)) {
                        failure_result = FiniteTimeStretchFailure::ProcessorProtocolError;
                        current_stage = FiniteTimeStretchStage::Failed;
                        return FiniteTimeStretchStepStatus::Failed;
                    }
                    sealed_mutable->channels[channel][static_cast<std::size_t>(frame)] = converted;
                }
            }
            converted_frames += count;
            if (converted_frames != config.target_frame_count)
                return FiniteTimeStretchStepStatus::Progress;
            sealed = std::move(sealed_mutable);
            input.clear();
            output.clear();
            input_planes.clear();
            output_planes.clear();
            current_stage = FiniteTimeStretchStage::Complete;
            return FiniteTimeStretchStepStatus::Complete;
        }

        const auto status = stretcher.step();
        if (status == signal::FiniteStretchStepStatus::progress)
            return FiniteTimeStretchStepStatus::Progress;
        if (status == signal::FiniteStretchStepStatus::failed) {
            failure_result = map_failure(stretcher.failure());
            current_stage = FiniteTimeStretchStage::Failed;
            return FiniteTimeStretchStepStatus::Failed;
        }
        if (!sealed_mutable || stretcher.output_frames_written() != config.target_frame_count) {
            failure_result = FiniteTimeStretchFailure::ProcessorProtocolError;
            current_stage = FiniteTimeStretchStage::Failed;
            return FiniteTimeStretchStepStatus::Failed;
        }
        current_stage = FiniteTimeStretchStage::ConvertOutput;
        return FiniteTimeStretchStepStatus::Progress;
    }

    FiniteTimeStretchConfig config;
    FiniteTimeStretchPrepareStatus prepare_result = FiniteTimeStretchPrepareStatus::InvalidSource;
    FiniteTimeStretchFailure failure_result = FiniteTimeStretchFailure::None;
    FiniteTimeStretchStage current_stage = FiniteTimeStretchStage::Failed;
    std::uint64_t timeline_input_frames = 0;
    std::uint64_t materialized_frames = 0;
    std::uint64_t converted_frames = 0;
    std::shared_ptr<const PreparedSampleRateConversion> converter;
    std::unique_ptr<SampleRateConversionBuilder> converter_builder;
    std::vector<std::vector<double>> input;
    std::vector<std::vector<double>> output;
    std::vector<const double*> input_planes;
    std::vector<double*> output_planes;
    signal::FiniteStretchBuilder64 stretcher;
    std::shared_ptr<AudioFileData> sealed_mutable;
    std::shared_ptr<const AudioFileData> sealed;
};

FiniteTimeStretchJob::FiniteTimeStretchJob() : impl_(std::make_unique<Impl>()) {}
FiniteTimeStretchJob::~FiniteTimeStretchJob() = default;
FiniteTimeStretchJob::FiniteTimeStretchJob(FiniteTimeStretchJob&&) noexcept = default;
FiniteTimeStretchJob& FiniteTimeStretchJob::operator=(FiniteTimeStretchJob&&) noexcept = default;

FiniteTimeStretchPrepareStatus FiniteTimeStretchJob::prepare(FiniteTimeStretchConfig config) {
    return impl_->prepare(std::move(config));
}

FiniteTimeStretchStepStatus FiniteTimeStretchJob::step() noexcept {
    return impl_->step();
}
FiniteTimeStretchPrepareStatus FiniteTimeStretchJob::prepare_status() const noexcept {
    return impl_->prepare_result;
}
FiniteTimeStretchFailure FiniteTimeStretchJob::failure() const noexcept {
    return impl_->failure_result;
}
FiniteTimeStretchStage FiniteTimeStretchJob::stage() const noexcept {
    return impl_->current_stage;
}
std::uint64_t FiniteTimeStretchJob::timeline_input_frame_count() const noexcept {
    return impl_->timeline_input_frames;
}
std::uint64_t FiniteTimeStretchJob::input_frames_materialized() const noexcept {
    return impl_->materialized_frames;
}
std::uint64_t FiniteTimeStretchJob::output_frames_written() const noexcept {
    return impl_->stretcher.output_frames_written();
}
std::shared_ptr<const AudioFileData> FiniteTimeStretchJob::take() noexcept {
    return impl_->current_stage == FiniteTimeStretchStage::Complete ? std::move(impl_->sealed)
                                                                    : nullptr;
}

} // namespace pulp::audio
