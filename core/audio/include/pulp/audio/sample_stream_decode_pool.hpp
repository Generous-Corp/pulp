#pragma once

#include <pulp/audio/sample_stream_service.hpp>
#include <pulp/runtime/spsc_queue.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::audio {

struct SampleStreamDecodePoolConfig {
    std::uint32_t worker_count = 0;
    std::uint32_t source_capacity = 0;
    std::uint32_t maximum_channels = 0;
    std::uint64_t maximum_frames_per_job = 0;
    /// Bounded serial pipeline depth for each source. Jobs for one source stay
    /// pinned to one worker and are decoded FIFO, so non-thread-safe readers
    /// are never entered concurrently.
    std::uint32_t maximum_outstanding_jobs_per_source = 1;
};

struct SampleStreamDecodeJob {
    SampleStreamSourceToken source{};
    std::uint64_t reservation_serial = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t frame_count = 0;
};

enum class SampleStreamDecodeCompletionStatus : std::uint8_t {
    Decoded,
    ShortRead,
    ReaderError,
    Canceled,
    Stopped,
};

struct SampleStreamDecodeCompletion {
    SampleStreamSourceToken source{};
    std::uint64_t reservation_serial = 0;
    std::uint64_t requested_frames = 0;
    std::uint64_t decoded_frames = 0;
    std::uint32_t channels = 0;
    SampleStreamDecodeCompletionStatus status =
        SampleStreamDecodeCompletionStatus::ReaderError;
    std::uint32_t worker_index = 0;
    std::uint32_t scratch_slot = 0;
    /// Qualifies reuse of one scratch slot within a prepare epoch.
    std::uint64_t scratch_lease_generation = 0;
    /// Qualifies scratch leases across release()/prepare() cycles.
    std::uint64_t pool_prepare_epoch = 0;
};

enum class SampleStreamDecodeCompletionMatch : std::uint8_t {
    Accepted,
    StaleSource,
    StaleGeneration,
    StaleReservation,
    DecodeError,
    Canceled,
    Stopped,
};

inline SampleStreamDecodeCompletionMatch match_sample_stream_decode_completion(
    const SampleStreamDecodeCompletion& completion,
    SampleStreamSourceToken expected_source,
    std::uint64_t expected_reservation_serial) noexcept {
    if (completion.source.source_id != expected_source.source_id)
        return SampleStreamDecodeCompletionMatch::StaleSource;
    if (completion.source.source_generation != expected_source.source_generation)
        return SampleStreamDecodeCompletionMatch::StaleGeneration;
    if (completion.reservation_serial != expected_reservation_serial)
        return SampleStreamDecodeCompletionMatch::StaleReservation;
    if (completion.status == SampleStreamDecodeCompletionStatus::Canceled)
        return SampleStreamDecodeCompletionMatch::Canceled;
    if (completion.status == SampleStreamDecodeCompletionStatus::Stopped)
        return SampleStreamDecodeCompletionMatch::Stopped;
    if (completion.status != SampleStreamDecodeCompletionStatus::Decoded)
        return SampleStreamDecodeCompletionMatch::DecodeError;
    return SampleStreamDecodeCompletionMatch::Accepted;
}

enum class SampleStreamDecodeSourceAddStatus : std::uint8_t {
    Added,
    InvalidSource,
    DuplicateSource,
    SourceCapacityFull,
    Stopped,
};

struct SampleStreamDecodeSourceAddResult {
    SampleStreamDecodeSourceAddStatus status =
        SampleStreamDecodeSourceAddStatus::InvalidSource;
    std::uint32_t worker_index = 0;

    bool added() const noexcept {
        return status == SampleStreamDecodeSourceAddStatus::Added;
    }
};

enum class SampleStreamDecodeSubmitStatus : std::uint8_t {
    Queued,
    InvalidJob,
    StaleSource,
    StaleGeneration,
    StaleReservation,
    SourceInFlight,
    QueueFull,
    Stopped,
};

enum class SampleStreamDecodeCancelStatus : std::uint8_t {
    Canceled,
    AlreadyCanceled,
    StaleSource,
    StaleGeneration,
    Stopped,
};

struct SampleStreamDecodePoolTelemetry {
    std::uint64_t jobs_queued = 0;
    std::uint64_t completions_published = 0;
    std::uint64_t job_queue_full = 0;
    std::uint64_t completion_queue_full = 0;
    std::uint64_t stale_generation = 0;
    std::uint64_t stale_reservation = 0;
    std::uint64_t decode_errors = 0;
    std::uint64_t canceled_completions = 0;
    std::uint64_t stopped_jobs = 0;
    std::uint64_t source_outstanding_high_water = 0;
    std::uint64_t completed_frames = 0;
    std::uint64_t same_source_reader_concurrency_high_water = 0;
};

struct SampleStreamDecodeCompletionView {
    SampleStreamDecodeCompletion completion{};
    /// Valid until release_completion(); non-decoded outcomes expose no samples.
    BufferView<const float> audio{};
};

static_assert(std::is_trivially_copyable_v<SampleStreamDecodeJob>);
static_assert(std::is_trivially_copyable_v<SampleStreamDecodeCompletion>);

/// Fixed-storage owner/worker decode pool. Its public methods are control- or
/// service-thread operations; none are audio-callback APIs.
template<std::size_t JobMailboxCapacity = 8,
         std::size_t CompletionMailboxCapacity = 8>
class SampleStreamDecodePool {
public:
    static_assert(JobMailboxCapacity > 0);
    static_assert(CompletionMailboxCapacity > 0);
    static_assert(CompletionMailboxCapacity <=
                  std::numeric_limits<std::uint32_t>::max());

    SampleStreamDecodePool() = default;
    ~SampleStreamDecodePool() { release(); }

    SampleStreamDecodePool(const SampleStreamDecodePool&) = delete;
    SampleStreamDecodePool& operator=(const SampleStreamDecodePool&) = delete;

    bool prepare(const SampleStreamDecodePoolConfig& config) {
        release();
        if (config.worker_count == 0 || config.source_capacity == 0 ||
            config.maximum_channels == 0 || config.maximum_frames_per_job == 0 ||
            config.maximum_outstanding_jobs_per_source == 0 ||
            config.maximum_outstanding_jobs_per_source > JobMailboxCapacity ||
            config.maximum_outstanding_jobs_per_source > CompletionMailboxCapacity ||
            config.maximum_frames_per_job >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
            return false;
        }

        const auto frames_per_slot = checked_multiply(
            static_cast<std::uint64_t>(config.maximum_channels),
            config.maximum_frames_per_job);
        const auto slots = checked_multiply(
            static_cast<std::uint64_t>(config.worker_count),
            config.maximum_outstanding_jobs_per_source);
        const auto reserved_samples = frames_per_slot && slots
            ? checked_multiply(*frames_per_slot, *slots)
            : std::nullopt;
        if (!frames_per_slot ||
            *frames_per_slot > std::numeric_limits<std::size_t>::max() ||
            !reserved_samples) {
            return false;
        }

        if (next_prepare_epoch_ == 0) return false;
        const auto prepare_epoch = next_prepare_epoch_++;
        try {
            workers_ = std::make_unique<Worker[]>(config.worker_count);
            sources_ = std::make_unique<Source[]>(config.source_capacity);
            config_ = config;
            for (std::uint32_t index = 0; index < config.worker_count; ++index) {
                auto& worker = workers_[index];
                worker.scratch_slot_count =
                    config.maximum_outstanding_jobs_per_source;
                worker.scratch_slots = std::make_unique<ScratchSlot[]>(
                    worker.scratch_slot_count);
                for (std::uint32_t scratch_index = 0;
                     scratch_index < worker.scratch_slot_count;
                     ++scratch_index) {
                    auto& scratch = worker.scratch_slots[scratch_index];
                    scratch.audio.resize(config.maximum_channels,
                                         static_cast<std::size_t>(
                                             config.maximum_frames_per_job));
                    scratch.channel_ptrs.resize(config.maximum_channels);
                    scratch.const_channel_ptrs.resize(config.maximum_channels);
                }
            }
            reset_telemetry();
            reserved_scratch_samples_ = *reserved_samples;
            active_prepare_epoch_ = prepare_epoch;
            prepared_ = true;
            for (std::uint32_t index = 0; index < config.worker_count; ++index) {
                workers_[index].thread =
                    std::thread([this, index] { worker_loop(index); });
            }
            return true;
        } catch (...) {
            release();
            return false;
        }
    }

    void release() noexcept {
        if (workers_) {
            for (std::uint32_t index = 0; index < config_.worker_count; ++index) {
                workers_[index].stopping.store(true, std::memory_order_release);
            }
            if (sources_) {
                for (std::uint32_t index = 0; index < config_.source_capacity; ++index) {
                    sources_[index].active_stop_source.request_stop();
                }
            }
            for (std::uint32_t index = 0; index < config_.worker_count; ++index) {
                workers_[index].wake.notify_all();
            }
            for (std::uint32_t index = 0; index < config_.worker_count; ++index) {
                if (workers_[index].thread.joinable()) workers_[index].thread.join();
            }
        }
        prepared_ = false;
        sources_.reset();
        workers_.reset();
        config_ = {};
        next_worker_ = 0;
        reserved_scratch_samples_ = 0;
        active_prepare_epoch_ = 0;
    }

    bool prepared() const noexcept { return prepared_; }
    std::uint32_t worker_count() const noexcept { return config_.worker_count; }
    std::uint32_t source_capacity() const noexcept { return config_.source_capacity; }

    std::uint64_t reserved_scratch_samples() const noexcept {
        return reserved_scratch_samples_;
    }

    SampleStreamDecodeSourceAddResult add_source(SampleStreamSourceToken token,
                                                  std::uint32_t channels,
                                                  FrameReader reader) {
        if (!reader)
            return {SampleStreamDecodeSourceAddStatus::InvalidSource, 0};
        return add_source(token,
                          channels,
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

    SampleStreamDecodeSourceAddResult add_source(
        SampleStreamSourceToken token,
        std::uint32_t channels,
        FrameReaderBinding binding) {
        if (!prepared_) return {SampleStreamDecodeSourceAddStatus::Stopped, 0};
        if (token.source_id == 0 || token.source_generation == 0 || channels == 0 ||
            channels > config_.maximum_channels || !binding.read)
            return {SampleStreamDecodeSourceAddStatus::InvalidSource, 0};
        for (std::uint32_t index = 0; index < config_.source_capacity; ++index) {
            const auto& source = sources_[index];
            if (source.registered && source.token.source_id == token.source_id)
                return {SampleStreamDecodeSourceAddStatus::DuplicateSource,
                        source.worker_index};
        }
        for (std::uint32_t index = 0; index < config_.source_capacity; ++index) {
            auto& source = sources_[index];
            if (source.registered) continue;
            source.token = token;
            source.channels = channels;
            source.binding = std::move(binding);
            source.worker_index = next_worker_++ % config_.worker_count;
            source.last_reservation_serial = 0;
            source.outstanding_jobs = 0;
            source.canceled.store(false, std::memory_order_relaxed);
            source.registered = true;
            return {SampleStreamDecodeSourceAddStatus::Added, source.worker_index};
        }
        return {SampleStreamDecodeSourceAddStatus::SourceCapacityFull, 0};
    }

    SampleStreamDecodeSubmitStatus submit(const SampleStreamDecodeJob& job) noexcept {
        if (!prepared_) return SampleStreamDecodeSubmitStatus::Stopped;
        if (job.source.source_id == 0 || job.source.source_generation == 0 ||
            job.reservation_serial == 0 || job.frame_count == 0 ||
            job.frame_count > config_.maximum_frames_per_job ||
            job.start_frame >
                std::numeric_limits<std::uint64_t>::max() - job.frame_count) {
            return SampleStreamDecodeSubmitStatus::InvalidJob;
        }
        auto* source = find_source_id(job.source.source_id);
        if (source == nullptr) return SampleStreamDecodeSubmitStatus::StaleSource;
        if (source->token.source_generation != job.source.source_generation) {
            telemetry_.stale_generation.fetch_add(1, std::memory_order_relaxed);
            return SampleStreamDecodeSubmitStatus::StaleGeneration;
        }
        if (job.reservation_serial <= source->last_reservation_serial) {
            telemetry_.stale_reservation.fetch_add(1, std::memory_order_relaxed);
            return SampleStreamDecodeSubmitStatus::StaleReservation;
        }
        if (source->outstanding_jobs >=
            config_.maximum_outstanding_jobs_per_source) {
            return SampleStreamDecodeSubmitStatus::SourceInFlight;
        }
        if (source->canceled.load(std::memory_order_acquire))
            return SampleStreamDecodeSubmitStatus::StaleSource;

        auto& worker = workers_[source->worker_index];
        if (worker.scratch_lease_exhausted.load(std::memory_order_acquire) ||
            worker.stopping.load(std::memory_order_acquire)) {
            return SampleStreamDecodeSubmitStatus::Stopped;
        }
        InternalJob internal{job, static_cast<std::uint32_t>(source - sources_.get())};
        ++source->outstanding_jobs;
        if (!worker.jobs.try_push(internal)) {
            --source->outstanding_jobs;
            telemetry_.job_queue_full.fetch_add(1, std::memory_order_relaxed);
            return SampleStreamDecodeSubmitStatus::QueueFull;
        }
        source->last_reservation_serial = job.reservation_serial;
        update_high_water(telemetry_.source_outstanding_high_water,
                          source->outstanding_jobs);
        telemetry_.jobs_queued.fetch_add(1, std::memory_order_relaxed);
        worker.wake.notify_one();
        return SampleStreamDecodeSubmitStatus::Queued;
    }

    SampleStreamDecodeCancelStatus cancel_source(
        SampleStreamSourceToken token) noexcept {
        if (!prepared_) return SampleStreamDecodeCancelStatus::Stopped;
        auto* source = find_source_id(token.source_id);
        if (source == nullptr) return SampleStreamDecodeCancelStatus::StaleSource;
        if (source->token.source_generation != token.source_generation) {
            telemetry_.stale_generation.fetch_add(1, std::memory_order_relaxed);
            return SampleStreamDecodeCancelStatus::StaleGeneration;
        }
        if (source->canceled.exchange(true, std::memory_order_acq_rel))
            return SampleStreamDecodeCancelStatus::AlreadyCanceled;
        source->active_stop_source.request_stop();
        if (source->outstanding_jobs == 0) clear_source(*source);
        return SampleStreamDecodeCancelStatus::Canceled;
    }

    bool remove_idle_source(SampleStreamSourceToken token) noexcept {
        if (!prepared_) return false;
        auto* source = find_source(token);
        if (source == nullptr || source->outstanding_jobs != 0) return false;
        clear_source(*source);
        return true;
    }

    std::optional<SampleStreamDecodeCompletionView> try_pop_completion(
        std::uint32_t worker_index) noexcept {
        if (!prepared_ || worker_index >= config_.worker_count) return std::nullopt;
        auto& worker = workers_[worker_index];
        if (worker.scratch_lease_exhausted.load(std::memory_order_acquire))
            return std::nullopt;
        if (worker.completion_leased) return std::nullopt;
        auto completion = worker.completions.try_pop();
        if (!completion) return std::nullopt;
        worker.wake.notify_one();
        worker.completion_leased = true;
        worker.leased_completion = *completion;
        const auto publishable_frames =
            completion->status == SampleStreamDecodeCompletionStatus::Decoded
                ? completion->decoded_frames
                : std::uint64_t{0};
        if (completion->scratch_slot >= worker.scratch_slot_count) {
            worker.completion_leased = false;
            return std::nullopt;
        }
        auto& scratch = worker.scratch_slots[completion->scratch_slot];
        return SampleStreamDecodeCompletionView{
            .completion = *completion,
            .audio = BufferView<const float>(
                scratch.const_channel_ptrs.data(),
                completion->channels,
                static_cast<std::size_t>(publishable_frames)),
        };
    }

    std::optional<SampleStreamDecodeCompletionView> wait_pop_completion(
        std::uint32_t worker_index) noexcept {
        if (!prepared_ || worker_index >= config_.worker_count) return std::nullopt;
        auto& worker = workers_[worker_index];
        {
            std::unique_lock lock(worker.mutex);
            worker.wake.wait(lock, [&] {
                return worker.stopping.load(std::memory_order_acquire) ||
                       worker.scratch_lease_exhausted.load(
                           std::memory_order_acquire) ||
                       !worker.completions.empty();
            });
        }
        if (worker.stopping.load(std::memory_order_acquire) ||
            worker.scratch_lease_exhausted.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        return try_pop_completion(worker_index);
    }

    bool release_completion(const SampleStreamDecodeCompletion& completion) noexcept {
        if (!prepared_ || completion.worker_index >= config_.worker_count) return false;
        auto& worker = workers_[completion.worker_index];
        if (!worker.completion_leased ||
            worker.leased_completion.source.source_id != completion.source.source_id ||
            worker.leased_completion.source.source_generation !=
                completion.source.source_generation ||
            worker.leased_completion.reservation_serial !=
                completion.reservation_serial ||
            worker.leased_completion.scratch_slot != completion.scratch_slot ||
            worker.leased_completion.scratch_lease_generation !=
                completion.scratch_lease_generation) {
            return false;
        }
        if (worker.leased_completion.pool_prepare_epoch !=
                completion.pool_prepare_epoch ||
            completion.pool_prepare_epoch != active_prepare_epoch_) {
            return false;
        }

        auto* source = find_source(completion.source);
        if (source != nullptr && source->outstanding_jobs != 0) {
            --source->outstanding_jobs;
            if (source->outstanding_jobs == 0 &&
                source->canceled.load(std::memory_order_acquire)) {
                clear_source(*source);
            }
        }
        worker.completion_leased = false;
        worker.scratch_slots[completion.scratch_slot].in_use.store(
            false, std::memory_order_release);
        worker.wake.notify_one();
        return true;
    }

    SampleStreamDecodePoolTelemetry telemetry() const noexcept {
        return {
            .jobs_queued = telemetry_.jobs_queued.load(std::memory_order_relaxed),
            .completions_published =
                telemetry_.completions_published.load(std::memory_order_relaxed),
            .job_queue_full = telemetry_.job_queue_full.load(std::memory_order_relaxed),
            .completion_queue_full =
                telemetry_.completion_queue_full.load(std::memory_order_relaxed),
            .stale_generation =
                telemetry_.stale_generation.load(std::memory_order_relaxed),
            .stale_reservation =
                telemetry_.stale_reservation.load(std::memory_order_relaxed),
            .decode_errors = telemetry_.decode_errors.load(std::memory_order_relaxed),
            .canceled_completions =
                telemetry_.canceled_completions.load(std::memory_order_relaxed),
            .stopped_jobs = telemetry_.stopped_jobs.load(std::memory_order_relaxed),
            .source_outstanding_high_water =
                telemetry_.source_outstanding_high_water.load(std::memory_order_relaxed),
            .completed_frames =
                telemetry_.completed_frames.load(std::memory_order_relaxed),
            .same_source_reader_concurrency_high_water =
                telemetry_.same_source_reader_concurrency_high_water.load(
                    std::memory_order_relaxed),
        };
    }

private:
    struct InternalJob {
        SampleStreamDecodeJob job{};
        std::uint32_t source_index = 0;
    };

    struct Source {
        SampleStreamSourceToken token{};
        FrameReaderBinding binding;
        std::uint64_t last_reservation_serial = 0;
        std::uint32_t worker_index = 0;
        std::uint32_t channels = 0;
        std::atomic<bool> canceled{false};
        std::atomic<std::uint32_t> active_reader_calls{0};
        std::stop_source active_stop_source;
        bool registered = false;
        std::uint32_t outstanding_jobs = 0;
    };

    struct ScratchSlot {
        Buffer<float> audio;
        std::vector<float*> channel_ptrs;
        std::vector<const float*> const_channel_ptrs;
        std::atomic<bool> in_use{false};
        std::uint64_t next_lease_generation = 1;
    };

    struct Worker {
        runtime::SpscQueue<InternalJob, JobMailboxCapacity> jobs;
        runtime::SpscQueue<SampleStreamDecodeCompletion,
                           CompletionMailboxCapacity> completions;
        std::unique_ptr<ScratchSlot[]> scratch_slots;
        std::uint32_t scratch_slot_count = 0;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        std::atomic<bool> stopping{false};
        std::atomic<bool> scratch_lease_exhausted{false};
        SampleStreamDecodeCompletion leased_completion{};
        bool completion_leased = false;
    };

    struct AtomicTelemetry {
        std::atomic<std::uint64_t> jobs_queued{0};
        std::atomic<std::uint64_t> completions_published{0};
        std::atomic<std::uint64_t> job_queue_full{0};
        std::atomic<std::uint64_t> completion_queue_full{0};
        std::atomic<std::uint64_t> stale_generation{0};
        std::atomic<std::uint64_t> stale_reservation{0};
        std::atomic<std::uint64_t> decode_errors{0};
        std::atomic<std::uint64_t> canceled_completions{0};
        std::atomic<std::uint64_t> stopped_jobs{0};
        std::atomic<std::uint64_t> source_outstanding_high_water{0};
        std::atomic<std::uint64_t> completed_frames{0};
        std::atomic<std::uint64_t> same_source_reader_concurrency_high_water{0};
    };

    Source* find_source_id(std::uint64_t source_id) noexcept {
        if (!sources_) return nullptr;
        for (std::uint32_t index = 0; index < config_.source_capacity; ++index) {
            auto& source = sources_[index];
            if (source.registered && source.token.source_id == source_id) return &source;
        }
        return nullptr;
    }

    Source* find_source(SampleStreamSourceToken token) noexcept {
        auto* source = find_source_id(token.source_id);
        if (source == nullptr ||
            source->token.source_generation != token.source_generation) {
            return nullptr;
        }
        return source;
    }

    static void clear_source(Source& source) noexcept {
        source.binding = {};
        source.token = {};
        source.last_reservation_serial = 0;
        source.worker_index = 0;
        source.channels = 0;
        source.canceled.store(false, std::memory_order_relaxed);
        source.active_reader_calls.store(0, std::memory_order_relaxed);
        source.active_stop_source = std::stop_source{};
        source.outstanding_jobs = 0;
        source.registered = false;
    }

    void worker_loop(std::uint32_t worker_index) noexcept {
        auto& worker = workers_[worker_index];
        for (;;) {
            auto internal = worker.jobs.try_pop();
            if (!internal) {
                std::unique_lock lock(worker.mutex);
                worker.wake.wait(lock, [&] {
                    return worker.stopping.load(std::memory_order_acquire) ||
                           !worker.jobs.empty();
                });
                if (worker.stopping.load(std::memory_order_acquire)) break;
                continue;
            }

            auto scratch_lease = acquire_scratch_slot(worker);
            if (!scratch_lease) break;
            auto& scratch = worker.scratch_slots[scratch_lease->slot];
            auto& source = sources_[internal->source_index];
            for (std::uint32_t channel = 0; channel < source.channels; ++channel) {
                scratch.channel_ptrs[channel] = scratch.audio.channel(channel).data();
                scratch.const_channel_ptrs[channel] = scratch.channel_ptrs[channel];
            }
            BufferView<float> destination(
                scratch.channel_ptrs.data(),
                source.channels,
                static_cast<std::size_t>(internal->job.frame_count));
            std::uint64_t decoded = 0;
            bool reader_error = false;
            const auto canceled_before_read =
                source.canceled.load(std::memory_order_acquire);
            if (!canceled_before_read) {
                const auto active_reader_calls =
                    source.active_reader_calls.fetch_add(1,
                                                         std::memory_order_acq_rel) + 1;
                update_high_water(telemetry_.same_source_reader_concurrency_high_water,
                                  active_reader_calls);
                try {
                    decoded = source.binding.read(
                        internal->job.start_frame,
                        destination,
                        internal->job.frame_count,
                        source.active_stop_source.get_token());
                    if (decoded > internal->job.frame_count) {
                        decoded = 0;
                        reader_error = true;
                    }
                } catch (...) {
                    reader_error = true;
                    decoded = 0;
                }
                source.active_reader_calls.fetch_sub(1, std::memory_order_acq_rel);
            }

            if (worker.stopping.load(std::memory_order_acquire)) {
                scratch.in_use.store(false, std::memory_order_release);
                telemetry_.stopped_jobs.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            auto status = SampleStreamDecodeCompletionStatus::Decoded;
            if (reader_error) {
                status = SampleStreamDecodeCompletionStatus::ReaderError;
                telemetry_.decode_errors.fetch_add(1, std::memory_order_relaxed);
            } else if (source.canceled.load(std::memory_order_acquire)) {
                status = SampleStreamDecodeCompletionStatus::Canceled;
                telemetry_.canceled_completions.fetch_add(1,
                                                          std::memory_order_relaxed);
            } else if (decoded != internal->job.frame_count) {
                status = SampleStreamDecodeCompletionStatus::ShortRead;
                telemetry_.decode_errors.fetch_add(1, std::memory_order_relaxed);
            }

            const SampleStreamDecodeCompletion completion{
                .source = internal->job.source,
                .reservation_serial = internal->job.reservation_serial,
                .requested_frames = internal->job.frame_count,
                .decoded_frames = decoded,
                .channels = source.channels,
                .status = status,
                .worker_index = worker_index,
                .scratch_slot = scratch_lease->slot,
                .scratch_lease_generation = scratch_lease->generation,
                .pool_prepare_epoch = active_prepare_epoch_,
            };
            while (!worker.completions.try_push(completion)) {
                telemetry_.completion_queue_full.fetch_add(1,
                                                           std::memory_order_relaxed);
                std::unique_lock lock(worker.mutex);
                worker.wake.wait(lock, [&] {
                    return worker.stopping.load(std::memory_order_acquire) ||
                           worker.completions.size_approx() <
                               CompletionMailboxCapacity;
                });
                if (worker.stopping.load(std::memory_order_acquire)) break;
            }
            if (worker.stopping.load(std::memory_order_acquire)) {
                scratch.in_use.store(false, std::memory_order_release);
                telemetry_.stopped_jobs.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            telemetry_.completions_published.fetch_add(1,
                                                       std::memory_order_relaxed);
            if (status == SampleStreamDecodeCompletionStatus::Decoded) {
                telemetry_.completed_frames.fetch_add(decoded,
                                                      std::memory_order_relaxed);
            }
            worker.wake.notify_all();
        }

        while (worker.jobs.try_pop())
            telemetry_.stopped_jobs.fetch_add(1, std::memory_order_relaxed);
    }

    SampleStreamDecodePoolConfig config_{};
    std::unique_ptr<Worker[]> workers_;
    std::unique_ptr<Source[]> sources_;
    AtomicTelemetry telemetry_{};
    std::uint32_t next_worker_ = 0;
    bool prepared_ = false;
    std::uint64_t reserved_scratch_samples_ = 0;
    std::uint64_t active_prepare_epoch_ = 0;
    std::uint64_t next_prepare_epoch_ = 1;

    void reset_telemetry() noexcept {
        telemetry_.jobs_queued.store(0, std::memory_order_relaxed);
        telemetry_.completions_published.store(0, std::memory_order_relaxed);
        telemetry_.job_queue_full.store(0, std::memory_order_relaxed);
        telemetry_.completion_queue_full.store(0, std::memory_order_relaxed);
        telemetry_.stale_generation.store(0, std::memory_order_relaxed);
        telemetry_.stale_reservation.store(0, std::memory_order_relaxed);
        telemetry_.decode_errors.store(0, std::memory_order_relaxed);
        telemetry_.canceled_completions.store(0, std::memory_order_relaxed);
        telemetry_.stopped_jobs.store(0, std::memory_order_relaxed);
        telemetry_.source_outstanding_high_water.store(0, std::memory_order_relaxed);
        telemetry_.completed_frames.store(0, std::memory_order_relaxed);
        telemetry_.same_source_reader_concurrency_high_water.store(
            0, std::memory_order_relaxed);
    }

    static void update_high_water(std::atomic<std::uint64_t>& high_water,
                                  std::uint64_t candidate) noexcept {
        auto observed = high_water.load(std::memory_order_relaxed);
        while (observed < candidate &&
               !high_water.compare_exchange_weak(observed,
                                                 candidate,
                                                 std::memory_order_relaxed)) {}
    }

    static std::optional<std::uint64_t> checked_multiply(
        std::uint64_t lhs, std::uint64_t rhs) noexcept {
        if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
            return std::nullopt;
        return lhs * rhs;
    }

    struct ScratchLease {
        std::uint32_t slot = 0;
        std::uint64_t generation = 0;
    };

    static std::optional<ScratchLease> acquire_scratch_slot(Worker& worker) noexcept {
        for (;;) {
            for (std::uint32_t index = 0; index < worker.scratch_slot_count; ++index) {
                bool available = false;
                if (worker.scratch_slots[index].in_use.compare_exchange_strong(
                        available, true, std::memory_order_acq_rel)) {
                    auto& scratch = worker.scratch_slots[index];
                    const auto generation = scratch.next_lease_generation;
                    if (generation == 0) {
                        scratch.in_use.store(false, std::memory_order_release);
                        worker.scratch_lease_exhausted.store(
                            true, std::memory_order_release);
                        worker.wake.notify_all();
                        return std::nullopt;
                    }
                    ++scratch.next_lease_generation;
                    return ScratchLease{index, generation};
                }
            }
            std::unique_lock lock(worker.mutex);
            worker.wake.wait(lock, [&] {
                if (worker.stopping.load(std::memory_order_acquire)) return true;
                for (std::uint32_t index = 0;
                     index < worker.scratch_slot_count;
                     ++index) {
                    if (!worker.scratch_slots[index].in_use.load(
                            std::memory_order_acquire)) return true;
                }
                return false;
            });
            if (worker.stopping.load(std::memory_order_acquire)) return std::nullopt;
        }
    }
};

}  // namespace pulp::audio
