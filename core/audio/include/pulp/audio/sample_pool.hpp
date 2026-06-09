#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include <pulp/audio/published_sample_store.hpp>

namespace pulp::audio {

inline constexpr std::uint32_t kInvalidSamplePoolId =
    std::numeric_limits<std::uint32_t>::max();

struct SamplePoolEntry {
    std::uint32_t sample_id = kInvalidSamplePoolId;
    const PublishedSampleStore* store = nullptr;
    PublishedSampleView view{};
};

struct SamplePoolResolution {
    bool valid = false;
    std::uint32_t sample_id = kInvalidSamplePoolId;
    std::uint32_t entry_index = 0;
    const PublishedSampleStore* store = nullptr;
    PublishedSampleView view{};
};

class SamplePool {
public:
    SamplePool() = default;

    static bool entry_valid(const SamplePoolEntry& entry) noexcept;

    // Control/background-thread only. Copies and may allocate. Entries borrow
    // PublishedSampleStore instances; owners must keep those stores alive,
    // prepared, and not destructively reset/released/reprepared while any
    // realtime reader can resolve from this pool. Replace sample contents via
    // generation-safe publication, then publish a new pool snapshot when the
    // pool should resolve to a new view.
    bool configure(std::span<const SamplePoolEntry> entries);
    void clear() noexcept;

    std::span<const SamplePoolEntry> entries() const noexcept { return entries_; }
    bool empty() const noexcept { return entries_.empty(); }
    std::size_t size() const noexcept { return entries_.size(); }

    // RT-safe when the pool is immutable for the duration of the call and the
    // borrowed PublishedSampleStore lifetimes are externally guaranteed.
    SamplePoolResolution resolve(std::uint32_t sample_id) const noexcept;

    static const float* channel_data(const SamplePoolResolution& resolution,
                                     std::uint32_t channel) noexcept;
    static bool populate_channel_ptrs(const SamplePoolResolution& resolution,
                                      const float** channel_ptrs,
                                      std::size_t channel_capacity) noexcept;

private:
    std::vector<SamplePoolEntry> entries_;
};

}  // namespace pulp::audio
