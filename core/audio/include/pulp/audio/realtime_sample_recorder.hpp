#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/rolling_audio_capture_buffer.hpp>
#include <pulp/runtime/spsc_queue.hpp>

namespace pulp::audio {

enum class RealtimeSampleRecorderState : std::uint8_t {
    Idle,
    Armed,
    Recording,
    Completing,
    Completed,
    Cancelled,
    Failed,
};

enum class RealtimeSampleRecorderCommandType : std::uint8_t {
    Arm,
    Start,
    Stop,
    Cancel,
    Reset,
};

enum class RealtimeSampleRecorderTimingType : std::uint8_t {
    Immediate,
    BlockOffset,
};

enum class RealtimeSampleRecorderEventType : std::uint8_t {
    CommandAccepted,
    CommandRejected,
    Started,
    Completed,
    Cancelled,
    Failed,
};

struct RealtimeSampleRecorderTiming {
    RealtimeSampleRecorderTimingType type = RealtimeSampleRecorderTimingType::Immediate;
    std::uint32_t frame_offset = 0;
};

struct RealtimeSampleRecorderCommand {
    std::uint64_t sequence_id = 0;
    RealtimeSampleRecorderCommandType type = RealtimeSampleRecorderCommandType::Start;
    RealtimeSampleRecorderTiming timing;
    std::uint64_t max_frames = 0;
    bool cancel_on_transport_jump = true;
};

struct RealtimeSampleRecorderEvent {
    std::uint64_t sequence_id = 0;
    RealtimeSampleRecorderEventType type = RealtimeSampleRecorderEventType::CommandAccepted;
    RealtimeSampleRecorderState state = RealtimeSampleRecorderState::Idle;
    std::uint64_t frames_recorded = 0;
    std::uint32_t block_offset = 0;
};

struct RealtimeSampleRecorderConfig {
    std::uint32_t num_channels = 0;
    std::uint64_t max_frames = 0;
    double sample_rate = 0.0;
};

struct RealtimeSampleRecorderStats {
    std::uint64_t frames_recorded = 0;
    std::uint64_t total_recorded_frames = 0;
    std::uint64_t dropped_commands = 0;
    std::uint64_t dropped_events = 0;
};

class RealtimeSampleRecorder {
public:
    static constexpr std::size_t kCommandQueueCapacity = 32;
    static constexpr std::size_t kEventQueueCapacity = 64;

    RealtimeSampleRecorder() = default;

    bool prepare(const RealtimeSampleRecorderConfig& config);
    // Offline-only. Do not call concurrently with process(); use a queued
    // Reset command for live control while the audio callback is active.
    void release() noexcept;
    // Offline-only. Do not call concurrently with process(); use a queued
    // Reset command for live control while the audio callback is active.
    void reset() noexcept;

    bool enqueue_command(const RealtimeSampleRecorderCommand& command) noexcept;
    bool pop_event(RealtimeSampleRecorderEvent& event) noexcept;

    void process(BufferView<const float> source,
                 std::uint32_t frames,
                 bool transport_jumped = false) noexcept;
    void process(BufferView<float> source,
                 std::uint32_t frames,
                 bool transport_jumped = false) noexcept;

    // Off-real-time helper for paused/external-synchronized rolling capture.
    // The RollingAudioCaptureSnapshot descriptor does not pin live rolling
    // storage against concurrent append().
    bool materialize_snapshot_quiescent(
        const RollingAudioCaptureBuffer& capture,
        const RollingAudioCaptureSnapshot& snapshot) noexcept;
    // Off-real-time helper for snapshots pinned by
    // RollingAudioCaptureBuffer::begin_hold_last(). The capture hold must stay
    // active until this call returns.
    bool materialize_held_snapshot(
        const RollingAudioCaptureBuffer& capture,
        const RollingAudioCaptureSnapshot& snapshot) noexcept;
    [[deprecated("Use materialize_snapshot_quiescent() or materialize_held_snapshot()")]]
    bool materialize_snapshot(const RollingAudioCaptureBuffer& capture,
                              const RollingAudioCaptureSnapshot& snapshot) noexcept;
    // Off-real-time. Marks completed data as consumed after an external
    // materialization path has copied it. Do not call concurrently with process().
    bool consume_completed_recording() noexcept;

    RealtimeSampleRecorderState state() const noexcept;
    std::uint32_t num_channels() const noexcept { return num_channels_; }
    std::uint64_t max_frames() const noexcept { return max_frames_; }
    std::uint64_t frames_recorded() const noexcept;
    double sample_rate() const noexcept { return sample_rate_; }
    // Valid after a Completed/Cancelled event or external synchronization.
    // Do not read concurrently with process() while a new recording is active.
    const float* channel_data(std::uint32_t channel) const noexcept;
    RealtimeSampleRecorderStats stats() const noexcept;

private:
    struct TimedCommand {
        RealtimeSampleRecorderCommand command;
        std::uint32_t offset = 0;
        std::uint32_t order = 0;
    };

    template<typename SampleType>
    void process_impl(BufferView<SampleType> source,
                      std::uint32_t frames,
                      bool transport_jumped) noexcept;

    template<typename SampleType>
    void copy_span(BufferView<SampleType> source,
                   std::uint32_t source_offset,
                   std::uint32_t frames) noexcept;

    bool materialize_snapshot_impl(const RollingAudioCaptureBuffer& capture,
                                   const RollingAudioCaptureSnapshot& snapshot,
                                   bool held) noexcept;

    bool valid_config(const RealtimeSampleRecorderConfig& config) const noexcept;
    void clear_storage() noexcept;
    void push_event(const RealtimeSampleRecorderEvent& event) noexcept;
    void push_simple_event(std::uint64_t sequence_id,
                           RealtimeSampleRecorderEventType type,
                           std::uint32_t block_offset) noexcept;
    void set_state(RealtimeSampleRecorderState state) noexcept;
    void publish_frames_recorded() noexcept;
    bool collect_commands(std::array<TimedCommand, kCommandQueueCapacity>& commands,
                          std::uint32_t& count,
                          std::uint32_t block_frames) noexcept;
    void sort_commands(std::array<TimedCommand, kCommandQueueCapacity>& commands,
                       std::uint32_t count) noexcept;
    void apply_command(const RealtimeSampleRecorderCommand& command,
                       std::uint32_t block_offset) noexcept;
    void begin_recording(const RealtimeSampleRecorderCommand& command,
                         std::uint32_t block_offset) noexcept;
    void complete_recording(std::uint64_t sequence_id, std::uint32_t block_offset) noexcept;
    void cancel_recording(std::uint64_t sequence_id, std::uint32_t block_offset) noexcept;
    float* mutable_channel_data(std::uint32_t channel) noexcept;

    runtime::SpscQueue<RealtimeSampleRecorderCommand, kCommandQueueCapacity> command_queue_;
    runtime::SpscQueue<RealtimeSampleRecorderEvent, kEventQueueCapacity> event_queue_;

    std::vector<float> storage_;
    std::vector<float*> mutable_ptrs_;

    RealtimeSampleRecorderState state_ = RealtimeSampleRecorderState::Idle;
    std::uint32_t num_channels_ = 0;
    std::uint64_t max_frames_ = 0;
    std::uint64_t target_frames_ = 0;
    std::uint64_t frames_recorded_ = 0;
    std::uint64_t active_sequence_id_ = 0;
    double sample_rate_ = 0.0;
    bool cancel_on_transport_jump_ = true;
    bool prepared_ = false;

    std::atomic<std::uint8_t> state_snapshot_{
        static_cast<std::uint8_t>(RealtimeSampleRecorderState::Idle)};
    std::atomic<std::uint64_t> frames_recorded_snapshot_{0};
    std::atomic<std::uint64_t> total_recorded_frames_{0};
    std::atomic<std::uint64_t> dropped_commands_{0};
    std::atomic<std::uint64_t> dropped_events_{0};
};

}  // namespace pulp::audio
