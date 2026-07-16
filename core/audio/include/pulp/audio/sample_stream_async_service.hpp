#pragma once

#include <pulp/audio/sample_stream_decode_pool.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pulp::audio {

struct SampleStreamAsyncServiceConfig {
    SampleStreamCacheServiceConfig cache{};
    SampleStreamDecodePoolConfig decode{};
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
};

/// Owner-thread integration for cache reservations and background decode workers.
/// Destruction joins workers before any cache source or window is destroyed.
template<std::size_t JobMailboxCapacity = 8,
         std::size_t CompletionMailboxCapacity = 8>
class SampleStreamAsyncService {
    struct ReservationRecord;

public:
    SampleStreamAsyncService() = default;
    ~SampleStreamAsyncService() { release(); }

    SampleStreamAsyncService(const SampleStreamAsyncService&) = delete;
    SampleStreamAsyncService& operator=(const SampleStreamAsyncService&) = delete;

    bool prepare(const SampleStreamAsyncServiceConfig& config) {
        release();
        if (config.decode.source_capacity == 0 ||
            !cache_.prepare(config.cache) || !decode_.prepare(config.decode)) {
            release();
            return false;
        }
        try {
            records_.resize(config.decode.source_capacity);
            retirements_.reserve(config.decode.source_capacity);
        } catch (...) {
            release();
            return false;
        }
        prepared_ = true;
        telemetry_ = {};
        return true;
    }

    void release() noexcept {
        decode_.release();
        for (auto& record : records_) {
            if (record.active) {
                (void) cache_.cancel_async_reservation(record.reservation);
                record = {};
            }
        }
        cache_.release();
        records_.clear();
        retirements_.clear();
        telemetry_ = {};
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
                    std::stop_token) {
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
                return read(start, destination, frames, std::stop_token{});
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
                    record = {};
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
            const auto retried = submit_record(record);
            if (retried.status == SampleStreamAsyncDispatchStatus::Queued)
                return retried;
            if (retried.submit_status == SampleStreamDecodeSubmitStatus::QueueFull) {
                queue_full = retried;
                continue;
            }
            return retried;
        }
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
        return submit_record(*record);
    }

private:
    SampleStreamAsyncDispatchResult submit_record(
        ReservationRecord& record) noexcept {
        const auto& reservation = record.reservation;
        if (!cache_.async_reservation_has_interest(reservation)) {
            (void) cache_.cancel_async_reservation(reservation);
            record = {};
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
                record = {};
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
        if (record != nullptr) *record = {};

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
    bool prepared_ = false;
};

}  // namespace pulp::audio
