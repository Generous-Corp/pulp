#include <pulp/audio/sample_pool.hpp>

#include <limits>

namespace pulp::audio {

bool SamplePool::entry_valid(const SamplePoolEntry& entry) noexcept {
    return entry.sample_id != kInvalidSamplePoolId &&
           entry.store != nullptr &&
           entry.view.valid &&
           entry.store->slot_view_valid(entry.view);
}

bool SamplePool::configure(std::span<const SamplePoolEntry> entries) {
    if (entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!entry_valid(entries[i])) return false;
        for (std::size_t j = i + 1; j < entries.size(); ++j) {
            if (entries[i].sample_id == entries[j].sample_id) {
                return false;
            }
        }
    }

    entries_.assign(entries.begin(), entries.end());
    return true;
}

void SamplePool::clear() noexcept {
    entries_.clear();
}

SamplePoolResolution SamplePool::resolve(std::uint32_t sample_id) const noexcept {
    SamplePoolResolution result;
    if (sample_id == kInvalidSamplePoolId) return result;

    const auto span = entries();
    for (std::uint32_t index = 0; index < span.size(); ++index) {
        const auto& entry = span[index];
        if (entry.sample_id != sample_id) continue;
        if (!entry_valid(entry)) return result;

        result.valid = true;
        result.sample_id = sample_id;
        result.entry_index = index;
        result.store = entry.store;
        result.view = entry.view;
        return result;
    }

    return result;
}

const float* SamplePool::channel_data(const SamplePoolResolution& resolution,
                                      std::uint32_t channel) noexcept {
    if (!resolution.valid || resolution.store == nullptr) return nullptr;
    return resolution.store->channel_data(resolution.view, channel);
}

bool SamplePool::populate_channel_ptrs(const SamplePoolResolution& resolution,
                                       const float** channel_ptrs,
                                       std::size_t channel_capacity) noexcept {
    if (!resolution.valid || resolution.store == nullptr) return false;
    return resolution.store->populate_channel_ptrs(resolution.view,
                                                  channel_ptrs,
                                                  channel_capacity);
}

}  // namespace pulp::audio
