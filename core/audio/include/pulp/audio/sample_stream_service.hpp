#pragma once

#include <pulp/audio/sample_stream_scheduler.hpp>
#include <pulp/audio/sample_stream_window.hpp>
#include <pulp/audio/sample_memory_governor.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>
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
    bool prepare(const SampleStreamCacheServiceConfig& config) {
        release();
        if (config.scheduler_capacity == 0 ||
            config.maximum_async_reservations_per_source == 0 ||
            (!config.memory_governor && config.page_memory_budget_bytes == 0)) {
            return false;
        }
        if (!scheduler_.prepare(config.scheduler_capacity)) return false;
        if (config.memory_governor) {
            memory_governor_ = config.memory_governor;
        } else {
            if (!internal_memory_governor_.prepare(config.page_memory_budget_bytes)) {
                scheduler_.reset();
                return false;
            }
            memory_governor_ = internal_memory_governor_.handle();
        }
        maximum_async_reservations_per_source_ =
            config.maximum_async_reservations_per_source;
        prepared_ = true;
        return true;
    }

    void release() noexcept {
        sources_.clear();
        source_generations_.clear();
        scheduler_.reset();
        memory_governor_ = {};
        (void) internal_memory_governor_.release();
        active_audio_generation_ = 0;
        completed_audio_generation_ = 0;
        published_page_retirement_epoch_.store(0, std::memory_order_relaxed);
        completed_page_retirement_epoch_.store(0, std::memory_order_relaxed);
        next_page_retirement_epoch_ = 1;
        maximum_async_reservations_per_source_ = 1;
        prepared_ = false;
        stats_ = {};
    }

    bool prepared() const noexcept { return prepared_; }

    /// RT-safe page-lifetime barrier entry. Capture this before the callback
    /// performs any page lookup and return the value at callback completion.
    /// Audio callbacks must be single-threaded, non-overlapping, and complete
    /// in entry order; overlapping render callbacks are unsupported.
    std::uint64_t begin_audio_page_read() const noexcept {
        return published_page_retirement_epoch_.load(std::memory_order_acquire);
    }

    /// RT-safe page-lifetime barrier completion. The callback must publish only
    /// the exact epoch captured by begin_audio_page_read(), after its last page
    /// access. Audio callbacks are single-threaded and complete in entry order.
    void complete_audio_page_read(std::uint64_t epoch) noexcept {
        completed_page_retirement_epoch_.store(epoch, std::memory_order_release);
    }

    /// Updates the generation currently allowed to hold page views and the last
    /// generation whose audio callback has completed. Values are monotonic. A
    /// retired page is not writable again until completed reaches the generation
    /// that was active when the page was retired.
    bool update_audio_generations(std::uint64_t active_audio_generation,
                                  std::uint64_t completed_audio_generation) noexcept {
        if (active_audio_generation == 0 ||
            completed_audio_generation > active_audio_generation ||
            active_audio_generation < active_audio_generation_ ||
            completed_audio_generation < completed_audio_generation_) {
            ++stats_.invalid_audio_generation_updates;
            return false;
        }
        active_audio_generation_ = active_audio_generation;
        completed_audio_generation_ = completed_audio_generation;
        return true;
    }

    SampleStreamSourceAddResult add_source(const SampleStreamCacheSourceConfig& config,
                                           FrameReader reader) {
        if (!prepared_ || !valid_source_config(config) || !reader) {
            return {SampleStreamSourceAddStatus::InvalidConfig, {}};
        }
        if (find_source_id(config.token.source_id) != nullptr) {
            return {SampleStreamSourceAddStatus::DuplicateSource, {}};
        }
        const auto generation_record = std::find_if(
            source_generations_.begin(),
            source_generations_.end(),
            [&config](const SourceGenerationRecord& record) noexcept {
                return record.source_id == config.token.source_id;
            });
        if (generation_record != source_generations_.end() &&
            config.token.source_generation <= generation_record->highest_generation) {
            return {SampleStreamSourceAddStatus::StaleGeneration, {}};
        }

        const auto bytes = page_storage_bytes(config);
        if (!bytes || !memory_governor_) {
            return {SampleStreamSourceAddStatus::BudgetExceeded, {}};
        }
        auto page_reservation = memory_governor_.reserve(
            SampleMemoryCategory::Page, *bytes);
        if (!page_reservation.acquired())
            return {SampleStreamSourceAddStatus::BudgetExceeded, {}};

        try {
            auto source = std::make_unique<Source>();
            source->config = config;
            source->page_lease = std::move(page_reservation.lease);
            if (next_registration_epoch_ == 0) {
                return {SampleStreamSourceAddStatus::AllocationFailed, {}};
            }
            source->registration_epoch = next_registration_epoch_++;
            source->reader = std::move(reader);
            source->channel_ptrs.resize(config.channels);
            source->slots.resize(config.cache_page_count);
            if (!source->window.prepare({
                    .channels = config.channels,
                    .page_count = config.cache_page_count,
                    .page_frames = config.page_frames,
                })) {
                return {SampleStreamSourceAddStatus::AllocationFailed, {}};
            }

            const auto view = make_view(*source);
            sources_.push_back(std::move(source));
            if (generation_record == source_generations_.end()) {
                try {
                    source_generations_.push_back({
                        .source_id = config.token.source_id,
                        .highest_generation = config.token.source_generation,
                    });
                } catch (...) {
                    sources_.pop_back();
                    return {SampleStreamSourceAddStatus::AllocationFailed, {}};
                }
            } else {
                generation_record->highest_generation = config.token.source_generation;
            }
            stats_.reserved_page_bytes += *bytes;
            stats_.source_count = sources_.size();
            return {SampleStreamSourceAddStatus::Added, view};
        } catch (...) {
            return {SampleStreamSourceAddStatus::AllocationFailed, {}};
        }
    }

    /// Call only after the asset publisher has stopped issuing this source view.
    /// The current audio generation becomes the last generation allowed to hold
    /// a borrowed view; collection waits for that generation to complete.
    SampleStreamSourceRetireStatus retire_source_after_asset_unpublish(
        SampleStreamSourceToken token) noexcept {
        if (active_audio_generation_ == 0) {
            return SampleStreamSourceRetireStatus::InvalidGeneration;
        }
        auto* source = find_source(token);
        if (source == nullptr) return SampleStreamSourceRetireStatus::StaleSource;
        if (source->retire_after_audio_generation != 0)
            return SampleStreamSourceRetireStatus::AlreadyScheduled;
        source->retire_after_audio_generation = active_audio_generation_;
        ++stats_.sources_retire_scheduled;
        return SampleStreamSourceRetireStatus::Scheduled;
    }

    /// Remove a source that never became reachable from an audio-thread asset.
    /// The owner may use this only before retirement or async work begins.
    bool discard_unpublished_source(SampleStreamSourceToken token) noexcept {
        const auto found = std::find_if(
            sources_.begin(),
            sources_.end(),
            [token](const auto& source) noexcept {
                return source->config.token.source_id == token.source_id &&
                       source->config.token.source_generation ==
                           token.source_generation;
            });
        if (found == sources_.end() || (*found)->async_reservations != 0 ||
            (*found)->retire_after_audio_generation != 0) {
            return false;
        }
        scheduler_.cancel_source_generation(token.source_id,
                                            token.source_generation);
        const auto bytes = page_storage_bytes((*found)->config).value_or(0);
        stats_.reserved_page_bytes -=
            std::min(stats_.reserved_page_bytes, bytes);
        sources_.erase(found);
        stats_.source_count = sources_.size();
        return true;
    }

    bool retirement_watermark_reached(
        SampleStreamSourceToken token) const noexcept {
        const auto* source = find_source(token);
        return source != nullptr && source->retire_after_audio_generation != 0 &&
               completed_audio_generation_ >=
                   source->retire_after_audio_generation;
    }

    bool contains_source(SampleStreamSourceToken token) const noexcept {
        return find_source(token) != nullptr;
    }

    std::size_t collect_retired_sources() noexcept {
        std::size_t collected = 0;
        for (auto source = sources_.begin(); source != sources_.end();) {
            const auto retire_after = (*source)->retire_after_audio_generation;
            if (retire_after == 0 || completed_audio_generation_ < retire_after) {
                ++source;
                continue;
            }
            if ((*source)->async_reservations != 0) {
                ++source;
                continue;
            }
            scheduler_.cancel_source_generation(
                (*source)->config.token.source_id,
                (*source)->config.token.source_generation);
            const auto bytes = page_storage_bytes((*source)->config).value_or(0);
            stats_.reserved_page_bytes -=
                std::min(stats_.reserved_page_bytes, bytes);
            source = sources_.erase(source);
            ++collected;
        }
        stats_.source_count = sources_.size();
        stats_.sources_collected += collected;
        return collected;
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

    std::size_t cancel_pending_source_demands(SampleStreamSourceToken source) noexcept {
        if (source.source_id == 0 || source.source_generation == 0) return 0;
        return scheduler_.cancel_source_generation(source.source_id,
                                                   source.source_generation);
    }

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

    SampleStreamAsyncReserveResult reserve_async_page() noexcept {
        const auto request = scheduler_.most_urgent_if(
            [this](const SampleStreamPageRequest& candidate) noexcept {
                const auto* source = find_source(
                    {candidate.source_id, candidate.source_generation});
                return source == nullptr ||
                       (source->async_reservations <
                            maximum_async_reservations_per_source_ &&
                        !has_matching_filling_page(*source, candidate));
            });
        if (!request) {
            return {scheduler_.stats().pending == 0
                        ? SampleStreamAsyncReserveStatus::Idle
                        : SampleStreamAsyncReserveStatus::SourceInFlight,
                    {}};
        }

        auto* source = find_source({request->source_id, request->source_generation});
        if (source == nullptr) {
            scheduler_.complete_page(*request);
            ++stats_.stale_requests;
            return {SampleStreamAsyncReserveStatus::StaleSource, {}};
        }
        if (source->async_reservations >= maximum_async_reservations_per_source_)
            return {SampleStreamAsyncReserveStatus::SourceInFlight, {}};
        const auto ready = source->window.ready_page_for_frame(
            request->source_generation, request->start_frame);
        if (ready.valid) {
            source->slots[ready.page_index].publish_sequence =
                source->next_publish_sequence++;
            scheduler_.complete_page(*request);
            ++stats_.already_ready;
            return {SampleStreamAsyncReserveStatus::AlreadyReady, {}};
        }

        const auto serial = take_reservation_serial();
        if (serial == 0)
            return {SampleStreamAsyncReserveStatus::SerialExhausted, {}};
        const auto page = reserve_page(*source, serial);
        if (page.status != PageReservationStatus::Reserved) {
            if (page.status == PageReservationStatus::RetiredVictim) {
                ++stats_.pages_retired;
                return {SampleStreamAsyncReserveStatus::PageRetired, {}};
            }
            if (page.status == PageReservationStatus::WaitingForGeneration) {
                ++stats_.retire_waits;
                return {SampleStreamAsyncReserveStatus::WaitingForAudioGeneration, {}};
            }
            ++stats_.no_page_available;
            return {SampleStreamAsyncReserveStatus::NoPageAvailable, {}};
        }
        if (page.reused_retired) ++stats_.retired_pages_reused;

        ++source->async_reservations;
        stats_.async_reservations_high_water = std::max<std::uint64_t>(
            stats_.async_reservations_high_water, source->async_reservations);
        auto& slot = source->slots[page.page_index];
        slot.fill_registration_epoch = source->registration_epoch;
        slot.fill_start_frame = request->start_frame;
        slot.fill_frame_count = request->frame_count;
        const auto view = make_view(*source);
        return {
            SampleStreamAsyncReserveStatus::Reserved,
            {
                .source = source->config.token,
                .registration = view.registration,
                .registration_epoch = source->registration_epoch,
                .reservation_serial = serial,
                .start_frame = request->start_frame,
                .frame_count = request->frame_count,
                .page_index = page.page_index,
                .final_page = request->frame_count ==
                    source->config.total_frames - request->start_frame,
                .request = *request,
            },
        };
    }

    bool commit_async_dispatch(
        const SampleStreamAsyncReservation& reservation) noexcept {
        auto* source = matching_async_source(reservation);
        if (source == nullptr || !matching_filling_slot(*source, reservation))
            return false;
        scheduler_.complete_page(reservation.request);
        ++stats_.decode_calls;
        return true;
    }

    bool async_reservation_has_interest(
        const SampleStreamAsyncReservation& reservation) const noexcept {
        return scheduler_.has_page_interest(reservation.request);
    }

    SampleStreamAsyncCompletionStatus cancel_async_reservation(
        const SampleStreamAsyncReservation& reservation) noexcept {
        auto* source = find_source(reservation.source);
        if (source == nullptr) return SampleStreamAsyncCompletionStatus::StaleSource;
        if (!registration_matches(*source, reservation))
            return SampleStreamAsyncCompletionStatus::StaleRegistration;
        if (!matching_filling_slot(*source, reservation))
            return SampleStreamAsyncCompletionStatus::StaleReservation;
        if (!cancel_fill(*source,
                         reservation.page_index,
                         reservation.reservation_serial)) {
            return SampleStreamAsyncCompletionStatus::StaleReservation;
        }
        finish_async_reservation(*source);
        return SampleStreamAsyncCompletionStatus::Canceled;
    }

    SampleStreamAsyncCompletionStatus publish_async_reservation(
        const SampleStreamAsyncReservation& reservation,
        BufferView<const float> decoded) noexcept {
        auto* source = find_source(reservation.source);
        if (source == nullptr) return SampleStreamAsyncCompletionStatus::StaleSource;
        if (!registration_matches(*source, reservation))
            return SampleStreamAsyncCompletionStatus::StaleRegistration;
        if (!matching_filling_slot(*source, reservation))
            return SampleStreamAsyncCompletionStatus::StaleReservation;
        if (decoded.num_channels() != source->config.channels ||
            decoded.num_samples() < reservation.frame_count ||
            !source->window.copy_to_filling_page(reservation.page_index,
                                                 decoded,
                                                 reservation.frame_count)) {
            cancel_fill(*source,
                        reservation.page_index,
                        reservation.reservation_serial);
            finish_async_reservation(*source);
            ++stats_.read_failures;
            return SampleStreamAsyncCompletionStatus::CopyFailed;
        }
        if (!source->window.publish_page(
                reservation.page_index,
                {
                    .stream_generation = reservation.source.source_generation,
                    .start_frame = reservation.start_frame,
                    .valid_frames = reservation.frame_count,
                    .final_page = reservation.final_page,
                })) {
            cancel_fill(*source,
                        reservation.page_index,
                        reservation.reservation_serial);
            finish_async_reservation(*source);
            ++stats_.publish_failures;
            return SampleStreamAsyncCompletionStatus::PublishFailed;
        }
        auto& slot = source->slots[reservation.page_index];
        slot.fill_serial = 0;
        slot.publish_sequence = source->next_publish_sequence++;
        finish_async_reservation(*source);
        ++stats_.pages_published;
        return SampleStreamAsyncCompletionStatus::Published;
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
        const auto ready = source->window.ready_page_for_frame(
            request->source_generation, start_frame);
        if (ready.valid) {
            source->slots[ready.page_index].publish_sequence =
                source->next_publish_sequence++;
            scheduler_.complete_page(*request);
            ++stats_.already_ready;
            return SampleStreamServiceStatus::AlreadyReady;
        }

        const auto reservation_serial = take_reservation_serial();
        if (reservation_serial == 0) return SampleStreamServiceStatus::NoPageAvailable;
        const auto reservation = reserve_page(*source, reservation_serial);
        if (reservation.status != PageReservationStatus::Reserved) {
            if (reservation.status == PageReservationStatus::RetiredVictim) {
                ++stats_.pages_retired;
                return SampleStreamServiceStatus::PageRetired;
            }
            if (reservation.status == PageReservationStatus::WaitingForGeneration) {
                ++stats_.retire_waits;
                return SampleStreamServiceStatus::WaitingForAudioGeneration;
            }
            ++stats_.no_page_available;
            return SampleStreamServiceStatus::NoPageAvailable;
        }
        const auto page_slot = reservation.page_index;
        if (reservation.reused_retired) ++stats_.retired_pages_reused;

        scheduler_.complete_page(*request);

        for (std::uint32_t channel = 0; channel < source->config.channels; ++channel) {
            source->channel_ptrs[channel] =
                source->window.writable_channel_data(page_slot, channel);
            if (source->channel_ptrs[channel] == nullptr) {
                cancel_fill(*source, page_slot, reservation_serial);
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
            cancel_fill(*source, page_slot, reservation_serial);
            ++stats_.read_failures;
            return SampleStreamServiceStatus::ReadFailed;
        }

        const bool final_page =
            request->frame_count == source->config.total_frames - start_frame;
        if (!source->window.publish_page(page_slot,
                                        {
                                            .stream_generation = request->source_generation,
                                            .start_frame = start_frame,
                                            .valid_frames = request->frame_count,
                                            .final_page = final_page,
                                        })) {
            cancel_fill(*source, page_slot, reservation_serial);
            ++stats_.publish_failures;
            return SampleStreamServiceStatus::PublishFailed;
        }

        source->slots[page_slot].publish_sequence = source->next_publish_sequence++;
        source->slots[page_slot].fill_serial = 0;
        ++stats_.pages_published;
        return SampleStreamServiceStatus::Published;
    }

    SampleStreamSchedulerStats scheduler_stats() const noexcept {
        return scheduler_.stats();
    }

    SampleStreamCacheServiceStats stats() const noexcept {
        auto snapshot = stats_;
        if (memory_governor_)
            snapshot.memory = memory_governor_.stats();
        return snapshot;
    }

private:
    friend struct SampleStreamCacheServiceTestAccess;

    struct SlotRecord {
        std::uint64_t publish_sequence = 0;
        std::uint64_t fill_serial = 0;
        std::uint64_t fill_registration_epoch = 0;
        std::uint64_t fill_start_frame = 0;
        std::uint64_t fill_frame_count = 0;
    };

    struct SourceGenerationRecord {
        std::uint64_t source_id = 0;
        std::uint64_t highest_generation = 0;
    };

    struct Source {
        SampleStreamCacheSourceConfig config{};
        SampleMemoryLease page_lease;
        SampleStreamWindow window;
        FrameReader reader;
        std::vector<float*> channel_ptrs;
        std::vector<SlotRecord> slots;
        std::uint64_t next_publish_sequence = 1;
        std::uint64_t retire_after_audio_generation = 0;
        std::uint64_t registration_epoch = 0;
        std::uint32_t async_reservations = 0;
    };

    enum class PageReservationStatus : std::uint8_t {
        Reserved,
        RetiredVictim,
        WaitingForGeneration,
        Unavailable,
    };

    struct PageReservation {
        PageReservationStatus status = PageReservationStatus::Unavailable;
        std::uint32_t page_index = 0;
        bool reused_retired = false;
    };

    static bool valid_source_config(const SampleStreamCacheSourceConfig& config) noexcept {
        return config.token.source_id != 0 && config.token.source_generation != 0 &&
               config.channels != 0 && config.total_frames != 0 &&
               config.page_frames != 0 && config.cache_page_count != 0 &&
               config.page_frames <= std::numeric_limits<std::size_t>::max();
    }

    static std::optional<std::uint64_t> page_storage_bytes(
        const SampleStreamCacheSourceConfig& config) noexcept {
        return checked_sample_storage_bytes(config.channels,
                                            config.page_frames,
                                            config.cache_page_count);
    }

    SampleStreamCacheSourceView make_view(Source& source) const noexcept {
        return {
            .token = source.config.token,
            .window = &source.window,
            .total_frames = source.config.total_frames,
            .page_frames = source.config.page_frames,
            .registration_epoch = source.registration_epoch,
            .registration = SampleStreamSourceRegistrationProof(
                source.config.token,
                &source.window,
                source.config.total_frames,
                source.config.page_frames,
                source.registration_epoch),
            .memory_governor_epoch = memory_governor_.epoch(),
        };
    }

    Source* find_source_id(std::uint64_t source_id) noexcept {
        const auto found = std::find_if(sources_.begin(), sources_.end(),
            [source_id](const auto& source) noexcept {
                return source->config.token.source_id == source_id;
            });
        return found == sources_.end() ? nullptr : found->get();
    }

    const Source* find_source_id(std::uint64_t source_id) const noexcept {
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

    const Source* find_source(SampleStreamSourceToken token) const noexcept {
        const auto* source = find_source_id(token.source_id);
        if (source == nullptr ||
            source->config.token.source_generation != token.source_generation) {
            return nullptr;
        }
        return source;
    }

    static bool registration_matches(
        const Source& source,
        const SampleStreamAsyncReservation& reservation) noexcept {
        return reservation.registration_epoch == source.registration_epoch &&
               reservation.registration.matches(
                   source.config.token,
                   &source.window,
                   source.config.total_frames,
                   source.config.page_frames,
                   source.registration_epoch);
    }

    static bool matching_filling_slot(
        const Source& source,
        const SampleStreamAsyncReservation& reservation) noexcept {
        return reservation.page_index < source.slots.size() &&
               source.async_reservations != 0 &&
               source.slots[reservation.page_index].fill_serial ==
                   reservation.reservation_serial &&
               source.slots[reservation.page_index].fill_registration_epoch ==
                   reservation.registration_epoch &&
               source.slots[reservation.page_index].fill_start_frame ==
                   reservation.start_frame &&
               source.slots[reservation.page_index].fill_frame_count ==
                   reservation.frame_count &&
               source.window.page_state(reservation.page_index) ==
                   SampleStreamPageState::Filling;
    }

    static bool has_matching_filling_page(
        const Source& source,
        const SampleStreamPageRequest& request) noexcept {
        for (std::uint32_t page = 0; page < source.slots.size(); ++page) {
            const auto& slot = source.slots[page];
            if (slot.fill_registration_epoch == source.registration_epoch &&
                slot.fill_start_frame == request.start_frame &&
                slot.fill_frame_count == request.frame_count &&
                source.window.page_state(page) == SampleStreamPageState::Filling) {
                return true;
            }
        }
        return false;
    }

    Source* matching_async_source(
        const SampleStreamAsyncReservation& reservation) noexcept {
        auto* source = find_source(reservation.source);
        if (source == nullptr || !registration_matches(*source, reservation))
            return nullptr;
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

    PageReservation reserve_page(Source& source,
                                 std::uint64_t reservation_serial) noexcept {
        const auto completed_page_retirement_epoch =
            completed_page_retirement_epoch_.load(std::memory_order_acquire);
        for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
            if (source.window.page_state(page) == SampleStreamPageState::Empty &&
                source.window.begin_fill_page(page)) {
                source.slots[page] = {};
                source.slots[page].fill_serial = reservation_serial;
                return {PageReservationStatus::Reserved, page, false};
            }
        }

        bool has_retired_page = false;
        for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
            const auto state = source.window.page_state(page);
            if (state != SampleStreamPageState::Retired &&
                state != SampleStreamPageState::Retiring) {
                continue;
            }
            has_retired_page = true;
            if (state == SampleStreamPageState::Retired &&
                source.window.begin_fill_page(
                    page, completed_page_retirement_epoch)) {
                source.slots[page] = {};
                source.slots[page].fill_serial = reservation_serial;
                return {PageReservationStatus::Reserved, page, true};
            }
        }
        if (has_retired_page) {
            return {PageReservationStatus::WaitingForGeneration, 0, false};
        }

        if (active_audio_generation_ == 0 || next_page_retirement_epoch_ == 0)
            return {};

        std::optional<std::uint32_t> victim;
        for (std::uint32_t page = 0; page < source.window.page_count(); ++page) {
            if (source.window.page_state(page) != SampleStreamPageState::Ready) continue;
            if (!victim ||
                source.slots[page].publish_sequence <
                    source.slots[*victim].publish_sequence) {
                victim = page;
            }
        }
        if (!victim) return {};
        const auto retirement_epoch = next_page_retirement_epoch_++;
        if (!source.window.retire_page(*victim, retirement_epoch)) {
            return {};
        }
        // Publish only after Ready -> Retired. A callback that entered before
        // retirement captured the previous epoch and cannot release this page;
        // at least one callback entering after retirement must complete first.
        published_page_retirement_epoch_.store(retirement_epoch,
                                                std::memory_order_release);
        return {PageReservationStatus::RetiredVictim, *victim, false};
    }

    std::uint64_t take_reservation_serial() noexcept {
        if (next_reservation_serial_ == 0) return 0;
        return next_reservation_serial_++;
    }

    static bool cancel_fill(Source& source,
                            std::uint32_t page_index,
                            std::uint64_t reservation_serial) noexcept {
        if (page_index >= source.slots.size() ||
            source.slots[page_index].fill_serial != reservation_serial ||
            source.window.page_state(page_index) != SampleStreamPageState::Filling) {
            return false;
        }
        if (!source.window.cancel_fill_page(page_index)) return false;
        source.slots[page_index].fill_serial = 0;
        return true;
    }

    static bool finish_async_reservation(Source& source) noexcept {
        if (source.async_reservations == 0) return false;
        --source.async_reservations;
        return true;
    }

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
    std::uint32_t maximum_async_reservations_per_source_ = 1;
    bool prepared_ = false;
    SampleStreamCacheServiceStats stats_{};
};

}  // namespace pulp::audio
