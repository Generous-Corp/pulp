#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>

namespace pulp::audio {

struct SampleStreamWindowConfig {
    std::uint32_t channels = 0;
    std::uint32_t page_count = 0;
    std::uint64_t page_frames = 0;
};

enum class SampleStreamPageState : std::uint8_t {
    Empty,
    Filling,
    Ready,
    Retiring,
    Retired,
};

struct SampleStreamPageDescriptor {
    // Nonzero identity for one logical streamed sample. Reads are always
    // generation-qualified so old pages cannot satisfy a new sample request.
    std::uint64_t stream_generation = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t valid_frames = 0;
    bool final_page = false;
};

struct SampleStreamPageView {
    bool valid = false;
    std::uint32_t page_index = 0;
    std::uint64_t stream_generation = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t valid_frames = 0;
    std::uint64_t local_offset = 0;
    bool final_page = false;
};

struct SampleStreamWindowReadRequest {
    std::uint64_t stream_generation = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t frames = 0;
    bool accumulate = false;
    bool zero_fill_misses = true;
};

struct SampleStreamWindowReadResult {
    std::uint64_t requested_frames = 0;
    std::uint64_t copied_frames = 0;
    std::uint64_t missed_frames = 0;
    std::uint64_t zero_filled_frames = 0;
    bool complete = false;
};

struct SampleStreamWindowStats {
    std::uint64_t pages_published = 0;
    std::uint64_t pages_retired = 0;
    std::uint64_t ready_read_chunks = 0;
    std::uint64_t ready_frames_read = 0;
    std::uint64_t missed_frames = 0;
    std::uint64_t zero_filled_frames = 0;
    std::uint32_t ready_pages = 0;
};

// Prepared planar page storage for storage-backed sample streaming.
//
// This is a data-plane primitive only: it owns fixed page storage and page
// metadata, but it does not own files, decoders, worker threads, prefetch
// policy, voice state, or loop rendering. Background/importer code fills and
// publishes pages; audio code performs generation-qualified ready-page lookup
// and reads already-resident data only.
class SampleStreamWindow {
public:
    SampleStreamWindow() = default;

    // Offline/quiescent only. Allocates all page storage and metadata. A failed
    // prepare releases any previously prepared storage.
    bool prepare(const SampleStreamWindowConfig& config);
    // Offline/quiescent only; not safe while readers or publishers are live.
    void release() noexcept;
    // Quiescent/control-thread only. Clears pages and counters without freeing
    // prepared storage.
    void reset_when_quiescent() noexcept;

    bool prepared() const noexcept { return prepared_; }
    std::uint32_t channels() const noexcept { return channels_; }
    std::uint32_t page_count() const noexcept { return page_count_; }
    std::uint64_t page_frames() const noexcept { return page_frames_; }

    SampleStreamPageState page_state(std::uint32_t page_index) const noexcept;

    // Control/background-thread fill lifecycle. Single publisher ownership is
    // expected. A Ready page must be retired before it can be filled again, and
    // retired pages can only be reused once the caller reports an audio-safe
    // generation at or beyond the page's retire-after generation.
    bool begin_fill_page(std::uint32_t page_index,
                         std::uint64_t completed_audio_generation = 0) noexcept;
    float* writable_channel_data(std::uint32_t page_index,
                                 std::uint32_t channel) noexcept;
    bool copy_to_filling_page(std::uint32_t page_index,
                              BufferView<const float> source,
                              std::uint64_t frames) noexcept;
    bool copy_to_filling_page(std::uint32_t page_index,
                              BufferView<float> source,
                              std::uint64_t frames) noexcept;
    bool publish_page(std::uint32_t page_index,
                      const SampleStreamPageDescriptor& descriptor) noexcept;
    bool cancel_fill_page(std::uint32_t page_index) noexcept;
    bool retire_page(std::uint32_t page_index,
                     std::uint64_t retire_after_audio_generation) noexcept;

    // RT-safe after prepare when page lifetimes are externally generation-gated.
    // Lookup is a bounded linear scan over the prepared page window; keep
    // page_count small until a higher-level indexed streaming cache exists.
    SampleStreamPageView ready_page_for_frame(
        std::uint64_t stream_generation,
        std::uint64_t source_frame) const noexcept;
    const float* ready_channel_data(const SampleStreamPageView& view,
                                    std::uint32_t channel) const noexcept;
    SampleStreamWindowReadResult read_frames(
        BufferView<float> destination,
        const SampleStreamWindowReadRequest& request) noexcept;
    SampleStreamWindowStats stats() const noexcept;

private:
    template<typename SampleType>
    bool copy_to_filling_page_impl(std::uint32_t page_index,
                                   BufferView<SampleType> source,
                                   std::uint64_t frames) noexcept;

    bool valid_page(std::uint32_t page_index) const noexcept;
    bool descriptor_valid(const SampleStreamPageDescriptor& descriptor) const noexcept;
    bool overlaps_ready_page(std::uint32_t page_index,
                             const SampleStreamPageDescriptor& descriptor) const noexcept;
    float* mutable_page_channel(std::uint32_t page_index,
                                std::uint32_t channel) noexcept;
    const float* page_channel(std::uint32_t page_index,
                              std::uint32_t channel) const noexcept;
    void clear_page(std::uint32_t page_index) noexcept;
    void clear_unprepared() noexcept;
    std::uint64_t frames_until_next_ready_page(std::uint64_t stream_generation,
                                               std::uint64_t source_frame,
                                               std::uint64_t limit) const noexcept;

    struct PageMetadata {
        SampleStreamPageDescriptor descriptor{};
    };

    std::vector<float> storage_;
    std::vector<std::atomic<std::uint8_t>> states_;
    std::vector<PageMetadata> metadata_;
    std::vector<std::atomic<std::uint64_t>> retire_after_generation_;

    std::uint32_t channels_ = 0;
    std::uint32_t page_count_ = 0;
    std::uint64_t page_frames_ = 0;
    bool prepared_ = false;

    std::atomic<std::uint64_t> pages_published_{0};
    std::atomic<std::uint64_t> pages_retired_{0};
    std::atomic<std::uint64_t> ready_read_chunks_{0};
    std::atomic<std::uint64_t> ready_frames_read_{0};
    std::atomic<std::uint64_t> missed_frames_{0};
    std::atomic<std::uint64_t> zero_filled_frames_{0};
};

}  // namespace pulp::audio
