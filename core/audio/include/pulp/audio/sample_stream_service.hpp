#pragma once

#include <pulp/audio/sample_stream_scheduler.hpp>
#include <pulp/audio/sample_stream_window.hpp>
#include <pulp/audio/sample_memory_governor.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
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
    /// Maximum number of distinct source IDs whose highest generation is
    /// retained for ABA rejection. Once full, new IDs fail closed; an existing
    /// ID may continue with a strictly newer non-zero generation.
    std::size_t source_identity_capacity = 1024;
    /// Maximum number of independently leased Filling pages for one source.
    std::uint32_t maximum_async_reservations_per_source = 1;
    /// Page-only cap used when memory_governor is empty. Ignored in shared
    /// mode, where the governor's combined preload+page cap is authoritative.
    std::uint64_t page_memory_budget_bytes = 0;
    /// Optional shared preload+page issuer. When omitted, the service owns a
    /// page-only governor capped by page_memory_budget_bytes for compatibility.
    SampleMemoryGovernorHandle memory_governor{};
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

enum class SampleStreamCommandType : std::uint8_t {
    DemandPage,
    CancelRequester,
    CancelPendingSourceDemands,
};

struct SampleStreamCommand {
    SampleStreamCommandType type = SampleStreamCommandType::DemandPage;
    SampleStreamPageDemand demand{};
    SampleStreamRequesterToken requester{};
    SampleStreamSourceToken source{};
};

static_assert(std::is_trivially_copyable_v<SampleStreamCommand>);

enum class SampleStreamCommandPushStatus : std::uint8_t {
    Enqueued,
    Full,
};

struct SampleStreamCommandInboxTelemetry {
    std::size_t pending = 0;
    std::size_t capacity = 0;
    std::uint64_t overflow_count = 0;
};

/// Prepared bounded command port from one RT producer to the service owner.
/// Construct the inbox off the audio thread; subsequent command pushes are
/// fixed-storage SPSC operations with no dynamic payload ownership.
template<std::size_t Capacity>
class SampleStreamCommandInbox {
public:
    SampleStreamCommandPushStatus demand_page(
        const SampleStreamPageDemand& demand) noexcept {
        return push({
            .type = SampleStreamCommandType::DemandPage,
            .demand = demand,
        });
    }

    SampleStreamCommandPushStatus cancel_requester(
        SampleStreamRequesterToken requester) noexcept {
        return push({
            .type = SampleStreamCommandType::CancelRequester,
            .requester = requester,
        });
    }

    SampleStreamCommandPushStatus cancel_pending_source_demands(
        SampleStreamSourceToken source) noexcept {
        return push({
            .type = SampleStreamCommandType::CancelPendingSourceDemands,
            .source = source,
        });
    }

    SampleStreamCommandInboxTelemetry telemetry() const noexcept {
        const auto queue = queue_.telemetry();
        return {
            .pending = queue.size_approx,
            .capacity = queue.capacity,
            .overflow_count = queue.overflow_count,
        };
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    friend class SampleStreamCacheService;

    SampleStreamCommandPushStatus push(const SampleStreamCommand& command) noexcept {
        return queue_.try_push(command) ? SampleStreamCommandPushStatus::Enqueued
                                        : SampleStreamCommandPushStatus::Full;
    }

    std::optional<SampleStreamCommand> try_pop() noexcept {
        return queue_.try_pop();
    }

    runtime::SpscQueue<SampleStreamCommand, Capacity> queue_;
};

struct SampleStreamCommandDrainResult {
    std::size_t commands_drained = 0;
    std::size_t demand_commands = 0;
    std::size_t cancel_requester_commands = 0;
    std::size_t cancel_pending_source_commands = 0;
    std::size_t demands_inserted = 0;
    std::size_t demands_refreshed = 0;
    std::size_t demands_rejected_full = 0;
    std::size_t demands_invalid = 0;
    std::size_t requests_cancelled = 0;
};

class SampleStreamCacheService;
struct SampleStreamCacheServiceTestAccess;

class SampleStreamSourceRegistrationProof {
public:
    SampleStreamSourceRegistrationProof() = default;

    bool matches(SampleStreamSourceToken token,
                 const SampleStreamWindow* window,
                 std::uint64_t total_frames,
                 std::uint64_t page_frames,
                 std::uint64_t registration_epoch) const noexcept {
        return window_ != nullptr && window_ == window &&
               source_id_ == token.source_id &&
               source_generation_ == token.source_generation &&
               total_frames_ == total_frames && page_frames_ == page_frames &&
               registration_epoch_ == registration_epoch;
    }

private:
    friend class SampleStreamCacheService;

    SampleStreamSourceRegistrationProof(SampleStreamSourceToken token,
                                        const SampleStreamWindow* window,
                                        std::uint64_t total_frames,
                                        std::uint64_t page_frames,
                                        std::uint64_t registration_epoch) noexcept
        : window_(window),
          source_id_(token.source_id),
          source_generation_(token.source_generation),
          total_frames_(total_frames),
          page_frames_(page_frames),
          registration_epoch_(registration_epoch) {}

    const SampleStreamWindow* window_ = nullptr;
    std::uint64_t source_id_ = 0;
    std::uint64_t source_generation_ = 0;
    std::uint64_t total_frames_ = 0;
    std::uint64_t page_frames_ = 0;
    std::uint64_t registration_epoch_ = 0;
};

static_assert(std::is_trivially_copyable_v<SampleStreamSourceRegistrationProof>);
static_assert(std::is_standard_layout_v<SampleStreamSourceRegistrationProof>);

struct SampleStreamCacheSourceView {
    SampleStreamSourceToken token{};
    SampleStreamWindow* window = nullptr;
    std::uint64_t total_frames = 0;
    std::uint64_t page_frames = 0;
    std::uint64_t registration_epoch = 0;
    SampleStreamSourceRegistrationProof registration{};
    SampleMemoryGovernorEpoch memory_governor_epoch{};

    bool valid() const noexcept {
        return token.source_id != 0 && token.source_generation != 0 &&
               window != nullptr && total_frames != 0 && page_frames != 0 &&
               memory_governor_epoch.valid() &&
               registration.matches(token,
                                    window,
                                    total_frames,
                                    page_frames,
                                    registration_epoch);
    }
};

static_assert(std::is_trivially_copyable_v<SampleStreamCacheSourceView>);
static_assert(std::is_standard_layout_v<SampleStreamCacheSourceView>);

enum class SampleStreamSourceAddStatus : std::uint8_t {
    Added,
    InvalidConfig,
    DuplicateSource,
    StaleGeneration,
    SourceIdentityCapacityExceeded,
    BudgetExceeded,
    AllocationFailed,
};

enum class SampleStreamSourceRetireStatus : std::uint8_t {
    Scheduled,
    InvalidGeneration,
    StaleSource,
    AlreadyScheduled,
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
    PageRetired,
    WaitingForAudioGeneration,
    StaleSource,
    NoPageAvailable,
    ReadFailed,
    PublishFailed,
};

enum class SampleStreamAsyncReserveStatus : std::uint8_t {
    Reserved,
    Idle,
    AlreadyReady,
    PageRetired,
    WaitingForAudioGeneration,
    StaleSource,
    SourceInFlight,
    NoPageAvailable,
    SerialExhausted,
};

struct SampleStreamAsyncReservation {
    SampleStreamSourceToken source{};
    SampleStreamSourceRegistrationProof registration{};
    std::uint64_t registration_epoch = 0;
    std::uint64_t reservation_serial = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t frame_count = 0;
    std::uint32_t page_index = 0;
    bool final_page = false;
    SampleStreamPageRequest request{};
};

struct SampleStreamAsyncReserveResult {
    SampleStreamAsyncReserveStatus status = SampleStreamAsyncReserveStatus::Idle;
    SampleStreamAsyncReservation reservation{};
};

enum class SampleStreamAsyncCompletionStatus : std::uint8_t {
    Published,
    Canceled,
    StaleSource,
    StaleRegistration,
    StaleReservation,
    CopyFailed,
    PublishFailed,
};

struct SampleStreamCacheServiceStats {
    SampleMemoryGovernorStats memory{};
    std::uint64_t reserved_page_bytes = 0;
    std::uint64_t source_count = 0;
    std::uint64_t source_identity_count = 0;
    std::uint64_t source_identity_capacity = 0;
    std::uint64_t source_identity_capacity_rejections = 0;
    std::uint64_t sources_retire_scheduled = 0;
    std::uint64_t sources_collected = 0;
    std::uint64_t decode_calls = 0;
    std::uint64_t pages_published = 0;
    std::uint64_t already_ready = 0;
    std::uint64_t pages_retired = 0;
    std::uint64_t retired_pages_reused = 0;
    std::uint64_t retire_waits = 0;
    std::uint64_t invalid_audio_generation_updates = 0;
    std::uint64_t stale_requests = 0;
    std::uint64_t no_page_available = 0;
    std::uint64_t read_failures = 0;
    std::uint64_t publish_failures = 0;
    std::uint64_t async_reservations_high_water = 0;
};

/// Bounded shared-page cache with deterministic caller-driven servicing.
/// All direct methods belong to one non-audio owner. Audio code reads returned
/// windows and pushes fixed commands into SampleStreamCommandInbox; the owner
/// applies those commands in FIFO order through drain_commands().
class SampleStreamCacheService {
public:
    SampleStreamCacheService();
    ~SampleStreamCacheService();

    SampleStreamCacheService(const SampleStreamCacheService&) = delete;
    SampleStreamCacheService& operator=(const SampleStreamCacheService&) = delete;

    bool prepare(const SampleStreamCacheServiceConfig& config);
    void release() noexcept;
    bool prepared() const noexcept;

    /// RT-safe page-lifetime barrier entry. Capture this before the callback
    /// performs any page lookup and return the value at callback completion.
    /// Audio callbacks must be single-threaded, non-overlapping, and complete
    /// in entry order; overlapping render callbacks are unsupported.
    std::uint64_t begin_audio_page_read() const noexcept;

    /// RT-safe page-lifetime barrier completion. The callback must publish only
    /// the exact epoch captured by begin_audio_page_read(), after its last page
    /// access. Audio callbacks are single-threaded and complete in entry order.
    void complete_audio_page_read(std::uint64_t epoch) noexcept;

    /// Updates the generation currently allowed to hold page views and the last
    /// generation whose audio callback has completed. Values are monotonic. A
    /// retired page is not writable again until completed reaches the generation
    /// that was active when the page was retired.
    bool update_audio_generations(std::uint64_t active_audio_generation,
                                  std::uint64_t completed_audio_generation) noexcept;

    SampleStreamSourceAddResult add_source(const SampleStreamCacheSourceConfig& config,
                                           FrameReader reader);

    /// Call only after the asset publisher has stopped issuing this source view.
    /// The current audio generation becomes the last generation allowed to hold
    /// a borrowed view; collection waits for that generation to complete.
    SampleStreamSourceRetireStatus retire_source_after_asset_unpublish(
        SampleStreamSourceToken token) noexcept;

    /// Remove a source that never became reachable from an audio-thread asset.
    /// The owner may use this only before retirement or async work begins.
    bool discard_unpublished_source(SampleStreamSourceToken token) noexcept;

    bool retirement_watermark_reached(
        SampleStreamSourceToken token) const noexcept;
    bool contains_source(SampleStreamSourceToken token) const noexcept;
    std::size_t collect_retired_sources() noexcept;
    SampleStreamScheduleStatus request_page(
        const SampleStreamPageDemand& demand) noexcept;
    std::size_t cancel_requester(SampleStreamRequesterToken requester) noexcept;
    std::size_t cancel_pending_source_demands(
        SampleStreamSourceToken source) noexcept;

    /// Applies all currently queued commands in producer order. Non-RT owner
    /// only; demand application may mutate the scheduler's pending vector.
    template<std::size_t Capacity>
    SampleStreamCommandDrainResult drain_commands(
        SampleStreamCommandInbox<Capacity>& inbox) noexcept {
        SampleStreamCommandDrainResult result;
        while (auto command = inbox.try_pop()) {
            ++result.commands_drained;
            switch (command->type) {
                case SampleStreamCommandType::DemandPage: {
                    ++result.demand_commands;
                    const auto status = request_page(command->demand);
                    if (status == SampleStreamScheduleStatus::Inserted) {
                        ++result.demands_inserted;
                    } else if (status == SampleStreamScheduleStatus::Refreshed) {
                        ++result.demands_refreshed;
                    } else if (status == SampleStreamScheduleStatus::Full) {
                        ++result.demands_rejected_full;
                    } else {
                        ++result.demands_invalid;
                    }
                    break;
                }
                case SampleStreamCommandType::CancelRequester:
                    ++result.cancel_requester_commands;
                    result.requests_cancelled +=
                        cancel_requester(command->requester);
                    break;
                case SampleStreamCommandType::CancelPendingSourceDemands:
                    ++result.cancel_pending_source_commands;
                    result.requests_cancelled +=
                        cancel_pending_source_demands(command->source);
                    break;
            }
        }
        return result;
    }

    SampleStreamAsyncReserveResult reserve_async_page() noexcept;

    bool commit_async_dispatch(
        const SampleStreamAsyncReservation& reservation) noexcept;

    bool async_reservation_has_interest(
        const SampleStreamAsyncReservation& reservation) const noexcept;

    SampleStreamAsyncCompletionStatus cancel_async_reservation(
        const SampleStreamAsyncReservation& reservation) noexcept;

    SampleStreamAsyncCompletionStatus publish_async_reservation(
        const SampleStreamAsyncReservation& reservation,
        BufferView<const float> decoded) noexcept;

    SampleStreamServiceStatus service_once() noexcept;
    SampleStreamSchedulerStats scheduler_stats() const noexcept;
    SampleStreamCacheServiceStats stats() const noexcept;

private:
    friend struct SampleStreamCacheServiceTestAccess;

    struct SlotRecord;
    struct SourceGenerationRecord;
    struct Source;
    enum class PageReservationStatus : std::uint8_t;
    struct PageReservation;

    static bool valid_source_config(
        const SampleStreamCacheSourceConfig& config) noexcept;

    static std::optional<std::uint64_t> page_storage_bytes(
        const SampleStreamCacheSourceConfig& config) noexcept;

    SampleStreamCacheSourceView make_view(Source& source) const noexcept;
    Source* find_source_id(std::uint64_t source_id) noexcept;
    const Source* find_source_id(std::uint64_t source_id) const noexcept;
    Source* find_source(SampleStreamSourceToken token) noexcept;
    const Source* find_source(SampleStreamSourceToken token) const noexcept;

    static bool registration_matches(
        const Source& source,
        const SampleStreamAsyncReservation& reservation) noexcept;

    static bool matching_filling_slot(
        const Source& source,
        const SampleStreamAsyncReservation& reservation) noexcept;

    static bool has_matching_filling_page(
        const Source& source,
        const SampleStreamPageRequest& request) noexcept;

    Source* matching_async_source(
        const SampleStreamAsyncReservation& reservation) noexcept;

    static std::optional<SampleStreamPageRequest> canonical_request(
        const Source& source,
        const SampleStreamPageDemand& demand) noexcept;

    PageReservation reserve_page(Source& source,
                                 std::uint64_t reservation_serial) noexcept;

    std::uint64_t take_reservation_serial() noexcept;

    static bool cancel_fill(Source& source,
                            std::uint32_t page_index,
                            std::uint64_t reservation_serial) noexcept;

    static bool finish_async_reservation(Source& source) noexcept;

    std::vector<std::unique_ptr<Source>> sources_;
    std::vector<SourceGenerationRecord> source_generations_;
    SampleStreamScheduler scheduler_;
    SampleMemoryGovernor internal_memory_governor_;
    SampleMemoryGovernorHandle memory_governor_;
    std::uint64_t active_audio_generation_ = 0;
    std::uint64_t completed_audio_generation_ = 0;
    // Written in opposite directions by the service and audio threads. Keep
    // each endpoint on its own cache line so the per-block load does not share
    // ownership with the infrequent retirement/completion store.
    alignas(64) std::atomic<std::uint64_t> published_page_retirement_epoch_{0};
    alignas(64) std::atomic<std::uint64_t> completed_page_retirement_epoch_{0};
    std::uint64_t next_page_retirement_epoch_ = 1;
    std::uint64_t next_registration_epoch_ = 1;
    std::uint64_t next_reservation_serial_ = 1;
    std::size_t source_identity_capacity_ = 0;
    std::uint32_t maximum_async_reservations_per_source_ = 1;
    bool prepared_ = false;
    SampleStreamCacheServiceStats stats_{};
};

}  // namespace pulp::audio
