#include <pulp/audio/sampler_looper_metrics.hpp>

#include <pulp/audio/audio_stream_handoff.hpp>
#include <pulp/audio/loop_renderer.hpp>
#include <pulp/audio/onset_detector.hpp>
#include <pulp/audio/realtime_sample_recorder.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/audio/sample_slot_bank.hpp>
#include <pulp/audio/slice_map.hpp>

namespace pulp::audio {

std::size_t sample_slot_state_index(SampleSlotState state) noexcept {
    return static_cast<std::size_t>(state);
}

SamplerLooperMetricsSnapshot collect_sampler_looper_metrics(
    const AudioStreamHandoff* handoff,
    const RollingAudioCaptureBuffer* capture,
    const RealtimeSampleRecorder* recorder,
    const SampleSlotBank* slots,
    const LoopRenderResult* loop,
    const OnsetDetectionResult* onsets,
    const SliceMap* slices) noexcept {
    SamplerLooperMetricsSnapshot snapshot;

    if (handoff != nullptr) {
        const auto stats = handoff->stats();
        snapshot.stream_underrun_frames = stats.underrun_frames;
        snapshot.stream_overrun_frames = stats.overrun_frames;
        snapshot.stream_dropped_source_frames = stats.dropped_source_frames;
        snapshot.stream_queued_source_frames = stats.queued_source_frames;
        snapshot.stream_queued_source_seconds = stats.queued_source_seconds;
        snapshot.stream_estimated_latency_seconds = stats.estimated_latency_seconds;
    }

    if (capture != nullptr) {
        snapshot.capture_capacity_frames = capture->capacity_frames();
        snapshot.capture_allocated_bytes = capture->allocated_bytes();
    }

    if (recorder != nullptr) {
        const auto stats = recorder->stats();
        snapshot.recorder_state = recorder->state();
        snapshot.recorder_frames_recorded = stats.frames_recorded;
        snapshot.recorder_total_recorded_frames = stats.total_recorded_frames;
        snapshot.recorder_dropped_commands = stats.dropped_commands;
        snapshot.recorder_dropped_events = stats.dropped_events;
    }

    if (slots != nullptr) {
        snapshot.slot_count = slots->slot_count();
        for (std::uint32_t i = 0; i < slots->slot_count(); ++i) {
            const auto index = sample_slot_state_index(slots->slot_state(i));
            if (index < snapshot.slot_state_counts.size()) {
                ++snapshot.slot_state_counts[index];
            }
        }
        const auto published = slots->read_published_view();
        snapshot.published_sample_valid = published.valid;
        snapshot.published_slot_index = published.slot_index;
        snapshot.active_generation = published.generation;
        snapshot.published_sample_frames = published.num_frames;
        snapshot.published_sample_channels = published.num_channels;
        snapshot.published_sample_rate = published.sample_rate;
    }

    if (loop != nullptr) {
        snapshot.loop_rendered_frames = loop->rendered_frames;
        snapshot.loop_silent_frames = loop->silent_frames;
        snapshot.loop_active = loop->active;
        snapshot.loop_wrapped = loop->wrapped;
        snapshot.loop_max_sample_delta = loop->max_sample_delta;
    }

    if (onsets != nullptr) snapshot.onset_count = onsets->markers.size();
    if (slices != nullptr) snapshot.slice_count = slices->markers.size();
    return snapshot;
}

}  // namespace pulp::audio

