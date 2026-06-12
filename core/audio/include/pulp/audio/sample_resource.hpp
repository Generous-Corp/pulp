#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/background_job.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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
    SampleResourceSnapshot snapshot() const {
        auto current = std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
        return current ? *current : SampleResourceSnapshot{};
    }
    SampleResourceDiagnostics diagnostics() const {
        std::lock_guard lock(diagnostics_mutex_);
        return diagnostics_;
    }

    void clear() {
        publish_snapshot({});
        std::lock_guard lock(diagnostics_mutex_);
        diagnostics_ = {};
    }

    void publish_missing(std::string path, std::string reason) {
        const auto next_generation = snapshot().generation + 1;
        publish_snapshot({
            .status = SampleResourceStatus::Missing,
            .generation = next_generation,
        });
        std::lock_guard lock(diagnostics_mutex_);
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
        const auto next_generation = snapshot().generation + 1;
        if (memory_budget_bytes != 0 && byte_size > memory_budget_bytes) {
            publish_snapshot({
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .frame_count = data.num_frames(),
                .channel_count = data.num_channels(),
                .sample_rate = data.sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            });
            std::lock_guard lock(diagnostics_mutex_);
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
        const auto next_generation = snapshot().generation + 1;
        if (memory_budget_bytes != 0 && byte_size > memory_budget_bytes) {
            publish_snapshot({
                .status = SampleResourceStatus::Oversized,
                .generation = next_generation,
                .frame_count = data->num_frames(),
                .channel_count = data->num_channels(),
                .sample_rate = data->sample_rate,
                .byte_size = byte_size,
                .memory_budget_bytes = memory_budget_bytes,
            });
            std::lock_guard lock(diagnostics_mutex_);
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

        SampleResourceSnapshot next{
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .frame_count = data->num_frames(),
            .channel_count = data->num_channels(),
            .sample_rate = data->sample_rate,
            .byte_size = byte_size,
            .memory_budget_bytes = memory_budget_bytes,
            .data = std::move(data),
        };
        publish_snapshot(next);
        std::lock_guard lock(diagnostics_mutex_);
        diagnostics_ = {
            .status = SampleResourceStatus::Loaded,
            .generation = next_generation,
            .path = std::move(path),
            .frame_count = next.frame_count,
            .channel_count = next.channel_count,
            .sample_rate = next.sample_rate,
            .byte_size = next.byte_size,
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
    void publish_snapshot(SampleResourceSnapshot snapshot) {
        std::shared_ptr<const SampleResourceSnapshot> next =
            std::make_shared<SampleResourceSnapshot>(std::move(snapshot));
        std::atomic_store_explicit(&snapshot_, std::move(next), std::memory_order_release);
    }

    std::shared_ptr<const SampleResourceSnapshot> snapshot_ =
        std::make_shared<SampleResourceSnapshot>();
    mutable std::mutex diagnostics_mutex_;
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

struct SampleResourcePage {
    uint64_t start_frame = 0;
    AudioFileData data;

    uint64_t end_frame() const noexcept { return start_frame + data.num_frames(); }

    bool contains(uint64_t frame) const noexcept {
        return frame >= start_frame && frame < end_frame();
    }

    float sample(uint32_t channel, uint64_t frame) const noexcept {
        if (!contains(frame) || channel >= data.channels.size()) return 0.0f;
        const auto& samples = data.channels[channel];
        const auto local = frame - start_frame;
        if (local >= samples.size()) return 0.0f;
        return samples[static_cast<std::size_t>(local)];
    }
};

struct SampleResourcePageWindow {
    uint32_t sample_rate = 0;
    uint32_t channel_count = 0;
    uint64_t generation = 0;
    std::vector<SampleResourcePage> pages;

    uint64_t covered_frame_count() const noexcept {
        uint64_t covered = 0;
        for (const auto& page : pages) {
            covered += page.data.num_frames();
        }
        return covered;
    }

    const SampleResourcePage* find_page(uint64_t frame) const noexcept {
        for (const auto& page : pages) {
            if (page.contains(frame)) return &page;
        }
        return nullptr;
    }

    float sample(uint32_t channel, uint64_t frame) const noexcept {
        const auto* page = find_page(frame);
        return page ? page->sample(channel, frame) : 0.0f;
    }
};

struct SampleResourcePageHandoffDiagnostics {
    uint64_t generation = 0;
    std::size_t page_count = 0;
    uint64_t covered_frame_count = 0;
    uint64_t page_misses = 0;
    uint64_t last_miss_frame = 0;
};

/// Render-thread view of immutable decoded sample pages.
///
/// Control/background code owns file I/O and decoding, then publishes a page
/// window. The render path can only read resident pages; cache misses return
/// silence and update atomic diagnostics, so this object cannot accidentally
/// perform file I/O from the audio thread.
class SampleResourcePageHandoff {
public:
    void publish(SampleResourcePageWindow window) {
        auto shared = std::make_shared<SampleResourcePageWindow>(std::move(window));
        publish(std::move(shared));
    }

    void publish(std::shared_ptr<const SampleResourcePageWindow> window) {
        std::atomic_store_explicit(&window_, std::move(window), std::memory_order_release);
        page_misses_.store(0, std::memory_order_relaxed);
        last_miss_frame_.store(0, std::memory_order_relaxed);
    }

    void clear() {
        std::shared_ptr<const SampleResourcePageWindow> empty;
        std::atomic_store_explicit(&window_, std::move(empty), std::memory_order_release);
        page_misses_.store(0, std::memory_order_relaxed);
        last_miss_frame_.store(0, std::memory_order_relaxed);
    }

    std::shared_ptr<const SampleResourcePageWindow> snapshot() const {
        return std::atomic_load_explicit(&window_, std::memory_order_acquire);
    }

    float sample_or_zero(uint32_t channel, uint64_t frame) const noexcept {
        const auto window = snapshot();
        if (!window) {
            record_miss(frame);
            return 0.0f;
        }

        const auto* page = window->find_page(frame);
        if (!page) {
            record_miss(frame);
            return 0.0f;
        }

        return page->sample(channel, frame);
    }

    void read_channel(float* dest,
                      uint32_t channel,
                      uint64_t start_frame,
                      uint64_t frame_count) const noexcept {
        if (!dest) return;
        for (uint64_t i = 0; i < frame_count; ++i) {
            dest[i] = sample_or_zero(channel, start_frame + i);
        }
    }

    SampleResourcePageHandoffDiagnostics diagnostics() const {
        const auto window = snapshot();
        return {
            .generation = window ? window->generation : 0,
            .page_count = window ? window->pages.size() : 0,
            .covered_frame_count = window ? window->covered_frame_count() : 0,
            .page_misses = page_misses_.load(std::memory_order_relaxed),
            .last_miss_frame = last_miss_frame_.load(std::memory_order_relaxed),
        };
    }

private:
    void record_miss(uint64_t frame) const noexcept {
        page_misses_.fetch_add(1, std::memory_order_relaxed);
        last_miss_frame_.store(frame, std::memory_order_relaxed);
    }

    std::shared_ptr<const SampleResourcePageWindow> window_;
    mutable std::atomic<uint64_t> page_misses_{0};
    mutable std::atomic<uint64_t> last_miss_frame_{0};
};

struct SampleResourceLoadOptions {
    uint64_t memory_budget_bytes = 0;
    runtime::BackgroundJobPriority priority = runtime::BackgroundJobPriority::normal;
};

struct SampleResourceServiceStats {
    uint64_t submitted_loads = 0;
    uint64_t submitted_prefetches = 0;
    uint64_t cache_hits = 0;
    uint64_t decode_successes = 0;
    uint64_t decode_failures = 0;
    uint64_t cancelled = 0;
    uint64_t drained_completions = 0;
};

using SampleResourceDecodeFunction = std::function<std::optional<AudioFileData>(
    const std::string& path,
    runtime::BackgroundJobContext& context)>;

/// Non-RT service for background sample decode and cache prefetch.
///
/// Worker jobs decode files and queue completions. Owners must call
/// `drain_completions()` from a non-RT control thread to publish decoded data
/// into `SampleResourceCache` and optional `SampleResourceHandle` instances.
class SampleResourceService {
public:
    explicit SampleResourceService(
        SampleResourceCacheLimits limits = {},
        SampleResourceDecodeFunction decode = {})
        : cache_(limits)
        , decode_(std::move(decode)) {
        if (!decode_) decode_ = default_decode;
    }

    ~SampleResourceService() {
        jobs_.cancel_all();
        jobs_.wait_all();
        drain_completions();
    }

    SampleResourceService(const SampleResourceService&) = delete;
    SampleResourceService& operator=(const SampleResourceService&) = delete;

    runtime::BackgroundJobHandle load_async(std::string path,
                                            SampleResourceHandle& target,
                                            SampleResourceLoadOptions options = {}) {
        {
            std::lock_guard lock(mutex_);
            ++stats_.submitted_loads;
            if (auto cached = cache_.get(path)) {
                ++stats_.cache_hits;
                target.publish_loaded(std::move(cached),
                                      std::move(path),
                                      options.memory_budget_bytes);
                return {};
            }
        }

        return submit_decode(std::move(path), &target, options);
    }

    runtime::BackgroundJobHandle prefetch_async(std::string path,
                                                SampleResourceLoadOptions options = {}) {
        {
            std::lock_guard lock(mutex_);
            ++stats_.submitted_prefetches;
            if (cache_.get(path)) {
                ++stats_.cache_hits;
                return {};
            }
        }

        return submit_decode(std::move(path), nullptr, options);
    }

    std::size_t drain_completions() {
        std::vector<Completion> completions;
        {
            std::lock_guard lock(mutex_);
            completions.swap(completions_);

            for (auto& completion : completions) {
                if (completion.cancelled) {
                    ++stats_.cancelled;
                } else if (completion.data) {
                    auto shared = std::make_shared<AudioFileData>(std::move(*completion.data));
                    const auto cached = shared;
                    cache_.put(completion.path, std::move(shared));
                    if (completion.target) {
                        completion.target->publish_loaded(std::move(cached),
                                                           completion.path,
                                                           completion.memory_budget_bytes);
                    }
                    ++stats_.decode_successes;
                } else {
                    if (completion.target) {
                        completion.target->publish_missing(completion.path, completion.reason);
                    }
                    ++stats_.decode_failures;
                }
                ++stats_.drained_completions;
            }
        }

        return completions.size();
    }

    bool publish_cached(const std::string& path,
                        SampleResourceHandle& target,
                        uint64_t memory_budget_bytes = 0) {
        std::lock_guard lock(mutex_);
        if (auto cached = cache_.get(path)) {
            ++stats_.cache_hits;
            target.publish_loaded(std::move(cached), path, memory_budget_bytes);
            return true;
        }
        return false;
    }

    bool publish_cached(const std::string& path,
                        SampleResourceHandle& target,
                        SampleResourceLoadOptions options) {
        return publish_cached(path, target, options.memory_budget_bytes);
    }

    SampleResourceCacheStats cache_stats() const {
        std::lock_guard lock(mutex_);
        return cache_.stats();
    }

    SampleResourceServiceStats stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    std::size_t pending_count() const {
        return jobs_.pending_count();
    }

    static std::optional<AudioFileData> default_decode(
        const std::string& path,
        runtime::BackgroundJobContext&) {
        return read_audio_file(path);
    }

private:
    struct Completion {
        std::string path;
        SampleResourceHandle* target = nullptr;
        uint64_t memory_budget_bytes = 0;
        std::optional<AudioFileData> data;
        std::string reason;
        bool cancelled = false;
    };

    runtime::BackgroundJobHandle submit_decode(std::string path,
                                               SampleResourceHandle* target,
                                               SampleResourceLoadOptions options) {
        auto job_path = path;
        return jobs_.submit(
            {
                .name = "sample-resource-decode:" + path,
                .priority = options.priority,
            },
            [this,
             path = std::move(job_path),
             target,
             memory_budget_bytes = options.memory_budget_bytes](
                runtime::BackgroundJobContext& context) mutable {
                if (context.is_cancelled()) {
                    queue_completion({
                        .path = std::move(path),
                        .target = target,
                        .memory_budget_bytes = memory_budget_bytes,
                        .cancelled = true,
                    });
                    return;
                }

                auto decoded = decode_(path, context);
                if (context.is_cancelled()) {
                    queue_completion({
                        .path = std::move(path),
                        .target = target,
                        .memory_budget_bytes = memory_budget_bytes,
                        .cancelled = true,
                    });
                    return;
                }

                queue_completion({
                    .path = std::move(path),
                    .target = target,
                    .memory_budget_bytes = memory_budget_bytes,
                    .data = std::move(decoded),
                    .reason = decoded ? std::string{} : "decode failed",
                });
            });
    }

    void queue_completion(Completion completion) {
        std::lock_guard lock(mutex_);
        completions_.push_back(std::move(completion));
    }

    mutable std::mutex mutex_;
    runtime::BackgroundJobService jobs_;
    SampleResourceCache cache_;
    SampleResourceDecodeFunction decode_;
    std::vector<Completion> completions_;
    SampleResourceServiceStats stats_{};
};

} // namespace pulp::audio
