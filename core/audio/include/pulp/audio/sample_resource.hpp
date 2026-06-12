#pragma once

#include <pulp/audio/audio_file.hpp>

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace pulp::audio {

enum class SampleResourceStatus : uint8_t {
    Empty,
    Loaded,
    Missing,
    Oversized,
};

struct SampleResourceSnapshot {
    SampleResourceStatus status = SampleResourceStatus::Empty;
    uint64_t generation = 0;
    uint64_t frame_count = 0;
    uint32_t channel_count = 0;
    uint32_t sample_rate = 0;
    uint64_t byte_size = 0;
    uint64_t memory_budget_bytes = 0;
    std::shared_ptr<const AudioFileData> data;

    bool ready() const noexcept {
        return status == SampleResourceStatus::Loaded && data && !data->empty();
    }
};

struct SampleResourceDiagnostics {
    SampleResourceStatus status = SampleResourceStatus::Empty;
    uint64_t generation = 0;
    std::string path;
    std::string reason;
    uint64_t frame_count = 0;
    uint32_t channel_count = 0;
    uint32_t sample_rate = 0;
    uint64_t byte_size = 0;
    uint64_t memory_budget_bytes = 0;
};

class SampleResourceHandle {
public:
    SampleResourceSnapshot snapshot() const noexcept { return snapshot_; }
    SampleResourceDiagnostics diagnostics() const { return diagnostics_; }

    void clear() {
        snapshot_ = {};
        diagnostics_ = {};
    }

    void publish_missing(std::string path, std::string reason) {
        const auto next_generation = snapshot_.generation + 1;
        snapshot_ = {
            .status = SampleResourceStatus::Missing,
            .generation = next_generation,
        };
        diagnostics_ = {
            .status = SampleResourceStatus::Missing,
            .generation = next_generation,
            .path = std::move(path),
            .reason = std::move(reason),
        };
    }

    bool publish_loaded(AudioFileData data,
                        std::string path,
                        uint64_t memory_budget_bytes = 0) {
        const auto byte_size = decoded_byte_size(data);
        const auto next_generation = snapshot_.generation + 1;
        if (memory_budget_bytes != 0 && byte_size > memory_budget_bytes) {
            snapshot_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .frame_count = data.num_frames(),
                .channel_count = data.num_channels(),
                .sample_rate = data.sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            diagnostics_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .path = std::move(path),
                .reason = "decoded sample exceeds memory budget",
                .frame_count = data.num_frames(),
                .channel_count = data.num_channels(),
                .sample_rate = data.sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            return false;
        }

        auto shared = std::make_shared<AudioFileData>(std::move(data));
        return publish_loaded(std::move(shared), std::move(path), memory_budget_bytes);
    }

    bool publish_loaded(std::shared_ptr<const AudioFileData> data,
                        std::string path,
                        uint64_t memory_budget_bytes = 0) {
        if (!data) {
            publish_missing(std::move(path), "sample data unavailable");
            return false;
        }

        const auto byte_size = decoded_byte_size(*data);
        const auto next_generation = snapshot_.generation + 1;
        if (memory_budget_bytes != 0 && byte_size > memory_budget_bytes) {
            snapshot_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .frame_count = data->num_frames(),
                .channel_count = data->num_channels(),
                .sample_rate = data->sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            diagnostics_ = {
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .path = std::move(path),
                .reason = "decoded sample exceeds memory budget",
                .frame_count = data->num_frames(),
                .channel_count = data->num_channels(),
                .sample_rate = data->sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            };
            return false;
        }

        snapshot_ = {
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .frame_count = data->num_frames(),
            .channel_count = data->num_channels(),
            .sample_rate = data->sample_rate,
            .byte_size = byte_size,
            .memory_budget_bytes = memory_budget_bytes,
            .data = std::move(data),
        };
        diagnostics_ = {
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .path = std::move(path),
            .frame_count = snapshot_.frame_count,
            .channel_count = snapshot_.channel_count,
            .sample_rate = snapshot_.sample_rate,
            .byte_size = snapshot_.byte_size,
            .memory_budget_bytes = memory_budget_bytes,
        };
        return true;
    }

    static uint64_t decoded_byte_size(const AudioFileData& data) noexcept {
        uint64_t total = 0;
        for (const auto& channel : data.channels) {
            total += static_cast<uint64_t>(channel.size() * sizeof(float));
        }
        return total;
    }

private:
    SampleResourceSnapshot snapshot_{};
    SampleResourceDiagnostics diagnostics_{};
};

struct SampleResourceCacheLimits {
    std::size_t max_entries = 16;
    uint64_t max_decoded_bytes = 0;
};

struct SampleResourceCacheStats {
    std::size_t entries = 0;
    uint64_t decoded_bytes = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t puts = 0;
    uint64_t evictions = 0;
    uint64_t rejections = 0;
};

/// Control-thread decoded sample cache for samplers and loopers.
///
/// This cache owns decoded sample memory and records deterministic hit/miss and
/// eviction counters. It is not an audio-thread API; publish cached data to a
/// `SampleResourceHandle` or a prepared realtime handoff slot before rendering.
class SampleResourceCache {
public:
    explicit SampleResourceCache(SampleResourceCacheLimits limits = {})
        : limits_(normalize_limits(limits)) {}

    std::shared_ptr<const AudioFileData> get(const std::string& path) {
        auto it = entries_.find(path);
        if (it == entries_.end()) {
            ++stats_.misses;
            return {};
        }

        lru_.splice(lru_.begin(), lru_, it->second.lru);
        ++stats_.hits;
        return it->second.data;
    }

    bool put(std::string path, AudioFileData data) {
        return put(std::move(path),
                   std::make_shared<AudioFileData>(std::move(data)));
    }

    bool put(std::string path, std::shared_ptr<const AudioFileData> data) {
        if (path.empty() || !data) {
            ++stats_.rejections;
            return false;
        }

        const auto bytes = SampleResourceHandle::decoded_byte_size(*data);
        if (limits_.max_decoded_bytes != 0 && bytes > limits_.max_decoded_bytes) {
            ++stats_.rejections;
            return false;
        }

        erase(path);
        lru_.push_front(path);
        entries_.emplace(path, Entry{
            .data = std::move(data),
            .decoded_bytes = bytes,
            .lru = lru_.begin(),
        });
        stats_.decoded_bytes += bytes;
        ++stats_.puts;
        evict_to_limits();
        return entries_.find(path) != entries_.end();
    }

    bool erase(const std::string& path) {
        auto it = entries_.find(path);
        if (it == entries_.end()) return false;
        stats_.decoded_bytes -= it->second.decoded_bytes;
        lru_.erase(it->second.lru);
        entries_.erase(it);
        return true;
    }

    void clear() {
        entries_.clear();
        lru_.clear();
        stats_.entries = 0;
        stats_.decoded_bytes = 0;
    }

    SampleResourceCacheStats stats() const {
        auto out = stats_;
        out.entries = entries_.size();
        return out;
    }

    SampleResourceCacheLimits limits() const noexcept { return limits_; }

private:
    struct Entry {
        std::shared_ptr<const AudioFileData> data;
        uint64_t decoded_bytes = 0;
        std::list<std::string>::iterator lru;
    };

    static SampleResourceCacheLimits normalize_limits(SampleResourceCacheLimits limits) {
        if (limits.max_entries == 0) limits.max_entries = 1;
        return limits;
    }

    void evict_to_limits() {
        while (entries_.size() > limits_.max_entries
               || (limits_.max_decoded_bytes != 0
                   && stats_.decoded_bytes > limits_.max_decoded_bytes)) {
            const auto victim = lru_.back();
            erase(victim);
            ++stats_.evictions;
        }
    }

    SampleResourceCacheLimits limits_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, Entry> entries_;
    SampleResourceCacheStats stats_{};
};

} // namespace pulp::audio
