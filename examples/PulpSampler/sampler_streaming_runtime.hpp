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

#include "sampler_api.hpp"
#include "sampler_mip_pyramid.hpp"
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

    bool valid() const noexcept {
        return callback_generation != 0;
    }
};

class SamplerStreamingRuntime {
  public:
    static constexpr std::size_t kCommandCapacity = 256;
    using CommandInbox = audio::SampleStreamCommandInbox<kCommandCapacity>;

    ~SamplerStreamingRuntime();

    static PulpSamplerPrepareResult
    estimate_prepare(float host_sample_rate, std::uint32_t maximum_host_block_frames,
                     std::uint64_t streaming_memory_budget_bytes = 0) noexcept;

    bool prepare(float host_sample_rate, std::uint32_t maximum_host_block_frames);

    PulpSamplerPrepareResult prepare_checked(float host_sample_rate,
                                             std::uint32_t maximum_host_block_frames,
                                             std::uint64_t streaming_memory_budget_bytes = 0);

    void release() noexcept;

    template <typename Loader, typename ViewReader, typename Publisher>
    bool load_and_publish_resident(Loader&& loader, ViewReader&& read_view,
                                   const SamplerMipPyramidView& resident_mips,
                                   Publisher&& on_published) {
        std::lock_guard lock(source_load_mutex_);
        if (selection_generation_ == std::numeric_limits<std::uint64_t>::max())
            return false;
        if (!loader())
            return false;
        const auto resident = read_view();
        if (!resident.valid)
            return false;
        const auto generation = next_selection_generation();
        if (!generation)
            return false;
        on_published(*generation);
        published_source_.write({
            .kind = SamplerPublishedSourceKind::Resident,
            .selection_generation = *generation,
            .resident = resident,
            .resident_mips = resident_mips,
        });
        retained_stream_recipe_ = {};
        service_wake_.notify_all();
        return true;
    }

    bool republish_resident(const SamplerPublishedSource& retained) noexcept;

    PulpSamplerLoadResult load_sample_file_result(std::string_view path);

    bool load_sample_file(std::string_view path);

    bool restore_retained_streamed_source();

    bool clone_published_source_to(SamplerStreamingRuntime& target);

#if defined(PULP_SAMPLER_TEST_HOOKS)
    void pause_file_stage_for_test(bool paused) noexcept;

    bool file_stage_paused_for_test() const noexcept;

    bool has_retained_streamed_source_for_test() const noexcept;

    void fail_next_retained_source_publish_for_test() noexcept;

    void fail_next_prepare_for_test() noexcept;

    void fail_next_thread_start_for_test() noexcept;

    void fail_next_service_prepare_for_test() noexcept;

    void fail_next_slot_allocation_for_test() noexcept;

    bool fully_released_for_test() const noexcept;
#endif

    SamplerPublishedSource published_source() const noexcept;

    PulpSamplerStreamStats stats() const noexcept;

    PulpSamplerPreloadPolicy preload_policy() const noexcept;

    CommandInbox& command_inbox() noexcept;

    SamplerAudioCallbackToken begin_audio_callback() noexcept;

    void complete_audio_callback(SamplerAudioCallbackToken token) noexcept;

    void acknowledge_selection(std::uint64_t generation) noexcept;

    void record_voice_outcome(audio::SampleStreamVoiceOutcomeClass outcome,
                              std::uint64_t starved_frames = 0) noexcept;

    void record_aggregate_rate_admission_rejection() noexcept;

    void record_aggregate_rate_automation_rejection() noexcept;

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
    static constexpr std::uint32_t kCachePagesPerSource = kMaximumVoices * kPagesPerVoiceWorkingSet;
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

    static DerivedPrepare derive_prepare(float host_sample_rate,
                                         std::uint32_t maximum_host_block_frames,
                                         std::uint64_t configured_budget) noexcept;

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
            return member_count != 0 && member_count <= files.size() && files[0].valid &&
                   files[0].binding.read != nullptr;
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

    static bool has_registered_audio_reader(std::string_view path);

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
    // Slot/member identities are stable and bounded. Reuse advances only the
    // generation so stale tokens can never alias a replacement source.
    std::array<std::uint64_t, kSourceCapacity> next_source_generations_{};
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

    std::optional<std::uint64_t> next_selection_generation() noexcept;

    void service_loop() noexcept;

    bool file_request_pending_snapshot() noexcept;

    static bool
    dispatch_made_owner_progress(const audio::SampleStreamAsyncDispatchResult& dispatched) noexcept;

    void process_file_request() noexcept;

    std::optional<audio::SampleStreamSourceToken>
    take_source_token(std::size_t identity_index) noexcept;

    PulpSamplerLoadResult submit_staged_file(StagedStreamedFile staged);

    bool prepare_streamed_preloads(PreparedStreamedFile& prepared, PulpSamplerLoadResult& result);

    StagedStreamedFile stage_retained_streamed_file();

    StagedStreamedFile stage_streamed_file(std::string_view path);

    PulpSamplerLoadStatus publish_streamed_file(PreparedStreamedFile& prepared);

    ReversePrewarmStatus prewarm_reverse_entry_pages(audio::SampleStreamSourceToken source,
                                                     const audio::SampleStreamCacheSourceView& view,
                                                     std::uint64_t total_frames,
                                                     std::uint64_t preload_frames,
                                                     double sample_rate) noexcept;

    static std::chrono::milliseconds
    reverse_prewarm_timeout_for_pages(std::uint32_t page_count) noexcept;

    void discard_unpublished_slot(StreamedSlot& slot) noexcept;

    void rollback_unpublished_source(audio::SampleStreamSourceToken source) noexcept;

    void collect_unpublished_rollbacks() noexcept;

    void retire_sources() noexcept;
};

} // namespace pulp::examples
