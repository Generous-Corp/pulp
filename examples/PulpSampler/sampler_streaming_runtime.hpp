#pragma once

#include <pulp/audio/buffer.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/published_sample_store.hpp>
#include <pulp/audio/sample_asset.hpp>
#include <pulp/audio/sample_preload_contract.hpp>
#include <pulp/audio/sample_stream_async_service.hpp>
#include <pulp/audio/sample_stream_voice_reader.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>
#include <pulp/runtime/seqlock.hpp>

#include "sampler_mip_pyramid.hpp"
#include "sampler_api.hpp"
#include "sampler_stream_mip_sidecar.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>

namespace pulp::examples {

enum class SamplerPublishedSourceKind : std::uint8_t {
    None,
    Resident,
    Streamed,
};

struct SamplerStreamMipLevelView {
    audio::SampleAssetView asset{};
    std::uint32_t octave = 0;

    bool valid() const noexcept {
        return octave != 0 && asset.valid();
    }
};

struct SamplerStreamMipPyramidView {
    std::array<SamplerStreamMipLevelView, SamplerStreamMipSidecar::kMaximumLevels> levels{};
    std::uint32_t level_count = 0;

    const SamplerStreamMipLevelView* level(std::uint32_t octave) const noexcept {
        if (octave == 0 || octave > level_count || octave > levels.size())
            return nullptr;
        const auto& candidate = levels[octave - 1];
        return candidate.valid() && candidate.octave == octave ? &candidate : nullptr;
    }
};

static_assert(std::is_trivially_copyable_v<SamplerStreamMipPyramidView>);

struct SamplerPublishedSource {
    SamplerPublishedSourceKind kind = SamplerPublishedSourceKind::None;
    std::uint64_t selection_generation = 0;
    audio::PublishedSampleView resident{};
    audio::SampleAssetView streamed{};
    SamplerMipPyramidView resident_mips{};
    SamplerStreamMipPyramidView streamed_mips{};
};

static_assert(std::is_trivially_copyable_v<SamplerPublishedSource>);

struct PulpSamplerStreamStats {
    audio::SampleStreamAsyncPrepareStatus service_prepare_status =
        audio::SampleStreamAsyncPrepareStatus::InvalidConfiguration;
    std::uint32_t decode_worker_count = 0;
    std::uint64_t pages_published = 0;
    std::uint64_t starved_output_frames = 0;
    // Output-supply symptoms. A decode failure can later cause one of these,
    // so it is intentionally allowed to co-occur with decode_failure_events.
    std::uint64_t service_starvation_events = 0;
    // Root-cause failures reported by the non-RT worker pool, independent of
    // whether contract lead hid the failed attempt from audible output.
    std::uint64_t decode_failure_events = 0;
    std::uint64_t invalid_preload_contract_events = 0;
    std::uint64_t stale_generation_events = 0;
    std::uint64_t normal_end_of_source_events = 0;
    std::uint64_t invalid_render_contract_events = 0;
    std::uint64_t cache_pages_retired = 0;
    std::uint64_t cache_pages_reused = 0;
    std::uint64_t decode_source_outstanding_high_water = 0;
    std::uint64_t decode_completed_frames = 0;
    std::uint64_t same_source_reader_concurrency_high_water = 0;
    std::uint64_t cache_async_reservations_high_water = 0;
    std::uint64_t active_reservations_high_water = 0;
    std::uint64_t aggregate_rate_admission_rejections = 0;
    std::uint64_t aggregate_rate_automation_rejections = 0;
    std::uint64_t decode_scratch_bytes = 0;
    std::uint64_t total_memory_capacity_bytes = 0;
    std::uint64_t current_total_memory_bytes = 0;
    std::uint64_t peak_total_memory_bytes = 0;
    std::uint64_t sources_retired = 0;
    std::uint64_t active_sources = 0;
    std::uint64_t preload_frames = 0;
    audio::SampleMemoryGovernorStats memory{};
};

struct PulpSamplerTestAccess;

struct SamplerAudioCallbackToken {
    std::uint64_t callback_generation = 0;
    std::uint64_t page_read_epoch = 0;

    bool valid() const noexcept { return callback_generation != 0; }
};

class SamplerStreamingRuntime {
  public:
    static constexpr std::size_t kCommandCapacity = 256;
    using CommandInbox = audio::SampleStreamCommandInbox<kCommandCapacity>;

    ~SamplerStreamingRuntime() {
        release();
    }

    static PulpSamplerPrepareResult estimate_prepare(
        float host_sample_rate,
        std::uint32_t maximum_host_block_frames,
        std::uint64_t streaming_memory_budget_bytes = 0) noexcept {
        return derive_prepare(host_sample_rate, maximum_host_block_frames,
                              streaming_memory_budget_bytes).result;
    }

    bool prepare(float host_sample_rate,
                 std::uint32_t maximum_host_block_frames) {
        return prepare_checked(host_sample_rate, maximum_host_block_frames)
            .prepared();
    }

    PulpSamplerPrepareResult prepare_checked(
        float host_sample_rate,
        std::uint32_t maximum_host_block_frames,
        std::uint64_t streaming_memory_budget_bytes = 0) {
        release();
        const auto derived = derive_prepare(
            host_sample_rate, maximum_host_block_frames,
            streaming_memory_budget_bytes);
        auto result = derived.result;
        if (!result.prepared()) return result;
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_prepare_for_test_.exchange(false,
                                                 std::memory_order_acq_rel)) {
            result.status = PulpSamplerPrepareStatus::AllocationFailure;
            return result;
        }
#endif
        host_sample_rate_ = host_sample_rate;
        maximum_host_block_frames_ = maximum_host_block_frames;
        selection_generation_ = 0;
        stream_audio_ack_selection_.store(0, std::memory_order_relaxed);
        audio_active_generation_.store(1, std::memory_order_relaxed);
        audio_completed_generation_.store(0, std::memory_order_relaxed);
        next_audio_generation_.store(1, std::memory_order_relaxed);
        audio_completed_page_read_epoch_ = 0;
        pages_published_.store(0, std::memory_order_relaxed);
        starved_frames_.store(0, std::memory_order_relaxed);
        service_starvation_events_.store(0, std::memory_order_relaxed);
        decode_failure_events_.store(0, std::memory_order_relaxed);
        invalid_preload_contract_events_.store(0, std::memory_order_relaxed);
        stale_generation_events_.store(0, std::memory_order_relaxed);
        normal_end_of_source_events_.store(0, std::memory_order_relaxed);
        invalid_render_contract_events_.store(0, std::memory_order_relaxed);
        cache_pages_retired_.store(0, std::memory_order_relaxed);
        cache_pages_reused_.store(0, std::memory_order_relaxed);
        decode_source_outstanding_high_water_.store(0, std::memory_order_relaxed);
        decode_completed_frames_.store(0, std::memory_order_relaxed);
        same_source_reader_concurrency_high_water_.store(0, std::memory_order_relaxed);
        cache_async_reservations_high_water_.store(0, std::memory_order_relaxed);
        active_reservations_high_water_.store(0, std::memory_order_relaxed);
        aggregate_rate_admission_rejections_.store(0, std::memory_order_relaxed);
        aggregate_rate_automation_rejections_.store(0, std::memory_order_relaxed);
        decode_scratch_bytes_.store(0, std::memory_order_relaxed);
        sources_retired_.store(0, std::memory_order_relaxed);
        active_sources_.store(0, std::memory_order_relaxed);
        preload_frames_.store(0, std::memory_order_relaxed);
        service_dispatch_paused_.store(false, std::memory_order_relaxed);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        fail_next_stream_decode_for_test_.store(false, std::memory_order_relaxed);
        reverse_prewarm_pending_for_test_.store(false, std::memory_order_relaxed);
        block_next_reverse_decode_for_test_.store(false, std::memory_order_relaxed);
        reverse_decode_entered_for_test_.store(false, std::memory_order_relaxed);
        release_reverse_decode_for_test_.store(true, std::memory_order_relaxed);
        unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
        unpublished_rollback_attempts_for_test_.store(0, std::memory_order_relaxed);
        fail_after_stream_member_count_for_test_.store(-1, std::memory_order_relaxed);
        pause_before_bundle_publish_for_test_.store(false, std::memory_order_relaxed);
        bundle_publish_paused_ack_for_test_.store(false, std::memory_order_relaxed);
        service_command_drain_paused_for_test_.store(false, std::memory_order_relaxed);
        service_command_drain_paused_ack_for_test_.store(false, std::memory_order_relaxed);
        file_stage_paused_for_test_.store(false, std::memory_order_relaxed);
        file_stage_paused_ack_for_test_.store(false, std::memory_order_relaxed);
        file_stage_attempts_for_test_.store(0, std::memory_order_relaxed);
        throw_during_file_stage_for_test_.store(false, std::memory_order_relaxed);
        reverse_prewarm_timeout_override_for_test_ = false;
        reverse_prewarm_timeout_for_test_ = std::chrono::milliseconds{0};
#endif
        unpublished_rollbacks_.fill({});
        published_source_.write({});
        file_request_result_ = {};

        page_frames_ = derived.page_frames;
        const auto governor_capacity =
            result.configured_streaming_memory_bytes - derived.decode_scratch_bytes;
        if (!memory_governor_.prepare(governor_capacity)) {
            result.status = PulpSamplerPrepareStatus::AllocationFailure;
            release();
            return result;
        }
        decode_scratch_bytes_.store(derived.decode_scratch_bytes,
                                    std::memory_order_relaxed);

        const auto service_config = audio::SampleStreamAsyncServiceConfig{
            .cache =
                {
                    .scheduler_capacity = kCommandCapacity,
                    .page_memory_budget_bytes = 0,
                    .memory_governor = memory_governor_.handle(),
                },
            .decode =
                {
                    .worker_count = kWorkerCount,
                    .source_capacity = kSourceCapacity,
                    .maximum_channels = kMaximumChannels,
                    .maximum_frames_per_job = page_frames_,
                    .maximum_outstanding_jobs_per_source =
                        kMaximumOutstandingJobsPerSource,
                },
        };
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (fail_next_service_prepare_for_test_.exchange(
                false, std::memory_order_acq_rel)) {
            result.status = PulpSamplerPrepareStatus::StreamingServiceUnavailable;
            release();
            return result;
        }
#endif
        const auto service_prepare = service_.prepare_checked(service_config);
        service_prepare_status_.store(service_prepare.status,
                                      std::memory_order_relaxed);
        service_ready_ = service_prepare.prepared();
        if (!service_ready_) {
            result.status = PulpSamplerPrepareStatus::AllocationFailure;
            release();
            return result;
        }
        try {
#if defined(PULP_SAMPLER_TEST_HOOKS)
            if (fail_next_prepare_allocation_for_test_.exchange(
                    false, std::memory_order_acq_rel)) {
                throw std::bad_alloc{};
            }
#endif
            for (auto& slot : slots_)
                slot = std::make_unique<StreamedSlot>();
            service_running_.store(true, std::memory_order_release);
#if defined(PULP_SAMPLER_TEST_HOOKS)
            if (fail_next_thread_start_for_test_.exchange(
                    false, std::memory_order_acq_rel)) {
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            }
#endif
            service_thread_ = std::thread([this] { service_loop(); });
        } catch (...) {
            result.status = PulpSamplerPrepareStatus::AllocationFailure;
            release();
            return result;
        }
        return result;
    }

    void release() noexcept {
        {
            std::lock_guard lock(file_request_mutex_);
            service_running_.store(false, std::memory_order_release);
            file_request_complete_ = true;
            file_request_result_.status = PulpSamplerLoadStatus::ShuttingDown;
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        release_reverse_decode_for_test_.store(true, std::memory_order_release);
        pause_before_bundle_publish_for_test_.store(false, std::memory_order_release);
        file_stage_paused_for_test_.store(false, std::memory_order_release);
#endif
        service_wake_.notify_all();
        file_request_changed_.notify_all();
        if (service_thread_.joinable())
            service_thread_.join();
        std::lock_guard source_lock(source_load_mutex_);

        published_source_.write({});
        if (service_ready_) {
            service_.update_audio_generations(
                audio_active_generation_.load(std::memory_order_acquire),
                audio_completed_generation_.load(std::memory_order_acquire));
            service_.drain_commands(commands_);
        }
        service_.release();
        decode_scratch_bytes_.store(0, std::memory_order_relaxed);
        // SampleStreamAsyncService::release() joins every decode worker while
        // the decode pool deliberately retains its telemetry until prepare().
        // Snapshot only after that join so a late short-read cannot disappear.
        decode_failure_events_.store(
            service_.decode_telemetry().decode_errors,
            std::memory_order_relaxed);
        unpublished_rollbacks_.fill({});
#if defined(PULP_SAMPLER_TEST_HOOKS)
        unpublished_rollback_count_for_test_.store(0, std::memory_order_relaxed);
#endif
        for (auto& slot : slots_) {
            if (slot) {
                for (auto& member : slot->members)
                    member.asset.release();
            }
            slot.reset();
        }
        retained_stream_recipe_ = {};
        service_ready_ = false;
        active_sources_.store(0, std::memory_order_relaxed);
        {
            std::lock_guard lock(file_request_mutex_);
            file_request_pending_ = false;
            file_request_prepared_.reset();
        }
        (void) memory_governor_.release();
    }

    template <typename Loader, typename ViewReader, typename Publisher>
    bool load_and_publish_resident(Loader&& loader, ViewReader&& read_view,
                                   const SamplerMipPyramidView& resident_mips,
                                   Publisher&& on_published) {
        std::lock_guard lock(source_load_mutex_);
        if (!loader())
            return false;
        const auto resident = read_view();
        if (!resident.valid)
            return false;
        const auto generation = ++selection_generation_;
        on_published(generation);
        published_source_.write({
            .kind = SamplerPublishedSourceKind::Resident,
            .selection_generation = generation,
            .resident = resident,
            .resident_mips = resident_mips,
        });
        retained_stream_recipe_ = {};
        service_wake_.notify_all();
        return true;
    }

    bool republish_resident(const SamplerPublishedSource& retained) noexcept {
        if (retained.kind != SamplerPublishedSourceKind::Resident ||
            !retained.resident.valid) {
            return false;
        }
        std::lock_guard lock(source_load_mutex_);
        const auto generation = ++selection_generation_;
        auto published = retained;
        published.selection_generation = generation;
        published_source_.write(published);
        service_wake_.notify_all();
        return true;
    }

    PulpSamplerLoadResult load_sample_file_result(std::string_view path) {
        PulpSamplerLoadResult result;
        if (path.empty()) {
            result.status = PulpSamplerLoadStatus::EmptyPath;
            return result;
        }
        std::lock_guard source_lock(source_load_mutex_);
        if (!service_running_.load(std::memory_order_acquire)) {
            result.status = PulpSamplerLoadStatus::NotPrepared;
            return result;
        }
        if (active_sources_.load(std::memory_order_acquire) >= kBundleSlotCount) {
            result.status = PulpSamplerLoadStatus::BundleCapacityExceeded;
            return result;
        }

        StagedStreamedFile staged;
        try {
            staged = stage_streamed_file(path);
        } catch (const std::bad_alloc&) {
            result.status = PulpSamplerLoadStatus::AllocationFailure;
            return result;
        } catch (...) {
            result.status = PulpSamplerLoadStatus::InternalFailure;
            return result;
        }
        if (!staged.prepared)
            return staged.result;
        return submit_staged_file(std::move(staged));
    }

    bool load_sample_file(std::string_view path) {
        return load_sample_file_result(path).loaded();
    }

    bool restore_retained_streamed_source() {
        std::lock_guard source_lock(source_load_mutex_);
        if (!service_running_.load(std::memory_order_acquire) ||
            !retained_stream_recipe_.valid()) {
            return false;
        }
        auto staged = stage_retained_streamed_file();
        return staged.prepared && submit_staged_file(std::move(staged)).loaded();
    }

    bool clone_published_source_to(SamplerStreamingRuntime& target) {
        std::lock_guard source_lock(source_load_mutex_);
        const auto retained = published_source_.read();
        if (retained.kind == SamplerPublishedSourceKind::None) return true;
        if (retained.kind == SamplerPublishedSourceKind::Resident)
            return target.republish_resident(retained);
        if (retained.kind != SamplerPublishedSourceKind::Streamed ||
            !retained_stream_recipe_.valid()) {
            return false;
        }
        target.retained_stream_recipe_ = retained_stream_recipe_;
        return target.restore_retained_streamed_source();
    }

#if defined(PULP_SAMPLER_TEST_HOOKS)
    void pause_file_stage_for_test(bool paused) noexcept {
        file_stage_paused_for_test_.store(paused, std::memory_order_release);
    }

    bool file_stage_paused_for_test() const noexcept {
        return file_stage_paused_ack_for_test_.load(std::memory_order_acquire);
    }

    bool has_retained_streamed_source_for_test() const noexcept {
        return retained_stream_recipe_.valid();
    }

    void fail_next_retained_source_publish_for_test() noexcept {
        fail_after_stream_member_count_for_test_.store(
            0, std::memory_order_release);
    }

    void fail_next_prepare_for_test() noexcept {
        fail_next_prepare_for_test_.store(true, std::memory_order_release);
    }

    void fail_next_thread_start_for_test() noexcept {
        fail_next_thread_start_for_test_.store(true, std::memory_order_release);
    }

    void fail_next_service_prepare_for_test() noexcept {
        fail_next_service_prepare_for_test_.store(true, std::memory_order_release);
    }

    void fail_next_slot_allocation_for_test() noexcept {
        fail_next_prepare_allocation_for_test_.store(true,
                                                     std::memory_order_release);
    }

    bool fully_released_for_test() const noexcept {
        if (service_ready_ || service_running_.load(std::memory_order_acquire) ||
            service_thread_.joinable() || service_.prepared()) return false;
        return std::all_of(slots_.begin(), slots_.end(),
                           [](const auto& slot) { return slot == nullptr; });
    }
#endif

    SamplerPublishedSource published_source() const noexcept {
        return published_source_.read();
    }

    PulpSamplerStreamStats stats() const noexcept {
        const auto memory = memory_governor_.stats();
        const auto decode_scratch_bytes =
            decode_scratch_bytes_.load(std::memory_order_relaxed);
        return {
            .service_prepare_status =
                service_prepare_status_.load(std::memory_order_relaxed),
            .decode_worker_count = service_ready_ ? kWorkerCount : 0,
            .pages_published = pages_published_.load(std::memory_order_relaxed),
            .starved_output_frames = starved_frames_.load(std::memory_order_relaxed),
            .service_starvation_events =
                service_starvation_events_.load(std::memory_order_relaxed),
            .decode_failure_events =
                decode_failure_events_.load(std::memory_order_relaxed),
            .invalid_preload_contract_events =
                invalid_preload_contract_events_.load(std::memory_order_relaxed),
            .stale_generation_events =
                stale_generation_events_.load(std::memory_order_relaxed),
            .normal_end_of_source_events =
                normal_end_of_source_events_.load(std::memory_order_relaxed),
            .invalid_render_contract_events =
                invalid_render_contract_events_.load(std::memory_order_relaxed),
            .cache_pages_retired =
                cache_pages_retired_.load(std::memory_order_relaxed),
            .cache_pages_reused =
                cache_pages_reused_.load(std::memory_order_relaxed),
            .decode_source_outstanding_high_water =
                decode_source_outstanding_high_water_.load(std::memory_order_relaxed),
            .decode_completed_frames =
                decode_completed_frames_.load(std::memory_order_relaxed),
            .same_source_reader_concurrency_high_water =
                same_source_reader_concurrency_high_water_.load(
                    std::memory_order_relaxed),
            .cache_async_reservations_high_water =
                cache_async_reservations_high_water_.load(std::memory_order_relaxed),
            .active_reservations_high_water =
                active_reservations_high_water_.load(std::memory_order_relaxed),
            .aggregate_rate_admission_rejections =
                aggregate_rate_admission_rejections_.load(std::memory_order_relaxed),
            .aggregate_rate_automation_rejections =
                aggregate_rate_automation_rejections_.load(std::memory_order_relaxed),
            .decode_scratch_bytes = decode_scratch_bytes,
            .total_memory_capacity_bytes =
                memory.capacity_bytes + decode_scratch_bytes,
            .current_total_memory_bytes =
                memory.current_total_bytes + decode_scratch_bytes,
            .peak_total_memory_bytes =
                memory.peak_total_bytes + decode_scratch_bytes,
            .sources_retired = sources_retired_.load(std::memory_order_relaxed),
            .active_sources = active_sources_.load(std::memory_order_relaxed),
            .preload_frames = preload_frames_.load(std::memory_order_relaxed),
            .memory = memory,
        };
    }

    PulpSamplerPreloadPolicy preload_policy() const noexcept {
        PulpSamplerPreloadPolicy policy;
        const auto source = published_source_.read();
        if (source.kind != SamplerPublishedSourceKind::Streamed)
            return policy;
        policy.selection_generation = source.selection_generation;
        const auto& contract = source.streamed.preload_contract;
        const auto evaluated = audio::evaluate_sample_preload_contract(contract);
        policy.source_sample_rate = contract.source_sample_rate;
        policy.host_sample_rate = contract.host_sample_rate;
        policy.maximum_playback_ratio = contract.maximum_playback_ratio;
        policy.certified_io_latency_seconds = contract.certified_io_latency_seconds;
        policy.scheduler_margin_seconds = contract.scheduler_margin_seconds;
        policy.decoder_latency_seconds = contract.decoder_latency_seconds;
        policy.maximum_host_block_frames = contract.maximum_host_block_frames;
        policy.interpolation_guard_frames = contract.interpolation_guard_frames;
        policy.loop_prefetch_guard_frames = contract.loop_prefetch_guard_frames;
        policy.required_preload_frames = evaluated.valid()
            ? evaluated.required_preload_frames : 0;
        policy.configured_preload_frames = contract.configured_preload_frames;
        policy.page_frames = source.streamed.stream_source.page_frames;
        return policy;
    }

    CommandInbox& command_inbox() noexcept {
        return commands_;
    }

    SamplerAudioCallbackToken begin_audio_callback() noexcept {
        // Capture the page barrier before any page lookup in this callback.
        // A retirement published after this load requires a later callback to
        // complete before that page can be cleared and reused.
        const auto page_read_epoch = service_.begin_audio_page_read();
        const auto generation = next_audio_generation_.fetch_add(1, std::memory_order_relaxed) + 1;
        audio_active_generation_.store(generation, std::memory_order_release);
        return {generation, page_read_epoch};
    }

    void complete_audio_callback(SamplerAudioCallbackToken token) noexcept {
        if (!token.valid()) return;
        // All page reads are complete before this acknowledgement. Source
        // generation completion remains a separate retirement domain.
        if (token.page_read_epoch != audio_completed_page_read_epoch_) {
            service_.complete_audio_page_read(token.page_read_epoch);
            audio_completed_page_read_epoch_ = token.page_read_epoch;
        }
        audio_completed_generation_.store(token.callback_generation,
                                          std::memory_order_release);
    }

    void acknowledge_selection(std::uint64_t generation) noexcept {
        stream_audio_ack_selection_.store(generation, std::memory_order_release);
    }

    void record_voice_outcome(audio::SampleStreamVoiceOutcomeClass outcome,
                              std::uint64_t starved_frames = 0) noexcept {
        switch (outcome) {
        case audio::SampleStreamVoiceOutcomeClass::None:
            return;
        case audio::SampleStreamVoiceOutcomeClass::ServiceStarvation:
            service_starvation_events_.fetch_add(1, std::memory_order_relaxed);
            starved_frames_.fetch_add(starved_frames, std::memory_order_relaxed);
            return;
        case audio::SampleStreamVoiceOutcomeClass::InvalidPreloadContract:
            invalid_preload_contract_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        case audio::SampleStreamVoiceOutcomeClass::StaleGeneration:
            stale_generation_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        case audio::SampleStreamVoiceOutcomeClass::NormalEndOfSource:
            normal_end_of_source_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        case audio::SampleStreamVoiceOutcomeClass::InvalidRenderContract:
            invalid_render_contract_events_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    void record_aggregate_rate_admission_rejection() noexcept {
        aggregate_rate_admission_rejections_.fetch_add(1, std::memory_order_relaxed);
    }

    void record_aggregate_rate_automation_rejection() noexcept {
        aggregate_rate_automation_rejections_.fetch_add(1, std::memory_order_relaxed);
    }

    static constexpr double maximum_pitch_ratio() noexcept {
        return kMaximumPitchRatio;
    }

    static constexpr std::uint32_t maximum_voice_count() noexcept {
        return kMaximumVoices;
    }

    static constexpr double maximum_source_sample_rate() noexcept {
        return kMaximumSourceRate;
    }

    static constexpr double certified_decoder_latency_seconds() noexcept {
        return kDecoderLatencySeconds;
    }

  private:
    friend struct PulpSamplerTestAccess;

    static constexpr std::uint32_t kWorkerCount = 2;
    static constexpr std::uint32_t kMaximumOutstandingJobsPerSource = 8;
    static constexpr std::uint32_t kBundleSlotCount = 2;
    static constexpr std::uint32_t kMaximumBundleMembers =
        1 + SamplerStreamMipSidecar::kMaximumLevels;
    static constexpr std::uint32_t kSourceCapacity = kBundleSlotCount * kMaximumBundleMembers;
    static constexpr std::uint32_t kMaximumVoices = 8;
    static constexpr std::uint32_t kPagesPerVoiceWorkingSet =
        audio::kSampleStreamVoiceMaxPageDemands;
    // A full-sample wrap crossfade reads two distant contiguous regions. Each
    // region can touch one partial page at either edge in addition to the pages
    // spanned by the block's source advance.
    static constexpr std::uint32_t kCrossfadeReadRegionCount = 2;
    static constexpr std::uint32_t kBoundaryPageDemandsPerRegion = 2;
    static constexpr std::uint32_t kSourceAdvancePageDemandsPerRegion =
        audio::kSampleStreamVoiceMaxPageDemands / kCrossfadeReadRegionCount -
        kBoundaryPageDemandsPerRegion;
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

    static_assert(audio::kSampleStreamVoiceMaxPageDemands % kCrossfadeReadRegionCount == 0);
    static_assert(kSourceAdvancePageDemandsPerRegion > 0);

    struct DerivedPrepare {
        PulpSamplerPrepareResult result{};
        std::uint64_t page_frames = 0;
        std::uint64_t decode_scratch_bytes = 0;
    };

    static DerivedPrepare derive_prepare(
        float host_sample_rate, std::uint32_t maximum_host_block_frames,
        std::uint64_t configured_budget) noexcept {
        DerivedPrepare derived;
        auto& result = derived.result;
        if (!std::isfinite(host_sample_rate) || host_sample_rate <= 0.0f ||
            maximum_host_block_frames == 0) {
            result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
            return derived;
        }
        const auto maximum_source_frames_per_block =
            std::ceil(static_cast<double>(maximum_host_block_frames) *
                      kMaximumPitchRatio * kMaximumSourceRate /
                      static_cast<double>(host_sample_rate));
        if (!std::isfinite(maximum_source_frames_per_block) ||
            maximum_source_frames_per_block < 0.0 ||
            maximum_source_frames_per_block >
                static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
            return derived;
        }
        const auto maximum_preload = audio::evaluate_sample_preload_contract({
            .source_sample_rate = kMaximumSourceRate,
            .host_sample_rate = static_cast<double>(host_sample_rate),
            .maximum_playback_ratio = kMaximumPitchRatio,
            .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
            .scheduler_margin_seconds = kSchedulerMarginSeconds,
            .decoder_latency_seconds = kDecoderLatencySeconds,
            .maximum_host_block_frames = maximum_host_block_frames,
            .interpolation_guard_frames = audio::kHighQualitySampleSincHalfWidth,
            .loop_prefetch_guard_frames =
                static_cast<std::uint64_t>(maximum_source_frames_per_block),
        });
        if (!maximum_preload.valid()) {
            result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
            return derived;
        }
        const auto cache_working_set_page_frames = std::ceil(
            (static_cast<double>(maximum_preload.required_preload_frames) +
             maximum_source_frames_per_block) /
            static_cast<double>(kPagesPerVoiceWorkingSet - 1));
        if (!std::isfinite(cache_working_set_page_frames) ||
            cache_working_set_page_frames < 0.0 ||
            cache_working_set_page_frames >
                static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
            result.status = PulpSamplerPrepareStatus::InvalidHostConfiguration;
            return derived;
        }
        derived.page_frames = std::max<std::uint64_t>({
            kDefaultPageFrames,
            static_cast<std::uint64_t>(maximum_source_frames_per_block /
                                       kSourceAdvancePageDemandsPerRegion) + 1,
            static_cast<std::uint64_t>(cache_working_set_page_frames)});
        const auto page_budget = audio::checked_sample_storage_bytes(
            kMaximumChannels, derived.page_frames,
            kSourceCapacity * kCachePagesPerSource);
        const auto preload_budget = audio::checked_sample_storage_bytes(
            kMaximumChannels, maximum_preload.required_preload_frames,
            kSourceCapacity);
        const auto decode_budget = audio::checked_sample_storage_bytes(
            kMaximumChannels, derived.page_frames,
            static_cast<std::uint64_t>(kWorkerCount) *
                kMaximumOutstandingJobsPerSource);
        if (!page_budget || !preload_budget || !decode_budget ||
            *preload_budget > std::numeric_limits<std::uint64_t>::max() - *page_budget ||
            *decode_budget > std::numeric_limits<std::uint64_t>::max() -
                (*page_budget + *preload_budget)) {
            result.status = PulpSamplerPrepareStatus::AllocationFailure;
            return derived;
        }
        result.required_streaming_memory_bytes =
            *page_budget + *preload_budget + *decode_budget;
        result.configured_streaming_memory_bytes = configured_budget == 0
            ? result.required_streaming_memory_bytes : configured_budget;
        derived.decode_scratch_bytes = *decode_budget;
        result.status = result.configured_streaming_memory_bytes <
                                result.required_streaming_memory_bytes
            ? PulpSamplerPrepareStatus::StreamingMemoryBudgetTooSmall
            : PulpSamplerPrepareStatus::Ok;
        return derived;
    }

    struct StreamedMember {
        audio::SampleAsset asset;
        audio::SampleStreamSourceToken source{};
        bool present = false;
        bool retirement_scheduled = false;
    };

    struct StreamedSlot {
        std::array<StreamedMember, kMaximumBundleMembers> members{};
        std::uint32_t member_count = 0;
        std::uint64_t selection_generation = 0;
        bool occupied = false;
    };

    struct PreparedStreamedFile {
        std::array<audio::FileFrameReader, kMaximumBundleMembers> files{};
        std::array<double, kMaximumBundleMembers> logical_rates{};
        std::array<std::uint32_t, kMaximumBundleMembers> octaves{};
        std::array<audio::SamplePreloadContract, kMaximumBundleMembers> contracts{};
        // Leases precede buffers so reverse member destruction frees sample
        // storage before returning its bytes to the shared governor.
        std::array<audio::SampleMemoryLease, kMaximumBundleMembers> preload_leases{};
        std::array<audio::Buffer<float>, kMaximumBundleMembers> preloads{};
        std::array<std::uint64_t, kMaximumBundleMembers> preload_counts{};
        std::uint32_t member_count = 0;
    };

    struct StreamedSourceRecipe {
        std::array<audio::FileFrameReader, kMaximumBundleMembers> files{};
        std::array<double, kMaximumBundleMembers> logical_rates{};
        std::array<std::uint32_t, kMaximumBundleMembers> octaves{};
        std::uint32_t member_count = 0;

        bool valid() const noexcept {
            return member_count != 0 && member_count <= files.size() &&
                   files[0].valid && files[0].binding.read != nullptr;
        }
    };

    struct StagedStreamedFile {
        std::unique_ptr<PreparedStreamedFile> prepared;
        PulpSamplerLoadResult result{};
    };

    enum class ReversePrewarmStatus : std::uint8_t {
        Ready,
        ScheduleFailed,
        TimedOut,
        ShuttingDown,
    };

    template <std::size_t Size>
    static void copy_fixed_name(std::array<char, Size>& destination,
                                std::string_view source) noexcept {
        destination.fill('\0');
        if constexpr (Size > 0) {
            const auto count = std::min(source.size(), Size - 1);
            std::copy_n(source.data(), count, destination.data());
        }
    }

    static bool has_registered_audio_reader(std::string_view path) {
        auto extension = std::filesystem::path(path).extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return audio::FormatRegistry::instance().find_reader(extension) != nullptr;
    }

    runtime::SeqLock<SamplerPublishedSource> published_source_;
    std::atomic<std::uint64_t> stream_audio_ack_selection_{0};
    std::atomic<std::uint64_t> audio_active_generation_{1};
    std::atomic<std::uint64_t> audio_completed_generation_{0};
    std::atomic<std::uint64_t> next_audio_generation_{1};
    // Audio-thread-only acknowledgement cache. Page retirement is infrequent,
    // so avoid writing the cross-thread completion atomic on unchanged blocks.
    std::uint64_t audio_completed_page_read_epoch_ = 0;
    std::atomic<std::uint64_t> pages_published_{0};
    std::atomic<audio::SampleStreamAsyncPrepareStatus> service_prepare_status_{
        audio::SampleStreamAsyncPrepareStatus::InvalidConfiguration};
    std::atomic<std::uint64_t> starved_frames_{0};
    std::atomic<std::uint64_t> service_starvation_events_{0};
    std::atomic<std::uint64_t> decode_failure_events_{0};
    std::atomic<std::uint64_t> invalid_preload_contract_events_{0};
    std::atomic<std::uint64_t> stale_generation_events_{0};
    std::atomic<std::uint64_t> normal_end_of_source_events_{0};
    std::atomic<std::uint64_t> invalid_render_contract_events_{0};
    std::atomic<std::uint64_t> cache_pages_retired_{0};
    std::atomic<std::uint64_t> cache_pages_reused_{0};
    std::atomic<std::uint64_t> decode_source_outstanding_high_water_{0};
    std::atomic<std::uint64_t> decode_completed_frames_{0};
    std::atomic<std::uint64_t> same_source_reader_concurrency_high_water_{0};
    std::atomic<std::uint64_t> cache_async_reservations_high_water_{0};
    std::atomic<std::uint64_t> active_reservations_high_water_{0};
    std::atomic<std::uint64_t> aggregate_rate_admission_rejections_{0};
    std::atomic<std::uint64_t> aggregate_rate_automation_rejections_{0};
    std::atomic<std::uint64_t> decode_scratch_bytes_{0};
    std::atomic<std::uint64_t> sources_retired_{0};
    std::atomic<std::uint64_t> active_sources_{0};
    std::atomic<std::uint64_t> preload_frames_{0};
    audio::SampleMemoryGovernor memory_governor_;
    audio::SampleStreamAsyncService<> service_;
    CommandInbox commands_;
    std::array<std::unique_ptr<StreamedSlot>, kBundleSlotCount> slots_{};
    std::array<audio::SampleStreamSourceToken, kSourceCapacity> unpublished_rollbacks_{};
    std::thread service_thread_;
    std::atomic<bool> service_running_{false};
    std::atomic<bool> service_dispatch_paused_{false};
#if defined(PULP_SAMPLER_TEST_HOOKS)
    std::atomic<bool> fail_next_service_prepare_for_test_{false};
    std::atomic<bool> fail_next_prepare_allocation_for_test_{false};
    std::atomic<bool> fail_next_thread_start_for_test_{false};
    std::atomic<bool> fail_next_stream_decode_for_test_{false};
    std::atomic<bool> reverse_prewarm_pending_for_test_{false};
    std::atomic<bool> block_next_reverse_decode_for_test_{false};
    std::atomic<bool> reverse_decode_entered_for_test_{false};
    std::atomic<bool> release_reverse_decode_for_test_{true};
    std::atomic<std::uint32_t> unpublished_rollback_count_for_test_{0};
    std::atomic<std::uint64_t> unpublished_rollback_attempts_for_test_{0};
    std::atomic<int> fail_after_stream_member_count_for_test_{-1};
    std::atomic<bool> pause_before_bundle_publish_for_test_{false};
    std::atomic<bool> bundle_publish_paused_ack_for_test_{false};
    std::atomic<bool> service_command_drain_paused_for_test_{false};
    std::atomic<bool> service_command_drain_paused_ack_for_test_{false};
    std::atomic<bool> file_stage_paused_for_test_{false};
    std::atomic<bool> file_stage_paused_ack_for_test_{false};
    std::atomic<std::uint64_t> file_stage_attempts_for_test_{0};
    std::atomic<bool> throw_during_file_stage_for_test_{false};
    std::atomic<bool> fail_next_prepare_for_test_{false};
    bool reverse_prewarm_timeout_override_for_test_ = false;
    std::chrono::milliseconds reverse_prewarm_timeout_for_test_{0};
#endif
    bool service_ready_ = false;
    float host_sample_rate_ = 44100.0f;
    std::uint32_t maximum_host_block_frames_ = 512;
    std::uint64_t page_frames_ = kDefaultPageFrames;
    std::uint64_t selection_generation_ = 0;
    std::uint64_t next_source_id_ = 1;
    std::uint64_t next_asset_id_ = 1;
    std::mutex source_load_mutex_;
    std::mutex file_request_mutex_;
    std::condition_variable file_request_changed_;
    std::condition_variable service_wake_;
    std::mutex service_wait_mutex_;
    std::unique_ptr<PreparedStreamedFile> file_request_prepared_;
    bool file_request_pending_ = false;
    bool file_request_complete_ = false;
    PulpSamplerLoadResult file_request_result_{};
    StreamedSourceRecipe retained_stream_recipe_{};

    void service_loop() noexcept {
        while (service_running_.load(std::memory_order_acquire)) {
            service_.update_audio_generations(
                audio_active_generation_.load(std::memory_order_acquire),
                audio_completed_generation_.load(std::memory_order_acquire));
            process_file_request();
#if defined(PULP_SAMPLER_TEST_HOOKS)
            const auto command_drain_paused =
                service_command_drain_paused_for_test_.load(std::memory_order_acquire);
            service_command_drain_paused_ack_for_test_.store(command_drain_paused,
                                                             std::memory_order_release);
            if (!command_drain_paused)
#endif
            {
                service_.drain_commands(commands_);
            }
            service_.drain_completions();
            collect_unpublished_rollbacks();
            if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
                for (std::uint32_t attempt = 0; attempt < kSourceCapacity * 2; ++attempt) {
                    const auto dispatched = service_.dispatch_once();
                    if (!dispatch_made_owner_progress(dispatched)) {
                        break;
                    }
                }
            }
            retire_sources();
            pages_published_.store(service_.telemetry().completions_published,
                                   std::memory_order_relaxed);
            decode_failure_events_.store(
                service_.decode_telemetry().decode_errors,
                std::memory_order_relaxed);
            const auto cache = service_.cache_stats();
            cache_pages_retired_.store(cache.pages_retired,
                                       std::memory_order_relaxed);
            cache_pages_reused_.store(cache.retired_pages_reused,
                                      std::memory_order_relaxed);
            const auto decode = service_.decode_telemetry();
            decode_source_outstanding_high_water_.store(
                decode.source_outstanding_high_water, std::memory_order_relaxed);
            decode_completed_frames_.store(decode.completed_frames,
                                           std::memory_order_relaxed);
            same_source_reader_concurrency_high_water_.store(
                decode.same_source_reader_concurrency_high_water,
                std::memory_order_relaxed);
            cache_async_reservations_high_water_.store(
                cache.async_reservations_high_water, std::memory_order_relaxed);
            active_reservations_high_water_.store(
                service_.telemetry().active_reservations_high_water,
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

    static bool dispatch_made_owner_progress(
        const audio::SampleStreamAsyncDispatchResult& dispatched) noexcept {
        if (dispatched.status == audio::SampleStreamAsyncDispatchStatus::Queued)
            return true;
        if (dispatched.status != audio::SampleStreamAsyncDispatchStatus::Deferred)
            return false;
        // These owner-side outcomes consume a ready/stale request or begin a
        // bounded retirement. Keep draining within the fixed attempt budget;
        // stopping after the first already-ready demand lets harmless refresh
        // traffic head-of-line block pages that actually need decoding.
        return dispatched.reserve_status ==
                   audio::SampleStreamAsyncReserveStatus::AlreadyReady ||
               dispatched.reserve_status ==
                   audio::SampleStreamAsyncReserveStatus::StaleSource ||
               dispatched.reserve_status ==
                   audio::SampleStreamAsyncReserveStatus::PageRetired;
    }

    void process_file_request() noexcept {
        std::unique_ptr<PreparedStreamedFile> prepared;
        {
            std::lock_guard lock(file_request_mutex_);
            if (!file_request_pending_)
                return;
            prepared = std::move(file_request_prepared_);
            file_request_pending_ = false;
        }

        PulpSamplerLoadStatus status = PulpSamplerLoadStatus::InternalFailure;
        try {
            status = prepared
                ? publish_streamed_file(*prepared)
                : PulpSamplerLoadStatus::InternalFailure;
        } catch (const std::bad_alloc&) {
            status = PulpSamplerLoadStatus::AllocationFailure;
        } catch (...) {
            status = PulpSamplerLoadStatus::InternalFailure;
        }
        // A completed result promises that every unpublished staging lease has
        // already rolled back. Destroy the staged bundle before waking the
        // caller; otherwise it can observe transient retained preload bytes.
        prepared.reset();
        {
            std::lock_guard lock(file_request_mutex_);
            file_request_result_.status =
                status == PulpSamplerLoadStatus::Ok &&
                        !service_running_.load(std::memory_order_acquire)
                    ? PulpSamplerLoadStatus::ShuttingDown
                    : status;
            file_request_complete_ = true;
        }
        file_request_changed_.notify_all();
    }

    PulpSamplerLoadResult submit_staged_file(StagedStreamedFile staged) {
        std::unique_lock request_lock(file_request_mutex_);
        if (!service_running_.load(std::memory_order_acquire)) {
            staged.result.status = PulpSamplerLoadStatus::ShuttingDown;
            return staged.result;
        }
        if (file_request_pending_) {
            staged.result.status = PulpSamplerLoadStatus::Busy;
            return staged.result;
        }
        file_request_prepared_ = std::move(staged.prepared);
        file_request_result_ = staged.result;
        file_request_complete_ = false;
        file_request_pending_ = true;
        service_wake_.notify_all();
        file_request_changed_.wait(request_lock, [this] {
            return file_request_complete_ ||
                   !service_running_.load(std::memory_order_acquire);
        });
        if (!file_request_complete_)
            file_request_result_.status = PulpSamplerLoadStatus::ShuttingDown;
        return file_request_result_;
    }

    bool prepare_streamed_preloads(PreparedStreamedFile& prepared,
                                   PulpSamplerLoadResult& result) {
        for (std::uint32_t member = 0; member < prepared.member_count; ++member) {
            const auto& file = prepared.files[member];
            const auto logical_rate = prepared.logical_rates[member];
            if (!file.valid || !file.supports_ranged_read ||
                file.channels != prepared.files[0].channels ||
                !(logical_rate > 0.0) || !std::isfinite(logical_rate) ||
                logical_rate > kMaximumSourceRate) {
                result.status = member == 0
                    ? PulpSamplerLoadStatus::UnsupportedCodec
                    : PulpSamplerLoadStatus::PublishFailed;
                return false;
            }
            auto& contract = prepared.contracts[member];
            contract = {
                .source_sample_rate = logical_rate,
                .host_sample_rate = static_cast<double>(host_sample_rate_),
                .maximum_playback_ratio = kMaximumPitchRatio,
                .certified_io_latency_seconds = kCertifiedIoLatencySeconds,
                .scheduler_margin_seconds = kSchedulerMarginSeconds,
                .decoder_latency_seconds = kDecoderLatencySeconds,
                .maximum_host_block_frames = maximum_host_block_frames_,
                .interpolation_guard_frames =
                    audio::kHighQualitySampleSincHalfWidth,
            };
            const auto block_source_frames = std::ceil(
                static_cast<double>(maximum_host_block_frames_) * logical_rate /
                static_cast<double>(host_sample_rate_) * kMaximumPitchRatio);
            if (!std::isfinite(block_source_frames) ||
                block_source_frames >= static_cast<double>(
                    std::numeric_limits<std::uint64_t>::max())) {
                result.status = PulpSamplerLoadStatus::InvalidPreloadContract;
                return false;
            }
            contract.loop_prefetch_guard_frames =
                static_cast<std::uint64_t>(block_source_frames);
            const auto required = audio::evaluate_sample_preload_contract(contract);
            if (!required.valid() || required.required_preload_frames == 0) {
                result.status = PulpSamplerLoadStatus::InvalidPreloadContract;
                return false;
            }
            const auto preload_frames =
                std::min(file.total_frames, required.required_preload_frames);
            contract.configured_preload_frames = preload_frames;
            const auto preload_bytes = audio::checked_sample_storage_bytes(
                file.channels, preload_frames);
            if (!preload_bytes ||
                *preload_bytes > std::numeric_limits<std::uint64_t>::max() -
                                     result.requested_streaming_memory_bytes) {
                result.status =
                    PulpSamplerLoadStatus::StreamingMemoryBudgetExceeded;
                return false;
            }
            result.requested_streaming_memory_bytes += *preload_bytes;
            auto reservation = memory_governor_.reserve(
                audio::SampleMemoryCategory::Preload, *preload_bytes);
            if (!reservation.acquired()) {
                result.status = reservation.status ==
                        audio::SampleMemoryReserveStatus::BudgetExceeded
                    ? PulpSamplerLoadStatus::StreamingMemoryBudgetExceeded
                    : PulpSamplerLoadStatus::InternalFailure;
                return false;
            }
            prepared.preload_leases[member] = std::move(reservation.lease);
            prepared.preloads[member].resize(
                file.channels, static_cast<std::size_t>(preload_frames));
            if (file.binding.read(0, prepared.preloads[member].view(),
                                  preload_frames, std::stop_token{}) !=
                preload_frames) {
                result.status = PulpSamplerLoadStatus::PreloadReadFailed;
                return false;
            }
            prepared.preload_counts[member] = preload_frames;
            if (member == 0) {
                result.channels = file.channels;
                result.sample_rate = static_cast<std::uint32_t>(logical_rate);
                result.total_frames = file.total_frames;
                result.required_preload_frames = required.required_preload_frames;
                result.configured_preload_frames = preload_frames;
            }
        }
        return true;
    }

    StagedStreamedFile stage_retained_streamed_file() {
        StagedStreamedFile staged;
        if (!retained_stream_recipe_.valid()) {
            staged.result.status = PulpSamplerLoadStatus::NotPrepared;
            return staged;
        }
        auto prepared = std::make_unique<PreparedStreamedFile>();
        prepared->files = retained_stream_recipe_.files;
        prepared->logical_rates = retained_stream_recipe_.logical_rates;
        prepared->octaves = retained_stream_recipe_.octaves;
        prepared->member_count = retained_stream_recipe_.member_count;
        staged.result.codec_capability = PulpSamplerCodecCapability::Ranged;
        staged.result.sidecar_level_count = prepared->member_count - 1;
        staged.result.sidecar_status = prepared->member_count > 1
            ? PulpSamplerSidecarStatus::Loaded
            : PulpSamplerSidecarStatus::NotPresent;
        if (!prepare_streamed_preloads(*prepared, staged.result)) return staged;
        staged.result.status = PulpSamplerLoadStatus::NotAttempted;
        staged.prepared = std::move(prepared);
        return staged;
    }

    StagedStreamedFile stage_streamed_file(std::string_view path) {
        StagedStreamedFile staged;
        auto& result = staged.result;
#if defined(PULP_SAMPLER_TEST_HOOKS)
        file_stage_attempts_for_test_.fetch_add(1, std::memory_order_relaxed);
        if (throw_during_file_stage_for_test_.exchange(false, std::memory_order_acq_rel)) {
            throw std::bad_alloc{};
        }
#endif
        const bool has_sidecar = sampler_stream_mip_sidecar_exists(path);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        file_stage_paused_ack_for_test_.store(
            file_stage_paused_for_test_.load(std::memory_order_acquire), std::memory_order_release);
        while (file_stage_paused_for_test_.load(std::memory_order_acquire) &&
               service_running_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        file_stage_paused_ack_for_test_.store(false, std::memory_order_release);
#endif
        if (!service_running_.load(std::memory_order_acquire)) {
            result.status = PulpSamplerLoadStatus::ShuttingDown;
            return staged;
        }
        if (!has_registered_audio_reader(path)) {
            result.status = PulpSamplerLoadStatus::UnsupportedCodec;
            return staged;
        }

        std::shared_ptr<audio::MemoryMappedAudioReader> retained_base;
        auto base = audio::make_memory_mapped_frame_reader(
            path, false, false, std::numeric_limits<std::uint64_t>::max(), &retained_base);
        if (!base.valid || retained_base == nullptr) {
            result.status = PulpSamplerLoadStatus::OpenFailed;
            return staged;
        }
        result.channels = base.channels;
        result.sample_rate = base.sample_rate;
        result.total_frames = base.total_frames;
        copy_fixed_name(result.codec_name, retained_base->info().format);
        result.codec_capability = base.supports_ranged_read
            ? PulpSamplerCodecCapability::Ranged
            : PulpSamplerCodecCapability::DecodeOnceFallback;
        if (!base.supports_ranged_read) {
            result.status = PulpSamplerLoadStatus::DecodeOnceFallbackRejected;
            return staged;
        }
        if (base.channels == 0 || base.channels > kMaximumChannels) {
            result.status = PulpSamplerLoadStatus::UnsupportedChannelCount;
            return staged;
        }
        if (base.sample_rate == 0 || base.sample_rate > kMaximumSourceRate) {
            result.status = PulpSamplerLoadStatus::UnsupportedSampleRate;
            return staged;
        }
        // Reopen with the strict ranged-only binding after capability
        // inspection. The permissive probe above must never leak its
        // decode-once fallback into an admitted streaming source.
        base = audio::make_memory_mapped_frame_reader(
            path, true, has_sidecar, std::numeric_limits<std::uint64_t>::max(),
            &retained_base);
        if (!base.valid || retained_base == nullptr) {
            result.status = PulpSamplerLoadStatus::OpenFailed;
            return staged;
        }
        // The file may have been replaced between the capability probe and
        // strict reopen. Validate and report the handle that will actually be
        // registered, rather than retaining probe-era metadata.
        result.channels = base.channels;
        result.sample_rate = base.sample_rate;
        result.total_frames = base.total_frames;
        copy_fixed_name(result.codec_name, retained_base->info().format);
        result.codec_capability = PulpSamplerCodecCapability::Ranged;
        if (base.channels == 0 || base.channels > kMaximumChannels) {
            result.status = PulpSamplerLoadStatus::UnsupportedChannelCount;
            return staged;
        }
        if (base.sample_rate == 0 || base.sample_rate > kMaximumSourceRate) {
            result.status = PulpSamplerLoadStatus::UnsupportedSampleRate;
            return staged;
        }

        auto sidecar = has_sidecar ? load_sampler_stream_mip_sidecar(path, base, retained_base)
                                   : SamplerStreamMipSidecar{};
        result.sidecar_status = sidecar.status == SamplerStreamMipSidecarStatus::Valid
            ? PulpSamplerSidecarStatus::Loaded
            : sidecar.status == SamplerStreamMipSidecarStatus::Invalid
                ? PulpSamplerSidecarStatus::IgnoredInvalid
                : PulpSamplerSidecarStatus::NotPresent;
        auto prepared = std::make_unique<PreparedStreamedFile>();
        auto& files = prepared->files;
        auto& logical_rates = prepared->logical_rates;
        auto& octaves = prepared->octaves;
        prepared->member_count = 1;
        logical_rates[0] = static_cast<double>(base.sample_rate);
        files[0] = std::move(base);
        if (sidecar.status == SamplerStreamMipSidecarStatus::Valid) {
            prepared->member_count += sidecar.level_count;
            result.sidecar_level_count = sidecar.level_count;
            for (std::uint32_t level = 0; level < sidecar.level_count; ++level) {
                files[level + 1] = std::move(sidecar.levels[level].reader);
                logical_rates[level + 1] = sidecar.levels[level].sample_rate;
                octaves[level + 1] = sidecar.levels[level].octave;
            }
        }

        if (!prepare_streamed_preloads(*prepared, result)) return staged;
        if (!service_running_.load(std::memory_order_acquire)) {
            result.status = PulpSamplerLoadStatus::ShuttingDown;
            return staged;
        }
        result.status = PulpSamplerLoadStatus::NotAttempted;
        staged.prepared = std::move(prepared);
        return staged;
    }

    PulpSamplerLoadStatus publish_streamed_file(PreparedStreamedFile& prepared) {
        StreamedSlot* slot = nullptr;
        for (auto& candidate : slots_) {
            if (candidate && !candidate->occupied) {
                slot = candidate.get();
                break;
            }
        }
        if (slot == nullptr)
            return PulpSamplerLoadStatus::BundleCapacityExceeded;

        StreamedSourceRecipe candidate_recipe;
        try {
            candidate_recipe.files = prepared.files;
            candidate_recipe.logical_rates = prepared.logical_rates;
            candidate_recipe.octaves = prepared.octaves;
            candidate_recipe.member_count = prepared.member_count;
        } catch (...) {
            return PulpSamplerLoadStatus::AllocationFailure;
        }

        for (auto& member : slot->members) {
            member.asset.release();
            member.source = {};
            member.present = false;
            member.retirement_scheduled = false;
        }
        slot->member_count = 0;
        slot->selection_generation = 0;

        auto& files = prepared.files;
        auto& logical_rates = prepared.logical_rates;
        auto& octaves = prepared.octaves;
        auto& contracts = prepared.contracts;
        auto& preloads = prepared.preloads;
        auto& preload_leases = prepared.preload_leases;
        auto& preload_counts = prepared.preload_counts;
        const auto member_count = prepared.member_count;

#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (block_next_reverse_decode_for_test_.exchange(false, std::memory_order_acq_rel)) {
            auto reader = std::move(files[0].binding.read);
            files[0].binding.read = [this, reader = std::move(reader)](
                                        std::uint64_t start, audio::BufferView<float> destination,
                                        std::uint64_t frames, std::stop_token stop_token) {
                reverse_decode_entered_for_test_.store(true, std::memory_order_release);
                while (!release_reverse_decode_for_test_.load(std::memory_order_acquire) &&
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
            files[0].binding.stop_mode = audio::FrameReaderStopMode::Cooperative;
        }
#endif

        std::array<audio::SampleStreamCacheSourceView, kMaximumBundleMembers> stream_views{};
        try {
            for (std::uint32_t member = 0; member < member_count; ++member) {
#if defined(PULP_SAMPLER_TEST_HOOKS)
                if (fail_after_stream_member_count_for_test_.load(std::memory_order_acquire) ==
                    static_cast<int>(member)) {
                    discard_unpublished_slot(*slot);
                    return PulpSamplerLoadStatus::SourceRegistrationFailed;
                }
                auto reader = std::move(files[member].binding.read);
                files[member].binding.read =
                    [this, reader = std::move(reader)](
                        std::uint64_t start,
                        audio::BufferView<float> destination,
                        std::uint64_t frames,
                        std::stop_token stop_token) {
                        if (fail_next_stream_decode_for_test_.exchange(
                                false, std::memory_order_acq_rel)) {
                            return std::uint64_t{0};
                        }
                        return reader(start, destination, frames, stop_token);
                    };
#endif
                const audio::SampleStreamSourceToken source{next_source_id_++, 1};
                const auto added = service_.add_source(
                    {
                        .token = source,
                        .channels = files[member].channels,
                        .total_frames = files[member].total_frames,
                        .page_frames = page_frames_,
                        .cache_page_count = kCachePagesPerSource,
                    },
                    std::move(files[member].binding));
                if (!added.added()) {
                    discard_unpublished_slot(*slot);
                    return PulpSamplerLoadStatus::SourceRegistrationFailed;
                }
                auto& owned = slot->members[member];
                owned.source = source;
                owned.present = true;
                stream_views[member] = added.view;
            }
#if defined(PULP_SAMPLER_TEST_HOOKS)
            if (fail_after_stream_member_count_for_test_.load(std::memory_order_acquire) ==
                static_cast<int>(member_count)) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::SourceRegistrationFailed;
            }
#endif

            if (preload_counts[0] < files[0].total_frames) {
                const auto prewarm = prewarm_reverse_entry_pages(
                    slot->members[0].source, stream_views[0], files[0].total_frames,
                    preload_counts[0], logical_rates[0]);
                if (prewarm != ReversePrewarmStatus::Ready) {
                    discard_unpublished_slot(*slot);
                    if (prewarm == ReversePrewarmStatus::TimedOut)
                        return PulpSamplerLoadStatus::ReversePrewarmTimedOut;
                    if (prewarm == ReversePrewarmStatus::ShuttingDown)
                        return PulpSamplerLoadStatus::ShuttingDown;
                    return PulpSamplerLoadStatus::PublishFailed;
                }
            }

            for (std::uint32_t member = 0; member < member_count; ++member) {
                const audio::SampleAssetConfig asset_config{
                    .asset = {next_asset_id_++, 1},
                    .source = slot->members[member].source,
                    .channels = files[member].channels,
                    .total_frames = files[member].total_frames,
                    .sample_rate = logical_rates[member],
                    .preload_frames = preload_counts[member],
                    .preload_contract = contracts[member],
                    .stream_source = stream_views[member],
                };
                if (!slot->members[member].asset.prepare_owned(
                        asset_config,
                        std::move(preloads[member]),
                        std::move(preload_leases[member]))) {
                    discard_unpublished_slot(*slot);
                    return PulpSamplerLoadStatus::PublishFailed;
                }
            }

#if defined(PULP_SAMPLER_TEST_HOOKS)
            bundle_publish_paused_ack_for_test_.store(
                pause_before_bundle_publish_for_test_.load(std::memory_order_acquire),
                std::memory_order_release);
            while (pause_before_bundle_publish_for_test_.load(std::memory_order_acquire) &&
                   service_running_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            bundle_publish_paused_ack_for_test_.store(false, std::memory_order_release);
#endif

            std::lock_guard lock(file_request_mutex_);
            if (!service_running_.load(std::memory_order_acquire)) {
                discard_unpublished_slot(*slot);
                return PulpSamplerLoadStatus::ShuttingDown;
            }
            SamplerStreamMipPyramidView streamed_mips;
            streamed_mips.level_count = member_count - 1;
            for (std::uint32_t member = 1; member < member_count; ++member) {
                streamed_mips.levels[member - 1] = {
                    .asset = slot->members[member].asset.view(),
                    .octave = octaves[member],
                };
            }
            slot->member_count = member_count;
            slot->selection_generation = ++selection_generation_;
            slot->occupied = true;
            published_source_.write({
                .kind = SamplerPublishedSourceKind::Streamed,
                .selection_generation = slot->selection_generation,
                .streamed = slot->members[0].asset.view(),
                .streamed_mips = streamed_mips,
            });
            retained_stream_recipe_ = std::move(candidate_recipe);
            active_sources_.fetch_add(1, std::memory_order_relaxed);
            preload_frames_.store(preload_counts[0], std::memory_order_relaxed);
            return PulpSamplerLoadStatus::Ok;
        } catch (const std::bad_alloc&) {
            discard_unpublished_slot(*slot);
            return PulpSamplerLoadStatus::AllocationFailure;
        } catch (...) {
            discard_unpublished_slot(*slot);
            return PulpSamplerLoadStatus::InternalFailure;
        }
    }

    ReversePrewarmStatus prewarm_reverse_entry_pages(
        audio::SampleStreamSourceToken source,
        const audio::SampleStreamCacheSourceView& view,
        std::uint64_t total_frames, std::uint64_t preload_frames,
        double sample_rate) noexcept {
        const auto tail_frame = total_frames - 1;
        const auto first_frame = total_frames > preload_frames ? total_frames - preload_frames : 0;
        const auto first_page = first_frame / page_frames_;
        const auto last_page = tail_frame / page_frames_;
        const auto horizon_page_count = last_page - first_page + 1;
        if (horizon_page_count > kCachePagesPerSource)
            return ReversePrewarmStatus::ScheduleFailed;
        const audio::SampleStreamRequesterToken requester{kAdmissionRequesterId, source.source_id};
        for (auto page = first_page; page <= last_page; ++page) {
            const auto page_end = page == last_page ? total_frames : (page + 1) * page_frames_;
            const auto scheduled = service_.request_page({
                .source = source,
                .requester = requester,
                .page_index = page,
                .resident_source_frames = tail_frame - (page_end - 1),
                .consumption_frames_per_second =
                    static_cast<double>(sample_rate) * kMaximumPitchRatio,
                .demand_class = audio::SampleStreamDemandClass::Attack,
            });
            if (scheduled != audio::SampleStreamScheduleStatus::Inserted &&
                scheduled != audio::SampleStreamScheduleStatus::Refreshed) {
                (void)service_.cancel_requester(requester);
                return ReversePrewarmStatus::ScheduleFailed;
            }
        }
#if defined(PULP_SAMPLER_TEST_HOOKS)
        reverse_prewarm_pending_for_test_.store(true, std::memory_order_release);
#endif

        auto prewarm_timeout =
            reverse_prewarm_timeout_for_pages(static_cast<std::uint32_t>(horizon_page_count));
#if defined(PULP_SAMPLER_TEST_HOOKS)
        if (reverse_prewarm_timeout_override_for_test_)
            prewarm_timeout = reverse_prewarm_timeout_for_test_;
#endif
        const auto deadline = std::chrono::steady_clock::now() + prewarm_timeout;
        const auto reverse_horizon_ready = [&] {
            for (auto page = first_page; page <= last_page; ++page) {
                const auto frame = std::min(tail_frame, page * page_frames_);
                if (!view.window->ready_page_for_frame(source.source_generation, frame).valid) {
                    return false;
                }
            }
            return true;
        };
        while (service_running_.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            service_.drain_commands(commands_);
            service_.drain_completions();
            if (reverse_horizon_ready()) {
                (void)service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
                reverse_prewarm_pending_for_test_.store(false, std::memory_order_release);
#endif
                return service_running_.load(std::memory_order_acquire)
                    ? ReversePrewarmStatus::Ready
                    : ReversePrewarmStatus::ShuttingDown;
            }
            if (!service_dispatch_paused_.load(std::memory_order_acquire)) {
                for (std::uint32_t attempt = 0; attempt < kSourceCapacity * 2; ++attempt) {
                    if (!dispatch_made_owner_progress(service_.dispatch_once())) {
                        break;
                    }
                }
            }
            std::this_thread::yield();
        }
        service_.drain_completions();
        const bool ready = reverse_horizon_ready();
        (void)service_.cancel_requester(requester);
#if defined(PULP_SAMPLER_TEST_HOOKS)
        reverse_prewarm_pending_for_test_.store(false, std::memory_order_release);
#endif
        if (!service_running_.load(std::memory_order_acquire))
            return ReversePrewarmStatus::ShuttingDown;
        return ready ? ReversePrewarmStatus::Ready
                     : ReversePrewarmStatus::TimedOut;
    }

    static std::chrono::milliseconds
    reverse_prewarm_timeout_for_pages(std::uint32_t page_count) noexcept {
        constexpr auto kMinimumTimeout = std::chrono::milliseconds{250};
        const auto per_page_milliseconds =
            std::ceil(1000.0 * (kCertifiedIoLatencySeconds + kSchedulerMarginSeconds +
                                kDecoderLatencySeconds));
        const auto derived = std::chrono::milliseconds{
            static_cast<std::int64_t>(per_page_milliseconds * page_count)};
        return std::max(kMinimumTimeout, derived);
    }

    void discard_unpublished_slot(StreamedSlot& slot) noexcept {
        for (auto& member : slot.members)
            member.asset.release();
        for (auto& member : slot.members) {
            if (member.present)
                rollback_unpublished_source(member.source);
            member.source = {};
            member.present = false;
            member.retirement_scheduled = false;
        }
        slot.member_count = 0;
        slot.selection_generation = 0;
        slot.occupied = false;
    }

    void rollback_unpublished_source(audio::SampleStreamSourceToken source) noexcept {
#if defined(PULP_SAMPLER_TEST_HOOKS)
        unpublished_rollback_attempts_for_test_.fetch_add(1, std::memory_order_relaxed);
#endif
        (void)service_.cancel_pending_source_demands(source);
        service_.drain_completions();
        if (service_.discard_unpublished_source(source))
            return;
        for (auto& pending : unpublished_rollbacks_) {
            if (pending.source_id == 0) {
                pending = source;
#if defined(PULP_SAMPLER_TEST_HOOKS)
                unpublished_rollback_count_for_test_.fetch_add(1, std::memory_order_release);
#endif
                return;
            }
        }
    }

    void collect_unpublished_rollbacks() noexcept {
        for (auto& pending : unpublished_rollbacks_) {
            if (pending.source_id == 0)
                continue;
            (void)service_.cancel_pending_source_demands(pending);
            if (service_.discard_unpublished_source(pending)) {
                pending = {};
#if defined(PULP_SAMPLER_TEST_HOOKS)
                unpublished_rollback_count_for_test_.fetch_sub(1, std::memory_order_release);
#endif
            }
        }
    }

    void retire_sources() noexcept {
        const auto current = published_source_.read();
        const auto acknowledged = stream_audio_ack_selection_.load(std::memory_order_acquire);
        for (auto& owned : slots_) {
            if (!owned || !owned->occupied)
                continue;
            auto& slot = *owned;
            const bool current_slot = current.kind == SamplerPublishedSourceKind::Streamed &&
                                      current.selection_generation == slot.selection_generation;
            if (!current_slot && acknowledged > slot.selection_generation) {
                for (auto& member : slot.members)
                    member.asset.release();
                for (auto& member : slot.members) {
                    if (!member.present || member.retirement_scheduled)
                        continue;
                    const auto status = service_.retire_source_after_asset_unpublish(member.source);
                    if (status == audio::SampleStreamSourceRetireStatus::Scheduled ||
                        status == audio::SampleStreamSourceRetireStatus::AlreadyScheduled ||
                        status == audio::SampleStreamSourceRetireStatus::StaleSource) {
                        member.retirement_scheduled = true;
                    }
                }
            }
        }

        (void)service_.collect_retired_sources();
        for (auto& owned : slots_) {
            if (!owned || !owned->occupied)
                continue;
            bool all_retired = true;
            for (const auto& member : owned->members) {
                if (member.present && service_.cache_service().contains_source(member.source)) {
                    all_retired = false;
                    break;
                }
            }
            if (!all_retired)
                continue;
            for (auto& member : owned->members) {
                member.source = {};
                member.present = false;
                member.retirement_scheduled = false;
            }
            owned->member_count = 0;
            owned->selection_generation = 0;
            owned->occupied = false;
            sources_retired_.fetch_add(1, std::memory_order_relaxed);
            active_sources_.fetch_sub(1, std::memory_order_relaxed);
        }
    }
};

} // namespace pulp::examples
