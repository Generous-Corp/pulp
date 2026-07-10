#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/planar_audio_ring_buffer.hpp>
#include <pulp/runtime/abstract_fifo.hpp>
#include <pulp/signal/resampler.hpp>

namespace pulp::audio {

struct AudioStreamHandoffConfig {
    std::uint32_t source_channels = 0;
    std::uint32_t host_channels = 0;
    double source_sample_rate = 0.0;
    double host_sample_rate = 0.0;
    std::uint64_t ring_capacity_frames = 0;
    std::uint32_t max_source_block_frames = 0;
    std::uint32_t max_host_block_frames = 0;
};

struct AudioStreamHandoffPullResult {
    std::uint64_t requested_frames = 0;
    std::uint64_t rendered_frames = 0;
    // Host/output frames that were backed by source audio. In resampled
    // pulls this remains in host frames; source_frames_consumed reports the
    // source-domain accounting separately.
    std::uint64_t source_backed_frames = 0;
    std::uint64_t source_frames_consumed = 0;
    bool underrun = false;
};

struct AudioStreamHandoffStats {
    std::uint64_t source_frames_pushed = 0;
    std::uint64_t source_frames_consumed = 0;
    std::uint64_t output_frames_pulled = 0;
    std::uint64_t underrun_frames = 0;
    std::uint64_t overrun_frames = 0;
    std::uint64_t dropped_source_frames = 0;
    std::uint64_t queued_source_frames = 0;
    double queued_source_seconds = 0.0;
    double estimated_source_latency_frames = 0.0;
    double estimated_host_latency_frames = 0.0;
    double estimated_latency_seconds = 0.0;
};

class AudioStreamHandoff {
public:
    AudioStreamHandoff() = default;

    // Offline/quiescent only. Allocates ring, pending buffers, and resampler
    // state. `host_channels` records the intended host output shape; the
    // destination BufferView controls final channel adaptation in pull().
    bool prepare(const AudioStreamHandoffConfig& config);
    void reset() noexcept;

    bool prepared() const noexcept { return prepared_; }
    bool resampling() const noexcept { return prepared_ && !same_rate_; }
    std::uint32_t source_channels() const noexcept { return source_channels_; }
    std::uint32_t host_channels() const noexcept { return host_channels_; }
    double source_sample_rate() const noexcept { return source_sample_rate_; }
    double host_sample_rate() const noexcept { return host_sample_rate_; }

    // RT-safe after prepare for one source producer and one host consumer.
    // Pull preserves resampler fractional state and zero-fills underruns.
    std::uint64_t push(BufferView<const float> source, std::uint64_t frames) noexcept;
    std::uint64_t push(BufferView<float> source, std::uint64_t frames) noexcept;

    AudioStreamHandoffPullResult pull(BufferView<float> destination,
                                      std::uint64_t frames) noexcept;

    AudioStreamHandoffStats stats() const noexcept;

private:
    void clear_unprepared() noexcept;
    bool valid_config(const AudioStreamHandoffConfig& config) const noexcept;
    void allocate_scratch(std::uint64_t pending_capacity_frames);
    void refill_pending_from_ring() noexcept;
    void consume_pending(std::uint64_t frames) noexcept;
    struct ResampledChunkResult {
        std::uint64_t output_frames = 0;
        std::uint64_t source_frames_consumed = 0;
    };
    ResampledChunkResult pull_resampled_chunk(BufferView<float> destination,
                                              std::uint64_t frames) noexcept;
    void copy_resampled_to_destination(BufferView<float> destination,
                                       std::uint64_t destination_offset,
                                       std::uint64_t frames) noexcept;
    void zero_fill(BufferView<float> destination,
                   std::uint64_t offset,
                   std::uint64_t frames) noexcept;

    template<typename SampleType>
    std::uint64_t push_impl(BufferView<SampleType> source, std::uint64_t frames) noexcept;

    PlanarAudioRingBuffer ring_;
    signal::Resampler resampler_;

    std::vector<std::vector<float>> pending_source_;
    std::vector<std::vector<float>> resampled_output_;
    std::vector<float*> pending_write_ptrs_;
    std::vector<const float*> pending_read_ptrs_;
    std::vector<float*> resampled_output_ptrs_;
    std::unique_ptr<runtime::AbstractFifo> pending_fifo_;

    std::uint32_t source_channels_ = 0;
    std::uint32_t host_channels_ = 0;
    double source_sample_rate_ = 0.0;
    double host_sample_rate_ = 0.0;
    std::uint64_t pending_capacity_frames_ = 0;
    std::uint32_t max_host_block_frames_ = 0;
    bool prepared_ = false;
    bool same_rate_ = true;

    std::atomic<std::uint64_t> source_frames_pushed_{0};
    std::atomic<std::uint64_t> source_frames_consumed_{0};
    std::atomic<std::uint64_t> output_frames_pulled_{0};
    std::atomic<std::uint64_t> underrun_frames_{0};
    std::atomic<std::uint64_t> overrun_frames_{0};
    std::atomic<std::uint64_t> dropped_source_frames_{0};
    std::atomic<std::uint64_t> pending_source_frames_snapshot_{0};
};

template<typename SampleType>
std::uint64_t AudioStreamHandoff::push_impl(BufferView<SampleType> source,
                                            std::uint64_t frames) noexcept {
    if (!prepared_ || frames == 0 || source.num_channels() == 0 ||
        source.num_samples() == 0) {
        return 0;
    }

    const auto requested =
        std::min(frames, static_cast<std::uint64_t>(source.num_samples()));
    const auto written = ring_.write(source, requested);
    source_frames_pushed_.fetch_add(written, std::memory_order_relaxed);
    if (written < requested) {
        const auto dropped = requested - written;
        overrun_frames_.fetch_add(dropped, std::memory_order_relaxed);
        dropped_source_frames_.fetch_add(dropped, std::memory_order_relaxed);
    }
    return written;
}

}  // namespace pulp::audio
