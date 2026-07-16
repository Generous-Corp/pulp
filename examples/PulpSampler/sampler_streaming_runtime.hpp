#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_preload_contract.hpp>
#include <pulp/audio/sample_stream_async_service.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>
#include <pulp/runtime/seqlock.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace pulp::examples {

enum class SamplerPublishedSourceKind : std::uint8_t {
    None,
    Resident,
    Streamed,
};

struct SamplerPublishedSource {
    SamplerPublishedSourceKind kind = SamplerPublishedSourceKind::None;
    std::uint64_t selection_generation = 0;
    audio::PublishedSampleView resident{};
    audio::SampleAssetView streamed{};
};

static_assert(std::is_trivially_copyable_v<SamplerPublishedSource>);

struct PulpSamplerStreamStats {
    std::uint64_t pages_published = 0;
    std::uint64_t starved_output_frames = 0;
    std::uint64_t sources_retired = 0;
    std::uint64_t active_sources = 0;
    std::uint64_t preload_frames = 0;
};

struct PulpSamplerTestAccess;

class SamplerStreamingRuntime {
public:
    static constexpr std::size_t kCommandCapacity = 256;
    using CommandInbox = audio::SampleStreamCommandInbox<kCommandCapacity>;

    ~SamplerStreamingRuntime() { release(); }

    bool prepare(float host_sample_rate, std::uint32_t maximum_host_block_frames) {
        release();
        host_sample_rate_ = host_sample_rate;
        maximum_host_block_frames_ = std::max<std::uint32_t>(
            1, maximum_host_block_frames);
        selection_generation_ = 0;
        stream_audio_ack_selection_.store(0, std::memory_order_relaxed);
        audio_active_generation_.store(1, std::memory_order_relaxed);
        audio_completed_generation_.store(0, std::memory_order_relaxed);
        next_audio_generation_.store(1, std::memory_order_relaxed);
        pages_published_.store(0, std::memory_order_relaxed);
        starved_frames_.store(0, std::memory_order_relaxed);
        sources_retired_.store(0, std::memory_order_relaxed);
        active_sources_.store(0, std::memory_order_relaxed);
        preload_frames_.store(0, std::memory_order_relaxed);
        service_dispatch_paused_.store(false, std::memory_order_relaxed);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        reverse_prewarm_pending_for_test_.store(false, std::memory_order_relaxed);
        block_next_reverse_decode_for_test_.store(false, std::memory_order_relaxed);
        reverse_decode_entered_for_test_.store(false, std::memory_order_relaxed);
        release_reverse_decode_for_test_.store(true, std::memory_order_relaxed);
        unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
#endif
        unpublished_rollbacks_.fill({});
        published_source_.write({});

        const auto maximum_source_frames_per_block =
            std::ceil(static_cast<double>(maximum_host_block_frames_) *
                      kMaximumPitchRatio * kMaximumSourceRate /
                      static_cast<double>(host_sample_rate_));
        const auto maximum_preload = audio::evaluate_sample_preload_contract({
            .source_sample_rate = kMaximumSourceRate,
            .host_sample_rate = static_cast<double>(host_sample_rate_),
            .maximum_playback_ratio = kMaximumPitchRatio,
            .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
            .scheduler_margin_seconds = kSchedulerMarginSeconds,
            .decoder_latency_seconds = kDecoderLatencySeconds,
            .maximum_host_block_frames = maximum_host_block_frames_,
            .interpolation_guard_frames = 2,
        });
        const auto cache_working_set_page_frames = maximum_preload.valid()
            ? std::ceil((static_cast<double>(maximum_preload.required_preload_frames) +
                         maximum_source_frames_per_block) /
                        static_cast<double>(kPagesPerVoiceWorkingSet - 1))
            : 0.0;
        page_frames_ = std::max<std::uint64_t>(
            {kDefaultPageFrames,
             static_cast<std::uint64_t>(maximum_source_frames_per_block /
                                        (audio::kSampleStreamVoiceMaxPageDemands - 2)) + 1,
             static_cast<std::uint64_t>(cache_working_set_page_frames)});

        service_ready_ = service_.prepare({
            .cache = {
                .scheduler_capacity = kCommandCapacity,
                .page_memory_budget_bytes =
                    kSourceCapacity * kMaximumChannels * kCachePagesPerSource *
                    page_frames_ * sizeof(float),
            },
            .decode = {
                .worker_count = kWorkerCount,
                .source_capacity = kSourceCapacity,
                .maximum_channels = kMaximumChannels,
                .maximum_frames_per_job = page_frames_,
            },
        });
        for (auto& slot : slots_) slot = std::make_unique<StreamedSlot>();
        service_running_.store(service_ready_, std::memory_order_release);
        if (service_ready_) {
            service_thread_ = std::thread([this] { service_loop(); });
        }
        return service_ready_;
    }

    void release() noexcept {
        {
            std::lock_guard lock(file_request_mutex_);
            service_running_.store(false, std::memory_order_release);
            file_request_complete_ = true;
            file_request_result_ = false;
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        release_reverse_decode_for_test_.store(true, std::memory_order_release);
#endif
        service_wake_.notify_all();
        file_request_changed_.notify_all();
        if (service_thread_.joinable()) service_thread_.join();

        published_source_.write({});
        if (service_ready_) {
            service_.update_audio_generations(
                audio_active_generation_.load(std::memory_order_acquire),
                audio_completed_generation_.load(std::memory_order_acquire));
            service_.drain_commands(commands_);
        }
        service_.release();
        unpublished_rollbacks_.fill({});
#if defined(PULP_SAMPLER_TEST_HOOKS)
        unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
#endif
        for (auto& slot : slots_) {
            if (slot) slot->asset.release();
            slot.reset();
        }
        service_ready_ = false;
        active_sources_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lock(file_request_mutex_);
            file_request_pending_ = false;
            file_request_path_.clear();
        }
    }

    template<typename Loader, typename ViewReader>
    bool load_and_publish_resident(Loader&& loader, ViewReader&& read_view) {
        std::lock_guard lock(source_load_mutex_);
        if (!loader()) return false;
        const auto resident = read_view();
        if (!resident.valid) return false;
        published_source_.write({
            .kind = SamplerPublishedSourceKind::Resident,
            .selection_generation = ++selection_generation_,
            .resident = resident,
        });
        service_wake_.notify_all();
        return true;
    }

    bool load_sample_file(std::string_view path) {
        if (path.empty()) return false;
        std::lock_guard source_lock(source_load_mutex_);
        std::unique_lock request_lock(file_request_mutex_);
        if (!service_running_.load(std::memory_order_acquire) || file_request_pending_)
            return false;
        file_request_path_ = std::string(path);
        file_request_result_ = false;
        file_request_complete_ = false;
        file_request_pending_ = true;
        service_wake_.notify_all();
        file_request_changed_.wait(request_lock, [this] {
            return file_request_complete_ ||
                   !service_running_.load(std::memory_order_acquire);
        });
        return file_request_complete_ && file_request_result_;
    }

    SamplerPublishedSource published_source() const noexcept {
        return published_source_.read();
    }

    PulpSamplerStreamStats stats() const noexcept {
        return {
            .pages_published = pages_published_.load(std::memory_order_relaxed),
            .starved_output_frames = starved_frames_.load(std::memory_order_relaxed),
            .sources_retired = sources_retired_.load(std::memory_order_relaxed),
            .active_sources = active_sources_.load(std::memory_order_relaxed),
            .preload_frames = preload_frames_.load(std::memory_order_relaxed),
        };
    }

    CommandInbox& command_inbox() noexcept { return commands_; }

    std::uint64_t begin_audio_callback() noexcept {
        const auto generation =
            next_audio_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
        audio_active_generation_.store(generation, std::memory_order_release);
        return generation;
    }

    void complete_audio_callback(std::uint64_t generation) noexcept {
        audio_completed_generation_.store(generation, std::memory_order_release);
    }

    void acknowledge_selection(std::uint64_t generation) noexcept {
        stream_audio_ack_selection_.store(generation, std::memory_order_release);
    }

    void add_starved_frames(std::uint64_t frames) noexcept {
        starved_frames_.fetch_add(frames, std::memory_order_relaxed);
    }

    static constexpr double maximum_pitch_ratio() noexcept {
        return kMaximumPitchRatio;
    }

    static constexpr std::uint32_t maximum_voice_count() noexcept {
        return kMaximumVoices;
    }

private:
    friend struct PulpSamplerTestAccess;

    static constexpr std::uint32_t kWorkerCount = 2;
    static constexpr std::uint32_t kSourceCapacity = 2;
    static constexpr std::uint32_t kMaximumVoices = 8;
    static constexpr std::uint32_t kPagesPerVoiceWorkingSet = 8;
    static constexpr std::uint32_t kCachePagesPerSource =
        kMaximumVoices * kPagesPerVoiceWorkingSet;
    static constexpr std::uint32_t kMaximumChannels = 2;
    static constexpr std::uint64_t kDefaultPageFrames = 2048;
    static constexpr double kMaximumPitchRatio = 4.0;
    static constexpr double kMaximumSourceRate = 192000.0;
    static constexpr double kCertifiedIoLatencySeconds = 0.020;
    static constexpr double kSchedulerMarginSeconds = 0.005;
    static constexpr double kDecoderLatencySeconds = 0.005;
    static constexpr std::uint64_t kAdmissionRequesterId =
        std::numeric_limits<std::uint64_t>::max();

    struct StreamedSlot {
        audio::SampleAsset asset;
        audio::SampleStreamSourceToken source{};
        std::uint64_t selection_generation = 0;
        bool occupied = false;
        bool retirement_scheduled = false;
    };

    runtime::SeqLock<SamplerPublishedSource> published_source_;
    std::atomic<std::uint64_t> stream_audio_ack_selection_{0};
    std::atomic<std::uint64_t> audio_active_generation_{1};
    std::atomic<std::uint64_t> audio_completed_generation_{0};
    std::atomic<std::uint64_t> next_audio_generation_{1};
    std::atomic<std::uint64_t> pages_published_{0};
    std::atomic<std::uint64_t> starved_frames_{0};
    std::atomic<std::uint64_t> sources_retired_{0};
    std::atomic<std::uint64_t> active_sources_{0};
    std::atomic<std::uint64_t> preload_frames_{0};
    audio::SampleStreamAsyncService<> service_;
    CommandInbox commands_;
    std::array<std::unique_ptr<StreamedSlot>, kSourceCapacity> slots_{};
    std::array<audio::SampleStreamSourceToken, kSourceCapacity>
        unpublished_rollbacks_{};
    std::thread service_thread_;
    std::atomic<bool> service_running_{false};
    std::atomic<bool> service_dispatch_paused_{false};
#if defined(PULP_SAMPLER_TEST_HOOKS)
    std::atomic<bool> reverse_prewarm_pending_for_test_{false};
    std::atomic<bool> block_next_reverse_decode_for_test_{false};
    std::atomic<bool> reverse_decode_entered_for_test_{false};
    std::atomic<bool> release_reverse_decode_for_test_{true};
    std::atomic<std::uint32_t> unpublished_rollback_count_for_test_{0};
#endif
    bool service_ready_ = false;
    float host_sample_rate_ = 44100.0f;
    std::uint32_t maximum_host_block_frames_ = 512;
    std::uint64_t page_frames_ = kDefaultPageFrames;
    std::uint64_t selection_generation_ = 0;
    std::uint64_t next_source_id_ = 1;
    std::uint64_t next_asset_id_ = 1;
    std::chrono::milliseconds reverse_prewarm_timeout_{250};
    std::mutex source_load_mutex_;
    std::mutex file_request_mutex_;
    std::condition_variable file_request_changed_;
    std::condition_variable service_wake_;
    std::mutex service_wait_mutex_;
    std::string file_request_path_;
    bool file_request_pending_ = false;
    bool file_request_complete_ = false;
    bool file_request_result_ = false;

    void service_loop() noexcept {
        while (service_running_.load(std::memory_order_acquire)) {
            service_.update_audio_generations(
                audio_active_generation_.load(std::memory_order_acquire),
                audio_completed_generation_.load(std::memory_order_acquire));
            process_file_request();
            service_.drain_commands(commands_);
            service_.drain_completions();
            collect_unpublished_rollbacks();
            if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
                for (std::uint32_t attempt = 0;
                     attempt < kSourceCapacity * 2;
                     ++attempt) {
                    const auto dispatched = service_.dispatch_once();
                    if (dispatched.status !=
                        audio::SampleStreamAsyncDispatchStatus::Queued) {
                        break;
                    }
                }
            }
            retire_sources();
            pages_published_.store(
                service_.telemetry().completions_published,
                std::memory_order_relaxed);

            std::unique_lock lock(service_wait_mutex_);
            service_wake_.wait_for(lock, std::chrono::milliseconds(1), [this] {
                return !service_running_.load(std::memory_order_acquire) ||
                       file_request_pending_snapshot();
            });
        }
    }

    bool file_request_pending_snapshot() noexcept {
        std::lock_guard lock(file_request_mutex_);
        return file_request_pending_;
    }

    void process_file_request() noexcept {
        std::string path;
        {
            std::lock_guard lock(file_request_mutex_);
            if (!file_request_pending_) return;
            path = std::move(file_request_path_);
            file_request_pending_ = false;
        }

        bool loaded = false;
        try {
            loaded = prepare_streamed_file(path);
        } catch (...) {
            loaded = false;
        }
        {
            std::lock_guard lock(file_request_mutex_);
            file_request_result_ =
                loaded && service_running_.load(std::memory_order_acquire);
            file_request_complete_ = true;
        }
        file_request_changed_.notify_all();
    }

    bool prepare_streamed_file(const std::string& path) {
        StreamedSlot* slot = nullptr;
        for (auto& candidate : slots_) {
            if (candidate && !candidate->occupied) {
                slot = candidate.get();
                break;
            }
        }
        if (slot == nullptr) return false;

        auto file = audio::make_memory_mapped_frame_reader(path, true);
        if (!file.valid || !file.supports_ranged_read ||
            file.channels == 0 || file.channels > kMaximumChannels ||
            file.sample_rate == 0 || file.sample_rate > kMaximumSourceRate) {
            return false;
        }

        audio::SamplePreloadContract contract{
            .source_sample_rate = static_cast<double>(file.sample_rate),
            .host_sample_rate = static_cast<double>(host_sample_rate_),
            .maximum_playback_ratio = kMaximumPitchRatio,
            .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
            .scheduler_margin_seconds = kSchedulerMarginSeconds,
            .decoder_latency_seconds = kDecoderLatencySeconds,
            .maximum_host_block_frames = maximum_host_block_frames_,
            .interpolation_guard_frames = 2,
        };
        const auto required = audio::evaluate_sample_preload_contract(contract);
        if (!required.valid() || required.required_preload_frames == 0) return false;
        const auto preload_frames =
            std::min(file.total_frames, required.required_preload_frames);
        contract.configured_preload_frames = preload_frames;

        audio::Buffer<float> preload(file.channels,
                                     static_cast<std::size_t>(preload_frames));
        if (file.binding.read(0,
                              preload.view(),
                              preload_frames,
                              std::stop_token{}) != preload_frames) {
            return false;
        }
        if (!service_running_.load(std::memory_order_acquire)) return false;

#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (block_next_reverse_decode_for_test_.exchange(
                false, std::memory_order_acq_rel)) {
            auto reader = std::move(file.binding.read);
            file.binding.read =
                [this, reader = std::move(reader)](
                    std::uint64_t start,
                    audio::BufferView<float> destination,
                    std::uint64_t frames,
                    std::stop_token stop_token) {
                    reverse_decode_entered_for_test_.store(
                        true, std::memory_order_release);
                    while (!release_reverse_decode_for_test_.load(
                               std::memory_order_acquire) &&
                           service_running_.load(std::memory_order_acquire) &&
                           !stop_token.stop_requested()) {
                        std::this_thread::yield();
                    }
                    if (!service_running_.load(std::memory_order_acquire) ||
                        stop_token.stop_requested()) {
                        return std::uint64_t{0};
                    }
                    return reader(start, destination, frames, stop_token);
                };
            file.binding.stop_mode = audio::FrameReaderStopMode::Cooperative;
        }
#endif

        const audio::SampleStreamSourceToken source{next_source_id_++, 1};
        const auto added = service_.add_source(
            {
                .token = source,
                .channels = file.channels,
                .total_frames = file.total_frames,
                .page_frames = page_frames_,
                .cache_page_count = kCachePagesPerSource,
            },
            std::move(file.binding));
        if (!added.added()) return false;

        if (preload_frames < file.total_frames &&
            !prewarm_reverse_entry_page(source,
                                        added.view,
                                        file.total_frames,
                                        file.sample_rate)) {
            rollback_unpublished_source(source);
            return false;
        }

        const audio::SampleAssetConfig asset_config{
            .asset = {next_asset_id_++, 1},
            .source = source,
            .channels = file.channels,
            .total_frames = file.total_frames,
            .sample_rate = file.sample_rate,
            .preload_frames = preload_frames,
            .preload_contract = contract,
            .stream_source = added.view,
        };
        if (!slot->asset.prepare(asset_config, preload.view())) {
            rollback_unpublished_source(source);
            return false;
        }

        {
            std::lock_guard lock(file_request_mutex_);
            if (service_running_.load(std::memory_order_acquire)) {
                slot->source = source;
                slot->selection_generation = ++selection_generation_;
                slot->occupied = true;
                slot->retirement_scheduled = false;
                published_source_.write({
                    .kind = SamplerPublishedSourceKind::Streamed,
                    .selection_generation = slot->selection_generation,
                    .streamed = slot->asset.view(),
                });
                active_sources_.fetch_add(1, std::memory_order_relaxed);
                preload_frames_.store(preload_frames, std::memory_order_relaxed);
                return true;
            }
        }
        slot->asset.release();
        rollback_unpublished_source(source);
        return false;
    }

    bool prewarm_reverse_entry_page(
        audio::SampleStreamSourceToken source,
        const audio::SampleStreamCacheSourceView& view,
        std::uint64_t total_frames,
        std::uint32_t sample_rate) noexcept {
        const auto tail_frame = total_frames - 1;
        const audio::SampleStreamRequesterToken requester{
            kAdmissionRequesterId, source.source_id};
        const auto scheduled = service_.request_page({
            .source = source,
            .requester = requester,
            .page_index = tail_frame / page_frames_,
            .resident_source_frames = 0,
            .consumption_frames_per_second =
                static_cast<double>(sample_rate) * kMaximumPitchRatio,
            .demand_class = audio::SampleStreamDemandClass::Attack,
        });
        if (scheduled != audio::SampleStreamScheduleStatus::Inserted &&
            scheduled != audio::SampleStreamScheduleStatus::Refreshed) {
            (void) service_.cancel_requester(requester);
            return false;
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        reverse_prewarm_pending_for_test_.store(true, std::memory_order_release);
#endif

        const auto deadline =
            std::chrono::steady_clock::now() + reverse_prewarm_timeout_;
        while (service_running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            service_.drain_commands(commands_);
            service_.drain_completions();
            if (view.window->ready_page_for_frame(
                    source.source_generation, tail_frame).valid) {
                (void) service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
                reverse_prewarm_pending_for_test_.store(
                    false, std::memory_order_release);
#endif
                return service_running_.load(std::memory_order_acquire);
            }
            if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
                for (std::uint32_t attempt = 0;
                     attempt < kSourceCapacity * 2;
                     ++attempt) {
                    if (service_.dispatch_once().status !=
                        audio::SampleStreamAsyncDispatchStatus::Queued) {
                        break;
                    }
                }
            }
            std::this_thread::yield();
        }
        service_.drain_completions();
        const bool ready = view.window->ready_page_for_frame(
            source.source_generation, tail_frame).valid;
        (void) service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        reverse_prewarm_pending_for_test_.store(false, std::memory_order_release);
#endif
        return ready && service_running_.load(std::memory_order_acquire);
    }

    void rollback_unpublished_source(
        audio::SampleStreamSourceToken source) noexcept {
        (void) service_.cancel_pending_source_demands(source);
        service_.drain_completions();
        if (service_.discard_unpublished_source(source)) return;
        for (auto& pending : unpublished_rollbacks_) {
            if (pending.source_id == 0) {
                pending = source;
#if defined(PULP_SAMPLER_TEST_HOOKS)
                unpublished_rollback_count_for_test_.fetch_add(
                    1, std::memory_order_release);
#endif
                return;
            }
        }
    }

    void collect_unpublished_rollbacks() noexcept {
        for (auto& pending : unpublished_rollbacks_) {
            if (pending.source_id == 0) continue;
            (void) service_.cancel_pending_source_demands(pending);
            if (service_.discard_unpublished_source(pending)) {
                pending = {};
#if defined(PULP_SAMPLER_TEST_HOOKS)
                unpublished_rollback_count_for_test_.fetch_sub(
                    1, std::memory_order_release);
#endif
            }
        }
    }

    void retire_sources() noexcept {
        const auto current = published_source_.read();
        const auto acknowledged =
            stream_audio_ack_selection_.load(std::memory_order_acquire);
        for (auto& owned : slots_) {
            if (!owned || !owned->occupied) continue;
            auto& slot = *owned;
            const bool current_slot =
                current.kind == SamplerPublishedSourceKind::Streamed &&
                current.selection_generation == slot.selection_generation;
            if (!current_slot && !slot.retirement_scheduled &&
                acknowledged > slot.selection_generation) {
                slot.asset.release();
                if (service_.retire_source_after_asset_unpublish(slot.source) ==
                    audio::SampleStreamSourceRetireStatus::Scheduled) {
                    slot.retirement_scheduled = true;
                }
            }
        }

        (void) service_.collect_retired_sources();
        for (auto& owned : slots_) {
            if (!owned || !owned->occupied || !owned->retirement_scheduled)
                continue;
            if (service_.cache_service().contains_source(owned->source)) continue;
            owned->source = {};
            owned->selection_generation = 0;
            owned->occupied = false;
            owned->retirement_scheduled = false;
            sources_retired_.fetch_add(1, std::memory_order_relaxed);
            active_sources_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

}  // namespace pulp::examples
