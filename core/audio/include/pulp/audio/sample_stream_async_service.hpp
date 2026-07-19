#pragma once

#include <pulp/audio/sample_stream_decode_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::audio {

struct SampleStreamAsyncServiceConfig {
    SampleStreamCacheServiceConfig cache{};
    SampleStreamDecodePoolConfig decode{};
};

enum class SampleStreamAsyncPrepareStatus : std::uint8_t {
    Ok,
    InvalidConfiguration,
    CachePrepareFailed,
    DecodePrepareFailed,
    MetadataAllocationFailed,
};

struct SampleStreamAsyncPrepareResult {
    SampleStreamAsyncPrepareStatus status =
        SampleStreamAsyncPrepareStatus::InvalidConfiguration;
    constexpr bool prepared() const noexcept {
        return status == SampleStreamAsyncPrepareStatus::Ok;
    }
};

enum class SampleStreamAsyncDispatchStatus : std::uint8_t {
    Queued,
    Idle,
    Deferred,
    PoolRejected,
    InvalidState,
};

struct SampleStreamAsyncDispatchResult {
    SampleStreamAsyncDispatchStatus status =
        SampleStreamAsyncDispatchStatus::InvalidState;
    SampleStreamAsyncReserveStatus reserve_status =
        SampleStreamAsyncReserveStatus::Idle;
    SampleStreamDecodeSubmitStatus submit_status =
        SampleStreamDecodeSubmitStatus::InvalidJob;
};

enum class SampleStreamAsyncPollStatus : std::uint8_t {
    None,
    Published,
    Discarded,
    CopyFailed,
};

struct SampleStreamAsyncServiceTelemetry {
    std::uint64_t dispatches = 0;
    std::uint64_t completions_published = 0;
    std::uint64_t completions_discarded = 0;
    std::uint64_t completion_copy_failures = 0;
    std::uint64_t active_reservations_high_water = 0;
};

struct SampleStreamAsyncNoopDispatchCutpoint {
    static void after_queue_full_before_reserve() noexcept {}
};

/// Owner-thread integration for cache reservations and background decode workers.
/// Destruction joins workers before any cache source or window is destroyed.
template<std::size_t JobMailboxCapacity = 8,
         std::size_t CompletionMailboxCapacity = 8,
         typename DispatchCutpoint = SampleStreamAsyncNoopDispatchCutpoint>
class SampleStreamAsyncService {
    struct ReservationRecord;

public:
    SampleStreamAsyncService() = default;
    ~SampleStreamAsyncService() { release(); }

    SampleStreamAsyncService(const SampleStreamAsyncService&) = delete;
    SampleStreamAsyncService& operator=(const SampleStreamAsyncService&) = delete;

    bool prepare(const SampleStreamAsyncServiceConfig& config) {
        return prepare_checked(config).prepared();
    }

    SampleStreamAsyncPrepareResult prepare_checked(
        const SampleStreamAsyncServiceConfig& config) {
        release();
        if (config.cache.scheduler_capacity == 0 ||
            config.decode.worker_count == 0 ||
            config.decode.source_capacity == 0 ||
            config.decode.maximum_channels == 0 ||
            config.decode.maximum_frames_per_job == 0 ||
            config.decode.maximum_outstanding_jobs_per_source == 0 ||
            config.decode.source_capacity >
                std::numeric_limits<std::size_t>::max() /
                    config.decode.maximum_outstanding_jobs_per_source) {
            release();
            return {SampleStreamAsyncPrepareStatus::InvalidConfiguration};
        }
        const auto record_capacity =
            static_cast<std::size_t>(config.decode.source_capacity) *
            config.decode.maximum_outstanding_jobs_per_source;
        auto cache_config = config.cache;
        cache_config.maximum_async_reservations_per_source =
            config.decode.maximum_outstanding_jobs_per_source;
        if (!cache_.prepare(cache_config)) {
            release();
            return {SampleStreamAsyncPrepareStatus::CachePrepareFailed};
        }
        if (!decode_.prepare(config.decode)) {
            release();
            return {SampleStreamAsyncPrepareStatus::DecodePrepareFailed};
        }
        try {
            records_.resize(record_capacity);
            retirements_.reserve(config.decode.source_capacity);
        } catch (...) {
            release();
            return {SampleStreamAsyncPrepareStatus::MetadataAllocationFailed};
        }
        prepared_ = true;
        telemetry_ = {};
        active_record_count_ = 0;
        return {SampleStreamAsyncPrepareStatus::Ok};
    }

    void release() noexcept {
        decode_.release();
        for (auto& record : records_) {
            if (record.active) {
                (void) cache_.cancel_async_reservation(record.reservation);
                clear_record(record);
            }
        }
        cache_.release();
        records_.clear();
        retirements_.clear();
        telemetry_ = {};
        active_record_count_ = 0;
        prepared_ = false;
    }

    bool prepared() const noexcept { return prepared_; }

    SampleStreamSourceAddResult add_source(
        const SampleStreamCacheSourceConfig& config,
        FrameReader reader) {
        if (!reader) return {};
        return add_source(
            config,
            FrameReaderBinding{
                .read = [reader = std::move(reader)](
                    std::uint64_t start,
                    BufferView<float> destination,
                    std::uint64_t frames,
                    FrameReaderStopToken) {
                    return reader(start, destination, frames);
                },
                .stop_mode = FrameReaderStopMode::JoinOnly,
            });
    }

    SampleStreamSourceAddResult add_source(
        const SampleStreamCacheSourceConfig& config,
        FrameReaderBinding binding) {
        if (!prepared_ || !binding.read) return {};
        FrameReader synchronous_reader =
            [read = binding.read](std::uint64_t start,
                                  BufferView<float> destination,
                                  std::uint64_t frames) {
                return read(start, destination, frames, FrameReaderStopToken{});
            };
        if (!decode_.add_source(config.token,
                                config.channels,
                                std::move(binding)).added()) {
            return {};
        }
        const auto added = cache_.add_source(config, std::move(synchronous_reader));
        if (!added.added()) {
            (void) decode_.cancel_source(config.token);
            (void) decode_.remove_idle_source(config.token);
            return added;
        }
        return added;
    }

    SampleStreamScheduleStatus request_page(
        const SampleStreamPageDemand& demand) noexcept {
        return cache_.request_page(demand);
    }

    std::size_t cancel_pending_source_demands(
        SampleStreamSourceToken source) noexcept {
        return cache_.cancel_pending_source_demands(source);
    }

    std::size_t cancel_requester(SampleStreamRequesterToken requester) noexcept {
        return cache_.cancel_requester(requester);
    }

    template<std::size_t Capacity>
    SampleStreamCommandDrainResult drain_commands(
        SampleStreamCommandInbox<Capacity>& inbox) noexcept {
        return cache_.drain_commands(inbox);
    }

    bool update_audio_generations(std::uint64_t active,
                                  std::uint64_t completed) noexcept {
        return cache_.update_audio_generations(active, completed);
    }

    std::uint64_t begin_audio_page_read() const noexcept {
        return cache_.begin_audio_page_read();
    }

    void complete_audio_page_read(std::uint64_t epoch) noexcept {
        cache_.complete_audio_page_read(epoch);
    }

    SampleStreamSourceRetireStatus retire_source_after_asset_unpublish(
        SampleStreamSourceToken source) noexcept {
        const auto status = cache_.retire_source_after_asset_unpublish(source);
        if (status == SampleStreamSourceRetireStatus::Scheduled &&
            std::none_of(retirements_.begin(),
                         retirements_.end(),
                         [source](SampleStreamSourceToken candidate) noexcept {
                             return candidate.source_id == source.source_id &&
                                    candidate.source_generation ==
                                        source.source_generation;
                         })) {
            retirements_.push_back(source);
        }
        return status;
    }

    bool discard_unpublished_source(
        SampleStreamSourceToken source) noexcept {
        if (!prepared_ || !cache_.discard_unpublished_source(source))
            return false;
        return decode_.remove_idle_source(source);
    }

    std::size_t collect_retired_sources() noexcept {
        for (const auto source : retirements_) {
            if (!cache_.retirement_watermark_reached(source)) continue;
            (void) decode_.cancel_source(source);
            (void) cache_.cancel_pending_source_demands(source);
            for (auto& record : records_) {
                if (record.active && !record.submitted &&
                    record.reservation.source.source_id == source.source_id &&
                    record.reservation.source.source_generation ==
                        source.source_generation) {
                    (void) cache_.cancel_async_reservation(record.reservation);
                    clear_record(record);
                }
            }
            (void) decode_.remove_idle_source(source);
        }
        drain_completions();
        const auto collected = cache_.collect_retired_sources();
        std::erase_if(retirements_, [this](SampleStreamSourceToken source) noexcept {
            return !cache_.contains_source(source);
        });
        return collected;
    }

    SampleStreamAsyncDispatchResult dispatch_once() noexcept {
        if (!prepared_) return {};
        std::optional<SampleStreamAsyncDispatchResult> queue_full;
        for (auto& record : records_) {
            if (!record.active || record.submitted) continue;
            // A worker mailbox can become writable between retries. Never let
            // a later serial exploit that race and make an earlier retained
            // reservation permanently stale; unrelated sources may still use
            // the newly available worker capacity.
            if (has_earlier_unsubmitted_same_source(record)) continue;
            const auto retried = submit_record(record);
            if (retried.status == SampleStreamAsyncDispatchStatus::Queued)
                return retried;
            if (retried.submit_status == SampleStreamDecodeSubmitStatus::QueueFull) {
                queue_full = retried;
                continue;
            }
            return retried;
        }
        if (queue_full)
            DispatchCutpoint::after_queue_full_before_reserve();
        const auto reserved = cache_.reserve_async_page();
        if (reserved.status == SampleStreamAsyncReserveStatus::Idle) {
            if (queue_full) return *queue_full;
            return {SampleStreamAsyncDispatchStatus::Idle, reserved.status, {}};
        }
        if (reserved.status != SampleStreamAsyncReserveStatus::Reserved) {
            return {SampleStreamAsyncDispatchStatus::Deferred, reserved.status, {}};
        }

        auto* record = free_record();
        if (record == nullptr) {
            (void) cache_.cancel_async_reservation(reserved.reservation);
            return {SampleStreamAsyncDispatchStatus::InvalidState,
                    reserved.status,
                    SampleStreamDecodeSubmitStatus::SourceInFlight};
        }
        record->active = true;
        record->submitted = false;
        record->reservation = reserved.reservation;
        ++active_record_count_;
        telemetry_.active_reservations_high_water = std::max<std::uint64_t>(
            telemetry_.active_reservations_high_water, active_record_count_);
        if (has_earlier_unsubmitted_same_source(*record) && queue_full)
            return *queue_full;
        return submit_record(*record);
    }

private:
    SampleStreamAsyncDispatchResult submit_record(
        ReservationRecord& record) noexcept {
        const auto& reservation = record.reservation;
        if (!cache_.async_reservation_has_interest(reservation)) {
            (void) cache_.cancel_async_reservation(reservation);
            clear_record(record);
            return {SampleStreamAsyncDispatchStatus::Deferred,
                    SampleStreamAsyncReserveStatus::Idle,
                    SampleStreamDecodeSubmitStatus::InvalidJob};
        }
        const auto submitted = decode_.submit({
            .source = reservation.source,
            .reservation_serial = reservation.reservation_serial,
            .start_frame = reservation.start_frame,
            .frame_count = reservation.frame_count,
        });
        if (submitted != SampleStreamDecodeSubmitStatus::Queued) {
            if (submitted != SampleStreamDecodeSubmitStatus::QueueFull) {
                (void) cache_.cancel_async_reservation(reservation);
                clear_record(record);
            }
            return {SampleStreamAsyncDispatchStatus::PoolRejected,
                    SampleStreamAsyncReserveStatus::Reserved,
                    submitted};
        }
        record.submitted = true;
        if (!cache_.commit_async_dispatch(reservation)) {
            // The worker already owns this job. Keep its record and Filling
            // ticket until the canceled completion is drained.
            (void) decode_.cancel_source(reservation.source);
            return {SampleStreamAsyncDispatchStatus::InvalidState,
                    SampleStreamAsyncReserveStatus::Reserved,
                    submitted};
        }
        ++telemetry_.dispatches;
        return {SampleStreamAsyncDispatchStatus::Queued,
                SampleStreamAsyncReserveStatus::Reserved,
                submitted};
    }

public:

    SampleStreamAsyncPollStatus try_process_completion(
        std::uint32_t worker_index) noexcept {
        return process_completion(decode_.try_pop_completion(worker_index));
    }

    SampleStreamAsyncPollStatus wait_process_completion(
        std::uint32_t worker_index) noexcept {
        return process_completion(decode_.wait_pop_completion(worker_index));
    }

    void drain_completions() noexcept {
        if (!prepared_) return;
        bool made_progress = false;
        do {
            made_progress = false;
            for (std::uint32_t worker = 0; worker < decode_.worker_count(); ++worker) {
                if (try_process_completion(worker) !=
                    SampleStreamAsyncPollStatus::None) {
                    made_progress = true;
                }
            }
        } while (made_progress);
    }

    const SampleStreamCacheService& cache_service() const noexcept { return cache_; }
    SampleStreamCacheServiceStats cache_stats() const noexcept { return cache_.stats(); }
    SampleStreamDecodePoolTelemetry decode_telemetry() const noexcept {
        return decode_.telemetry();
    }
    SampleStreamAsyncServiceTelemetry telemetry() const noexcept { return telemetry_; }

private:
    struct ReservationRecord {
        SampleStreamAsyncReservation reservation{};
        bool active = false;
        bool submitted = false;
    };

    ReservationRecord* free_record() noexcept {
        for (auto& record : records_) {
            if (!record.active) return &record;
        }
        return nullptr;
    }

    void clear_record(ReservationRecord& record) noexcept {
        if (record.active && active_record_count_ != 0) --active_record_count_;
        record = {};
    }

    bool has_earlier_unsubmitted_same_source(
        const ReservationRecord& candidate) const noexcept {
        return std::any_of(
            records_.begin(), records_.end(),
            [&candidate](const ReservationRecord& record) noexcept {
                return record.active && !record.submitted &&
                       record.reservation.source.source_id ==
                           candidate.reservation.source.source_id &&
                       record.reservation.source.source_generation ==
                           candidate.reservation.source.source_generation &&
                       record.reservation.reservation_serial <
                           candidate.reservation.reservation_serial;
            });
    }

    ReservationRecord* find_record(
        const SampleStreamDecodeCompletion& completion) noexcept {
        for (auto& record : records_) {
            if (record.active &&
                record.reservation.source.source_id == completion.source.source_id &&
                record.reservation.source.source_generation ==
                    completion.source.source_generation &&
                record.reservation.reservation_serial ==
                    completion.reservation_serial) {
                return &record;
            }
        }
        return nullptr;
    }

    SampleStreamAsyncPollStatus process_completion(
        std::optional<SampleStreamDecodeCompletionView> completion) noexcept {
        if (!completion) return SampleStreamAsyncPollStatus::None;
        auto* record = find_record(completion->completion);
        SampleStreamAsyncCompletionStatus completed =
            SampleStreamAsyncCompletionStatus::StaleReservation;
        if (record != nullptr &&
            match_sample_stream_decode_completion(
                completion->completion,
                record->reservation.source,
                record->reservation.reservation_serial) ==
                SampleStreamDecodeCompletionMatch::Accepted) {
            completed = cache_.publish_async_reservation(record->reservation,
                                                         completion->audio);
        } else if (record != nullptr) {
            completed = cache_.cancel_async_reservation(record->reservation);
        }
        (void) decode_.release_completion(completion->completion);
        if (record != nullptr) clear_record(*record);

        if (completed == SampleStreamAsyncCompletionStatus::Published) {
            ++telemetry_.completions_published;
            return SampleStreamAsyncPollStatus::Published;
        }
        if (completed == SampleStreamAsyncCompletionStatus::CopyFailed ||
            completed == SampleStreamAsyncCompletionStatus::PublishFailed) {
            ++telemetry_.completion_copy_failures;
            return SampleStreamAsyncPollStatus::CopyFailed;
        }
        ++telemetry_.completions_discarded;
        return SampleStreamAsyncPollStatus::Discarded;
    }

    SampleStreamCacheService cache_;
    SampleStreamDecodePool<JobMailboxCapacity,
                           CompletionMailboxCapacity> decode_;
    std::vector<ReservationRecord> records_;
    std::vector<SampleStreamSourceToken> retirements_;
    SampleStreamAsyncServiceTelemetry telemetry_{};
    std::size_t active_record_count_ = 0;
    bool prepared_ = false;
};

}  // namespace pulp::audio
