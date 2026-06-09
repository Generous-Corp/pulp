#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/seqlock.hpp>

namespace pulp::audio {

class RollingAudioCaptureBuffer;

struct RollingAudioCaptureBufferConfig {
    std::uint32_t num_channels = 0;
    std::uint64_t max_frames = 0;
    std::uint64_t bytes_per_sample = sizeof(float);
};

struct RollingAudioCapturePrepareResult {
    bool ok = false;
    std::uint64_t requested_frames = 0;
    std::uint64_t allocated_frames = 0;
    std::uint64_t allocated_bytes = 0;
};

struct RollingAudioCaptureSnapshot {
    bool valid = false;
    std::uint64_t generation = 0;
    std::uint32_t num_channels = 0;
    std::uint64_t capacity_frames = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t frame_count = 0;
    std::uint64_t end_frame = 0;
};

enum class RollingAudioCaptureMaterializeStatus : std::uint8_t {
    Ok,
    InvalidSnapshot,
    Overwritten,
    DestinationTooSmall,
};

struct RollingAudioCaptureMaterializeResult {
    RollingAudioCaptureMaterializeStatus status =
        RollingAudioCaptureMaterializeStatus::InvalidSnapshot;
    std::uint64_t frames_copied = 0;
};

class RollingAudioCaptureHold {
public:
    RollingAudioCaptureHold() = default;
    RollingAudioCaptureHold(const RollingAudioCaptureHold&) = delete;
    RollingAudioCaptureHold& operator=(const RollingAudioCaptureHold&) = delete;
    RollingAudioCaptureHold(RollingAudioCaptureHold&& other) noexcept;
    RollingAudioCaptureHold& operator=(RollingAudioCaptureHold&& other) noexcept;
    ~RollingAudioCaptureHold() noexcept;

    bool valid() const noexcept { return buffer_ != nullptr && snapshot_.valid; }
    const RollingAudioCaptureSnapshot& snapshot() const noexcept { return snapshot_; }
    void release() noexcept;

private:
    friend class RollingAudioCaptureBuffer;

    RollingAudioCaptureHold(RollingAudioCaptureBuffer& buffer,
                            RollingAudioCaptureSnapshot snapshot,
                            std::uint64_t hold_id) noexcept;

    RollingAudioCaptureBuffer* buffer_ = nullptr;
    RollingAudioCaptureSnapshot snapshot_;
    std::uint64_t hold_id_ = 0;
};

class RollingAudioCaptureBuffer {
public:
    RollingAudioCaptureBuffer() = default;

    bool prepare(const RollingAudioCaptureBufferConfig& config);
    RollingAudioCapturePrepareResult prepare_seconds(std::uint32_t num_channels,
                                                     double sample_rate,
                                                     double seconds,
                                                     std::uint64_t max_bytes = 0);
    void reset() noexcept;

    std::uint32_t num_channels() const noexcept { return num_channels_; }
    std::uint64_t capacity_frames() const noexcept { return capacity_frames_; }
    std::uint64_t allocated_bytes() const noexcept;
    std::uint64_t frames_discarded_while_held() const noexcept {
        return frames_discarded_while_held_.load(std::memory_order_relaxed);
    }

    void append(BufferView<const float> source, std::uint64_t frames) noexcept;
    void append(BufferView<float> source, std::uint64_t frames) noexcept;

    RollingAudioCaptureSnapshot snapshot_last(std::uint64_t frames) const noexcept;

    // Live-freeze protocol. hold_last() is the preferred controller/owner
    // shape: the returned token releases the held storage in its destructor.
    // begin_hold_last(), end_hold(), append(), prepare(), and reset() remain
    // capture-owner operations for lower-level/manual integrations; call them
    // from the owner that calls append(), typically the audio thread at a block
    // boundary. While the hold is active, append() is a no-op and incoming
    // frames are intentionally discarded so off-RT materialization can copy the
    // held snapshot without racing rolling storage overwrite. The discarded
    // frame count is observable through frames_discarded_while_held().
    RollingAudioCaptureHold hold_last(std::uint64_t frames) noexcept;
    RollingAudioCaptureSnapshot begin_hold_last(std::uint64_t frames) noexcept;
    bool hold_active() const noexcept;
    void end_hold() noexcept;
    RollingAudioCaptureMaterializeResult materialize_held(
        const RollingAudioCaptureHold& hold,
        BufferView<float> destination) const noexcept;
    RollingAudioCaptureMaterializeResult materialize_held(
        const RollingAudioCaptureSnapshot& snapshot,
        BufferView<float> destination) const noexcept;

    // Off-real-time helper. The capture buffer must be paused/quiescent or
    // externally protected while materializing; the descriptor alone does not
    // pin the rolling storage against concurrent overwrite.
    RollingAudioCaptureMaterializeResult materialize_quiescent(
        const RollingAudioCaptureSnapshot& snapshot,
        BufferView<float> destination) const noexcept;
    [[deprecated("Use materialize_quiescent(); this path requires paused/external synchronization")]]
    RollingAudioCaptureMaterializeResult materialize(
        const RollingAudioCaptureSnapshot& snapshot,
        BufferView<float> destination) const noexcept;

    static std::uint64_t estimate_bytes(std::uint32_t num_channels,
                                        std::uint64_t frames,
                                        std::uint64_t bytes_per_sample = sizeof(float)) noexcept;

private:
    friend class RollingAudioCaptureHold;

    struct CursorState {
        std::uint64_t total_written = 0;
        std::uint64_t write_pos = 0;
        std::uint64_t generation = 0;
    };

    void clear_unprepared() noexcept;
    void advance_generation() noexcept;
    std::uint64_t next_hold_id() noexcept;
    void end_hold_if_current(const RollingAudioCaptureSnapshot& snapshot,
                             std::uint64_t hold_id) noexcept;

    template<typename SampleType>
    void append_impl(BufferView<SampleType> source, std::uint64_t frames) noexcept;

    bool snapshot_available(const RollingAudioCaptureSnapshot& snapshot,
                            CursorState state) const noexcept;
    float* channel_storage(std::uint32_t channel) noexcept;
    const float* channel_storage(std::uint32_t channel) const noexcept;

    std::vector<float> storage_;
    std::uint32_t num_channels_ = 0;
    std::uint64_t capacity_frames_ = 0;
    std::uint64_t write_pos_ = 0;
    std::uint64_t total_written_ = 0;
    std::uint64_t generation_ = 1;
    RollingAudioCaptureSnapshot held_snapshot_;
    std::uint64_t active_hold_id_ = 0;
    std::uint64_t next_hold_id_ = 1;
    std::atomic<std::uint64_t> frames_discarded_while_held_{0};
    std::atomic_bool hold_active_{false};
    runtime::SeqLock<CursorState> state_;
};

}  // namespace pulp::audio
