#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/sample_slot_bank.hpp>

namespace pulp::audio {

struct PublishedSampleStoreConfig {
    // The store-level replacement path needs one currently published slot and
    // one free slot for the next sample. Use SampleSlotBank directly for
    // specialized one-slot/offline materialization workflows.
    std::uint32_t slot_count = 0;
    std::uint32_t max_channels = 0;
    std::uint64_t max_frames_per_slot = 0;
};

// Reusable controller-side sample publication helper over SampleSlotBank.
//
// prepare() allocates fixed storage before audio starts. load_*() and publish()
// may allocate/copy and take a mutex, so call them only from a controller,
// importer, or background owner thread. Audio callbacks should only read
// PublishedSampleView snapshots and generation-aware channel pointers.
class PublishedSampleStore {
public:
    PublishedSampleStore() = default;

    bool prepare(const PublishedSampleStoreConfig& config);
    void release() noexcept;

    bool prepared() const noexcept { return storage_ready_; }
    std::uint32_t slot_count() const noexcept { return slots_.slot_count(); }
    std::uint32_t max_channels() const noexcept { return slots_.max_channels(); }
    std::uint64_t max_frames_per_slot() const noexcept {
        return slots_.max_frames_per_slot();
    }

    bool load_mono(const float* data,
                   int num_samples,
                   double sample_rate,
                   std::uint64_t audio_safe_generation = 0);
    bool load_interleaved_stereo(const float* interleaved,
                                 int num_frames,
                                 double sample_rate,
                                 std::uint64_t audio_safe_generation = 0);
    bool publish(BufferView<const float> source,
                 std::uint64_t frames,
                 double sample_rate,
                 std::uint64_t audio_safe_generation = 0);

    bool has_sample() const noexcept { return slots_.read_published_view().valid; }
    int sample_length() const noexcept;

    PublishedSampleView read_published_view() const noexcept {
        return slots_.read_published_view();
    }
    bool slot_view_valid(const PublishedSampleView& view) const noexcept {
        return slots_.slot_view_valid(view);
    }
    const float* channel_data(const PublishedSampleView& view,
                              std::uint32_t channel) const noexcept {
        return slots_.channel_data(view, channel);
    }
    bool populate_channel_ptrs(const PublishedSampleView& view,
                               const float** channel_ptrs,
                               std::size_t channel_capacity) const noexcept;

private:
    bool config_valid(const PublishedSampleStoreConfig& config) const noexcept;
    bool ensure_capacity(const PublishedSampleStoreConfig& config);
    bool publish_locked(BufferView<const float> source,
                        std::uint64_t frames,
                        double sample_rate,
                        std::uint64_t audio_safe_generation);

    SampleSlotBank slots_;
    std::mutex load_mutex_;
    std::vector<float> load_scratch_;
    PublishedSampleStoreConfig config_;
    bool storage_ready_ = false;
};

}  // namespace pulp::audio
