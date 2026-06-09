#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/runtime/seqlock.hpp>

namespace pulp::audio {

enum class SampleSlotState : std::uint8_t {
    Free,
    Reserved,
    Writing,
    Completed,
    Published,
    Retired,
};

struct PublishedSampleView {
    bool valid = false;
    std::uint32_t slot_index = 0;
    std::uint64_t generation = 0;
    std::uint32_t num_channels = 0;
    std::uint64_t num_frames = 0;
    double sample_rate = 0.0;
};

// Fixed-capacity sample publication bank for RT readers.
//
// Publication/lifecycle mutation is single-owner: callers must externally
// serialize reserve, write, complete, publish, release, reset,
// acknowledge_audio_generation, and publish_from_buffer calls. Audio threads
// only read PublishedSampleView snapshots and generation-aware channel pointers.
class SampleSlotBank {
public:
    SampleSlotBank() = default;

    // Offline/quiescent only. Allocates fixed slot storage; live replacement
    // should publish into free slots and use generation acknowledgements.
    bool prepare(std::uint32_t slot_count,
                 std::uint32_t max_channels,
                 std::uint64_t max_frames_per_slot);
    // Offline/quiescent only; not safe while audio readers or publishers are live.
    void reset() noexcept;

    std::uint32_t slot_count() const noexcept { return slot_count_; }
    std::uint32_t max_channels() const noexcept { return max_channels_; }
    std::uint64_t max_frames_per_slot() const noexcept { return max_frames_per_slot_; }

    // Single-publisher control/background-thread publication path. Audio
    // readers should use PublishedSampleView plus generation-aware channel_data().
    int reserve_slot(std::uint32_t num_channels,
                     std::uint64_t num_frames,
                     double sample_rate) noexcept;

    bool write_slot(std::uint32_t slot_index,
                    BufferView<const float> source,
                    std::uint64_t frames) noexcept;
    bool write_slot(std::uint32_t slot_index,
                    BufferView<float> source,
                    std::uint64_t frames) noexcept;

    // Single-publisher control/background-thread helper for the normal
    // publication path.
    // Acknowledges the oldest audio-held generation first, then reserves,
    // writes, completes, and publishes one free slot. Keep low-level calls
    // for tests and specialized materializers that need explicit state checks.
    bool publish_from_buffer(BufferView<const float> source,
                             std::uint64_t frames,
                             double sample_rate,
                             std::uint64_t audio_safe_generation = 0) noexcept;
    bool publish_from_buffer(BufferView<float> source,
                             std::uint64_t frames,
                             double sample_rate,
                             std::uint64_t audio_safe_generation = 0) noexcept;

    bool complete_slot(std::uint32_t slot_index) noexcept;
    bool publish_slot(std::uint32_t slot_index) noexcept;
    bool release_slot(std::uint32_t slot_index) noexcept;

    PublishedSampleView read_published_view() const noexcept { return published_.read(); }
    // Publisher-side acknowledgement of the oldest audio-held generation. Do
    // not call directly from process(); feed the audio watermark to the
    // serialized publication owner.
    bool acknowledge_audio_generation(std::uint64_t generation) noexcept;

    SampleSlotState slot_state(std::uint32_t slot_index) const noexcept;
    std::uint32_t slot_num_channels(std::uint32_t slot_index) const noexcept;
    std::uint64_t slot_num_frames(std::uint32_t slot_index) const noexcept;
    double slot_sample_rate(std::uint32_t slot_index) const noexcept;
    bool slot_view_valid(const PublishedSampleView& view) const noexcept;
    const float* channel_data(const PublishedSampleView& view,
                              std::uint32_t channel) const noexcept;
    static std::uint64_t oldest_active_generation(
        const PublishedSampleView& current,
        const PublishedSampleView* active_views,
        std::size_t active_count) noexcept;
    // Controlled diagnostics/tests for manually managed slots. Published audio
    // readers should use the generation-aware overload above.
    const float* slot_channel_data_for_test(std::uint32_t slot_index,
                                            std::uint32_t channel) const noexcept;
    [[deprecated("Use channel_data(PublishedSampleView, channel) for audio reads; raw slot access is test/manual-only")]]
    const float* channel_data(std::uint32_t slot_index, std::uint32_t channel) const noexcept;

private:
    struct SlotMetadata {
        std::uint32_t num_channels = 0;
        std::uint64_t num_frames = 0;
        double sample_rate = 0.0;
    };

    template<typename SampleType>
    bool write_slot_impl(std::uint32_t slot_index,
                         BufferView<SampleType> source,
                         std::uint64_t frames) noexcept;
    template<typename SampleType>
    bool publish_from_buffer_impl(BufferView<SampleType> source,
                                  std::uint64_t frames,
                                  double sample_rate,
                                  std::uint64_t audio_safe_generation) noexcept;

    bool valid_slot(std::uint32_t slot_index) const noexcept;
    float* mutable_channel_data(std::uint32_t slot_index, std::uint32_t channel) noexcept;
    const float* slot_channel_data_unchecked(std::uint32_t slot_index,
                                             std::uint32_t channel) const noexcept;
    void clear_metadata(std::uint32_t slot_index) noexcept;

    std::vector<float> storage_;
    std::vector<std::atomic<std::uint8_t>> states_;
    std::vector<std::atomic_bool> write_committed_;
    std::vector<SlotMetadata> metadata_;
    std::vector<std::atomic<std::uint64_t>> retire_after_generation_;
    runtime::SeqLock<PublishedSampleView> published_;
    PublishedSampleView current_;
    std::uint32_t slot_count_ = 0;
    std::uint32_t max_channels_ = 0;
    std::uint64_t max_frames_per_slot_ = 0;
    std::uint64_t publish_generation_ = 0;

    void clear_unprepared() noexcept;
};

}  // namespace pulp::audio
