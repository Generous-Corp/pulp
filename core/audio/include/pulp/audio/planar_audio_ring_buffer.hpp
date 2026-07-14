#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/spsc_ring_index.hpp>

namespace pulp::audio {

struct PlanarAudioRingBufferStats {
    std::uint64_t underrun_frames = 0;
    std::uint64_t overrun_frames = 0;
    std::uint64_t dropped_write_frames = 0;
};

class PlanarAudioRingBuffer {
public:
    PlanarAudioRingBuffer() = default;

    // Offline/quiescent only. Allocates fixed storage and FIFO state.
    bool prepare(std::uint32_t num_channels, std::uint64_t usable_capacity_frames);
    void release() noexcept;
    void reset() noexcept;

    std::uint32_t num_channels() const noexcept { return num_channels_; }
    std::uint64_t capacity_frames() const noexcept { return usable_capacity_frames_; }
    std::uint64_t available_frames() const noexcept;
    std::uint64_t free_frames() const noexcept;
    PlanarAudioRingBufferStats stats() const noexcept;

    // RT-safe after prepare for one producer and one consumer. Short reads
    // zero-fill and report underrun stats; short writes drop excess frames.
    std::uint64_t write(BufferView<const float> source, std::uint64_t frames) noexcept {
        return write_impl(source, frames);
    }

    std::uint64_t write(BufferView<float> source, std::uint64_t frames) noexcept {
        return write_impl(source, frames);
    }

    bool read(BufferView<float> destination, std::uint64_t frames) noexcept;
    std::uint64_t drain(std::uint64_t max_frames) noexcept;

private:
    void clear_unprepared() noexcept;
    static bool valid_capacity(std::uint64_t usable_capacity_frames) noexcept;
    static int clamp_to_fifo_count(std::uint64_t frames) noexcept;
    float* channel_storage(std::uint32_t channel) noexcept;
    const float* channel_storage(std::uint32_t channel) const noexcept;

    template<typename SampleType>
    void copy_source_segment(BufferView<SampleType> source,
                             int destination_start,
                             std::uint64_t source_offset,
                             int frame_count) noexcept;

    template<typename SampleType>
    std::uint64_t write_impl(BufferView<SampleType> source, std::uint64_t frames) noexcept;

    std::vector<float> storage_;
    std::unique_ptr<runtime::SpscRingIndex> fifo_;
    std::uint32_t num_channels_ = 0;
    std::uint64_t usable_capacity_frames_ = 0;
    std::uint64_t internal_capacity_frames_ = 0;

    std::atomic<std::uint64_t> underrun_frames_{0};
    std::atomic<std::uint64_t> overrun_frames_{0};
    std::atomic<std::uint64_t> dropped_write_frames_{0};
};

template<typename SampleType>
void PlanarAudioRingBuffer::copy_source_segment(BufferView<SampleType> source,
                                                int destination_start,
                                                std::uint64_t source_offset,
                                                int frame_count) noexcept {
    static_assert(std::is_same_v<std::remove_const_t<SampleType>, float>,
                  "PlanarAudioRingBuffer only supports float sample views");

    if (frame_count <= 0) return;

    for (std::uint32_t ch = 0; ch < num_channels_; ++ch) {
        float* destination = channel_storage(ch) + destination_start;
        if (ch < source.num_channels()) {
            const SampleType* source_ptr = source.channel_ptr(ch) + source_offset;
            std::copy_n(source_ptr, static_cast<std::size_t>(frame_count), destination);
        } else {
            std::fill_n(destination, static_cast<std::size_t>(frame_count), 0.0f);
        }
    }
}

template<typename SampleType>
std::uint64_t PlanarAudioRingBuffer::write_impl(BufferView<SampleType> source,
                                                std::uint64_t frames) noexcept {
    static_assert(std::is_same_v<std::remove_const_t<SampleType>, float>,
                  "PlanarAudioRingBuffer only supports float sample views");

    if (!fifo_ || num_channels_ == 0 || frames == 0 ||
        source.num_channels() == 0 || source.num_samples() == 0) {
        return 0;
    }

    const auto requested = std::min(frames, static_cast<std::uint64_t>(source.num_samples()));
    int start_1 = 0;
    int size_1 = 0;
    int start_2 = 0;
    int size_2 = 0;
    fifo_->prepare_to_write(clamp_to_fifo_count(requested), start_1, size_1, start_2, size_2);

    const auto written = static_cast<std::uint64_t>(size_1 + size_2);
    if (written == 0) {
        if (requested > 0) {
            overrun_frames_.fetch_add(requested, std::memory_order_relaxed);
            dropped_write_frames_.fetch_add(requested, std::memory_order_relaxed);
        }
        return 0;
    }

    copy_source_segment(source, start_1, 0, size_1);
    copy_source_segment(source, start_2, static_cast<std::uint64_t>(size_1), size_2);
    fifo_->finish_write(static_cast<int>(written));

    if (written < requested) {
        const auto dropped = requested - written;
        overrun_frames_.fetch_add(dropped, std::memory_order_relaxed);
        dropped_write_frames_.fetch_add(dropped, std::memory_order_relaxed);
    }

    return written;
}

}  // namespace pulp::audio
