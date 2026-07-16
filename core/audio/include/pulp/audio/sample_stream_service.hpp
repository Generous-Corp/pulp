#pragma once

#include <pulp/audio/sample_stream_scheduler.hpp>
#include <pulp/audio/sample_stream_window.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::audio {

struct SampleStreamSourceToken {
    std::uint64_t source_id = 0;
    std::uint64_t source_generation = 0;
};

struct SampleStreamRequesterToken {
    std::uint64_t requester_id = 0;
    std::uint64_t requester_generation = 0;
};

struct SampleStreamCacheServiceConfig {
    std::size_t scheduler_capacity = 0;
    std::uint64_t page_memory_budget_bytes = 0;
};

struct SampleStreamCacheSourceConfig {
    SampleStreamSourceToken token{};
    std::uint32_t channels = 0;
    std::uint64_t total_frames = 0;
    std::uint64_t page_frames = 0;
    std::uint32_t cache_page_count = 0;
};

struct SampleStreamPageDemand {
    SampleStreamSourceToken source{};
    SampleStreamRequesterToken requester{};
    std::uint64_t page_index = 0;
    std::uint64_t resident_source_frames = 0;
    double consumption_frames_per_second = 0.0;
    SampleStreamDemandClass demand_class = SampleStreamDemandClass::Sustain;
};

struct SampleStreamCacheSourceView {
    SampleStreamSourceToken token{};
    SampleStreamWindow* window = nullptr;
    std::uint64_t total_frames = 0;
    std::uint64_t page_frames = 0;

    bool valid() const noexcept {
        return token.source_id != 0 && token.source_generation != 0 &&
               window != nullptr && total_frames != 0 && page_frames != 0;
    }
};

enum class SampleStreamSourceAddStatus : std::uint8_t {
    Added,
    InvalidConfig,
    DuplicateSource,
    BudgetExceeded,
    AllocationFailed,
};

struct SampleStreamSourceAddResult {
    SampleStreamSourceAddStatus status = SampleStreamSourceAddStatus::InvalidConfig;
    SampleStreamCacheSourceView view{};

    bool added() const noexcept { return status == SampleStreamSourceAddStatus::Added; }
};

enum class SampleStreamServiceStatus : std::uint8_t {
    Idle,
    Published,
    AlreadyReady,
    StaleSource,
    NoPageAvailable,
    ReadFailed,
    PublishFailed,
};

struct SampleStreamCacheServiceStats {
    std::uint64_t reserved_page_bytes = 0;
    std::uint64_t source_count = 0;
    std::uint64_t decode_calls = 0;
    std::uint64_t pages_published = 0;
    std::uint64_t already_ready = 0;
    std::uint64_t stale_requests = 0;
    std::uint64_t no_page_available = 0;
    std::uint64_t read_failures = 0;
    std::uint64_t publish_failures = 0;
};

/// Bounded shared-page cache with deterministic caller-driven servicing.
/// All methods belong to one non-audio owner. Audio code reads returned windows
/// and hands trivially-owned demands to an external SPSC inbox before the owner
/// calls request_page() and service_once().
class SampleStreamCacheService {
public:
    bool prepare(const SampleStreamCacheServiceConfig& config) {
        release();
        if (config.scheduler_capacity == 0 || config.page_memory_budget_bytes == 0) {
            return false;
        }
        if (!scheduler_.prepare(config.scheduler_capacity)) return false;
        memory_budget_bytes_ = config.page_memory_budget_bytes;
        prepared_ = true;
        return true;
    }

    void release() noexcept {
        sources_.clear();
        scheduler_.reset();
        memory_budget_bytes_ = 0;
        prepared_ = false;
        stats_ = {};
    }

    bool prepared() const noexcept { return prepared_; }

    SampleStreamSourceAddResult add_source(const SampleStreamCacheSourceConfig& config,
                                           FrameReader reader) {
        if (!prepared_ || !valid_source_config(config) || !reader) {
            return {SampleStreamSourceAddStatus::InvalidConfig, {}};
        }
        if (find_source_id(config.token.source_id) != nullptr) {
            return {SampleStreamSourceAddStatus::DuplicateSource, {}};
        }

        const auto bytes = page_storage_bytes(config);
        if (!bytes || stats_.reserved_page_bytes > memory_budget_bytes_ ||
            *bytes > memory_budget_bytes_ - stats_.reserved_page_bytes) {
            return {SampleStreamSourceAddStatus::BudgetExceeded, {}};
        }

        try {
            auto source = std::make_unique<Source>();
            source->config = config;
            source->reader = std::move(reader);
            source->channel_ptrs.resize(config.channels);
            if (!source->window.prepare({
                    .channels = config.channels,
                    .page_count = config.cache_page_count,
                    .page_frames = config.page_frames,
                })) {
                return {SampleStreamSourceAddStatus::AllocationFailed, {}};
            }

            const auto view = make_view(*source);
            sources_.push_back(std::move(source));
            stats_.reserved_page_bytes += *bytes;
            stats_.source_count = sources_.size();
            return {SampleStreamSourceAddStatus::Added, view};
        } catch (...) {
            return {SampleStreamSourceAddStatus::AllocationFailed, {}};
        }
    }

    SampleStreamScheduleStatus request_page(const SampleStreamPageDemand& demand) noexcept {
        auto* source = find_source(demand.source);
        if (source == nullptr) {
            ++stats_.stale_requests;
            return SampleStreamScheduleStatus::Invalid;
        }
        const auto request = canonical_request(*source, demand);
        if (!request) return SampleStreamScheduleStatus::Invalid;
        return scheduler_.submit_or_refresh(*request);
    }

    std::size_t cancel_requester(SampleStreamRequesterToken requester) noexcept {
        if (requester.requester_id == 0 || requester.requester_generation == 0) return 0;
        return scheduler_.cancel_requester(requester.requester_id,
                                           requester.requester_generation);
    }

    std::size_t cancel_source_generation(SampleStreamSourceToken source) noexcept {
        if (source.source_id == 0 || source.source_generation == 0) return 0;
        return scheduler_.cancel_source_generation(source.source_id,
                                                   source.source_generation);
    }

    SampleStreamServiceStatus service_once() noexcept {
        const auto request = scheduler_.most_urgent();
        if (!request) return SampleStreamServiceStatus::Idle;

        auto* source = find_source({request->source_id, request->source_generation});
        if (source == nullptr) {
            scheduler_.complete_page(*request);
            ++stats_.stale_requests;
            return SampleStreamServiceStatus::StaleSource;
        }

        const auto start_frame = request->start_frame;
        if (source->window.ready_page_for_frame(request->source_generation, start_frame).valid) {
            scheduler_.complete_page(*request);
            ++stats_.already_ready;
            return SampleStreamServiceStatus::AlreadyReady;
        }

        const auto page_slot = reserve_empty_page(*source);
        if (!page_slot) {
            ++stats_.no_page_available;
            return SampleStreamServiceStatus::NoPageAvailable;
        }

        scheduler_.complete_page(*request);

        for (std::uint32_t channel = 0; channel < source->config.channels; ++channel) {
            source->channel_ptrs[channel] =
                source->window.writable_channel_data(*page_slot, channel);
            if (source->channel_ptrs[channel] == nullptr) {
                source->window.cancel_fill_page(*page_slot);
                ++stats_.read_failures;
                return SampleStreamServiceStatus::ReadFailed;
            }
        }

        BufferView<float> destination(source->channel_ptrs.data(),
                                      source->config.channels,
                                      static_cast<std::size_t>(request->frame_count));
        std::uint64_t decoded = 0;
        ++stats_.decode_calls;
        try {
            decoded = source->reader(start_frame, destination, request->frame_count);
        } catch (...) {
            decoded = 0;
        }

        if (decoded != request->frame_count) {
            source->window.cancel_fill_page(*page_slot);
            ++stats_.read_failures;
            return SampleStreamServiceStatus::ReadFailed;
        }

        const bool final_page =
            request->frame_count == source->config.total_frames - start_frame;
        if (!source->window.publish_page(*page_slot,
                                        {
                                            .stream_generation = request->source_generation,
                                            .start_frame = start_frame,
                                            .valid_frames = request->frame_count,
                                            .final_page = final_page,
                                        })) {
            source->window.cancel_fill_page(*page_slot);
            ++stats_.publish_failures;
            return SampleStreamServiceStatus::PublishFailed;
        }

        ++stats_.pages_published;
        return SampleStreamServiceStatus::Published;
    }

    SampleStreamSchedulerStats scheduler_stats() const noexcept {
        return scheduler_.stats();
    }

    SampleStreamCacheServiceStats stats() const noexcept { return stats_; }

private:
    struct Source {
        SampleStreamCacheSourceConfig config{};
        SampleStreamWindow window;
        FrameReader reader;
        std::vector<float*> channel_ptrs;
    };

    static bool valid_source_config(const SampleStreamCacheSourceConfig& config) noexcept {
        return config.token.source_id != 0 && config.token.source_generation != 0 &&
               config.channels != 0 && config.total_frames != 0 &&
               config.page_frames != 0 && config.cache_page_count != 0 &&
               config.page_frames <= std::numeric_limits<std::size_t>::max();
    }

    static std::optional<std::uint64_t> page_storage_bytes(
        const SampleStreamCacheSourceConfig& config) noexcept {
        std::uint64_t result = config.channels;
        for (const auto factor : {config.page_frames,
                                  static_cast<std::uint64_t>(config.cache_page_count),
                                  static_cast<std::uint64_t>(sizeof(float))}) {
            if (factor != 0 && result > std::numeric_limits<std::uint64_t>::max() / factor) {
                return std::nullopt;
            }
            result *= factor;
        }
        return result;
    }

    static SampleStreamCacheSourceView make_view(Source& source) noexcept {
        return {
            .token = source.config.token,
            .window = &source.window,
            .total_frames = source.config.total_frames,
            .page_frames = source.config.page_frames,
        };
    }

    Source* find_source_id(std::uint64_t source_id) noexcept {
        const auto found = std::find_if(sources_.begin(), sources_.end(),
            [source_id](const auto& source) noexcept {
                return source->config.token.source_id == source_id;
            });
        return found == sources_.end() ? nullptr : found->get();
    }

    Source* find_source(SampleStreamSourceToken token) noexcept {
        auto* source = find_source_id(token.source_id);
        if (source == nullptr ||
            source->config.token.source_generation != token.source_generation) {
            return nullptr;
        }
        return source;
    }

    static std::optional<SampleStreamPageRequest> canonical_request(
        const Source& source,
        const SampleStreamPageDemand& demand) noexcept {
        if (demand.requester.requester_id == 0 ||
            demand.requester.requester_generation == 0 ||
            demand.page_index >
                std::numeric_limits<std::uint64_t>::max() / source.config.page_frames) {
            return std::nullopt;
        }
        const auto start_frame = demand.page_index * source.config.page_frames;
        if (start_frame >= source.config.total_frames) return std::nullopt;
        const auto frame_count = std::min(source.config.page_frames,
                                          source.config.total_frames - start_frame);
        return SampleStreamPageRequest{
            .source_id = source.config.token.source_id,
            .source_generation = source.config.token.source_generation,
            .requester_id = demand.requester.requester_id,
            .requester_generation = demand.requester.requester_generation,
            .page_index = demand.page_index,
            .start_frame = start_frame,
            .frame_count = frame_count,
            .resident_source_frames = demand.resident_source_frames,
            .consumption_frames_per_second = demand.consumption_frames_per_second,
            .demand_class = demand.demand_class,
        };
    }

    static std::optional<std::uint32_t> reserve_empty_page(Source& source) noexcept {
        for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
            if (source.window.page_state(page) == SampleStreamPageState::Empty &&
                source.window.begin_fill_page(page)) {
                return page;
            }
        }
        return std::nullopt;
    }

    std::vector<std::unique_ptr<Source>> sources_;
    SampleStreamScheduler scheduler_;
    std::uint64_t memory_budget_bytes_ = 0;
    bool prepared_ = false;
    SampleStreamCacheServiceStats stats_{};
};

}  // namespace pulp::audio
