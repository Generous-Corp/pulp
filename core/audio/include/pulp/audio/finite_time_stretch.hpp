#pragma once

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <memory>

namespace pulp::audio {

enum class FiniteTimeStretchPrepareStatus : std::uint8_t {
    Prepared,
    InvalidSource,
    InvalidSlice,
    InvalidSampleRate,
    InvalidBlockSize,
    InvalidTarget,
    InvalidRatio,
    CapacityExceeded,
    SampleRateConverterPrepareFailed,
    ProcessorPrepareFailed,
};

enum class FiniteTimeStretchStepStatus : std::uint8_t { Progress, Complete, Failed };

enum class FiniteTimeStretchFailure : std::uint8_t {
    None,
    AllocationFailed,
    InvalidRatio,
    OutputTooShort,
    OutputTooLong,
    ProcessorProtocolError,
};

enum class FiniteTimeStretchStage : std::uint8_t {
    PrepareSampleRateConverter,
    MaterializeTimelineInput,
    PrepareStretch,
    Stretch,
    ConvertOutput,
    Complete,
    Failed,
};

using FiniteTimeStretchRatioAtInputFrame = float (*)(void* context, std::uint64_t input_frame,
                                                     std::uint64_t input_frame_count) noexcept;

struct FiniteTimeStretchConfig {
    std::shared_ptr<const AudioFileData> source;
    std::uint64_t source_start = 0;
    std::uint64_t source_frame_count = 0;
    std::uint32_t timeline_sample_rate = 0;
    std::uint64_t target_frame_count = 0;
    std::uint32_t max_block_frames = 256;
    float max_time_ratio = 4.0f;
    float constant_time_ratio = 1.0f;
    FiniteTimeStretchRatioAtInputFrame ratio_at_input_frame = nullptr;
    void* ratio_context = nullptr;
    std::uint64_t max_input_frames = 100'000'000u;
    std::uint64_t max_output_frames = 100'000'000u;
    // Offline determinism uses scalar double planar DSP scratch, then performs
    // a bounded final conversion into the public float artifact.
    std::uint64_t max_input_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_output_bytes = 1024u * 1024u * 1024u;
    std::uint64_t max_artifact_bytes = 512u * 1024u * 1024u;
    std::uint64_t max_sample_rate_converter_bytes = 256u * 1024u * 1024u;
    std::uint64_t max_scratch_allocation_bytes = 256u * 1024u * 1024u;
};

/// Control-thread-only, bounded finite SRC + time-stretch job.
///
/// prepare() performs checked admission and allocates the admitted planar
/// buffers. Each step() advances one converter/processor preparation unit,
/// materializes or converts at most max_block_frames, or drives one finite
/// stretcher work unit. The completed AudioFileData is immutable.
/// In exception-enabled builds, allocation failures are reported through the
/// status enums. In no-exception builds, checked capacity rejection remains
/// recoverable but allocator OOM follows the platform allocator's termination
/// policy.
class FiniteTimeStretchJob {
  public:
    FiniteTimeStretchJob();
    ~FiniteTimeStretchJob();
    FiniteTimeStretchJob(FiniteTimeStretchJob&&) noexcept;
    FiniteTimeStretchJob& operator=(FiniteTimeStretchJob&&) noexcept;
    FiniteTimeStretchJob(const FiniteTimeStretchJob&) = delete;
    FiniteTimeStretchJob& operator=(const FiniteTimeStretchJob&) = delete;

    FiniteTimeStretchPrepareStatus prepare(FiniteTimeStretchConfig config);
    FiniteTimeStretchStepStatus step() noexcept;

    FiniteTimeStretchPrepareStatus prepare_status() const noexcept;
    FiniteTimeStretchFailure failure() const noexcept;
    FiniteTimeStretchStage stage() const noexcept;
    std::uint64_t timeline_input_frame_count() const noexcept;
    std::uint64_t input_frames_materialized() const noexcept;
    std::uint64_t output_frames_written() const noexcept;
    std::shared_ptr<const AudioFileData> take() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::audio
