#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace pulp::audio {

class AudioStreamHandoff;
class RollingAudioCaptureBuffer;
class RealtimeSampleRecorder;
class SampleSlotBank;
struct LoopRenderResult;
struct OnsetDetectionResult;
struct SliceMap;

enum class RealtimeSampleRecorderState : std::uint8_t;
enum class SampleSlotState : std::uint8_t;

struct SamplerLooperMetricsSnapshot {
    std::uint64_t stream_underrun_frames = 0;
    std::uint64_t stream_overrun_frames = 0;
    std::uint64_t stream_dropped_source_frames = 0;
    std::uint64_t stream_queued_source_frames = 0;
    double stream_queued_source_seconds = 0.0;
    double stream_estimated_latency_seconds = 0.0;

    std::uint64_t capture_capacity_frames = 0;
    std::uint64_t capture_allocated_bytes = 0;

    RealtimeSampleRecorderState recorder_state{};
    std::uint64_t recorder_frames_recorded = 0;
    std::uint64_t recorder_total_recorded_frames = 0;
    std::uint64_t recorder_dropped_commands = 0;
    std::uint64_t recorder_dropped_events = 0;

    std::uint32_t slot_count = 0;
    std::array<std::uint32_t, 6> slot_state_counts{};
    bool published_sample_valid = false;
    std::uint32_t published_slot_index = 0;
    std::uint64_t active_generation = 0;
    std::uint64_t published_sample_frames = 0;
    std::uint32_t published_sample_channels = 0;
    double published_sample_rate = 0.0;

    std::uint64_t loop_rendered_frames = 0;
    std::uint64_t loop_silent_frames = 0;
    bool loop_active = false;
    bool loop_wrapped = false;
    float loop_max_sample_delta = 0.0f;

    std::size_t onset_count = 0;
    std::size_t slice_count = 0;
};

std::size_t sample_slot_state_index(SampleSlotState state) noexcept;

SamplerLooperMetricsSnapshot collect_sampler_looper_metrics(
    const AudioStreamHandoff* handoff = nullptr,
    const RollingAudioCaptureBuffer* capture = nullptr,
    const RealtimeSampleRecorder* recorder = nullptr,
    const SampleSlotBank* slots = nullptr,
    const LoopRenderResult* loop = nullptr,
    const OnsetDetectionResult* onsets = nullptr,
    const SliceMap* slices = nullptr) noexcept;

}  // namespace pulp::audio
