#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_stream_async_service.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

using pulp::audio::Buffer;
using pulp::audio::SampleStreamAsyncCompletionStatus;
using pulp::audio::SampleStreamAsyncDispatchStatus;
using pulp::audio::SampleStreamAsyncPollStatus;
using pulp::audio::SampleStreamAsyncReserveStatus;
using pulp::audio::SampleStreamAsyncService;
using pulp::audio::SampleStreamCacheService;
using pulp::audio::SampleStreamPageDemand;
using pulp::audio::SampleStreamPageState;
using pulp::audio::SampleStreamScheduleStatus;
using pulp::audio::SampleStreamSourceRetireStatus;

namespace {

class DecodeGate {
public:
    ~DecodeGate() { allow(); }

    std::uint64_t read(std::uint64_t start,
                       pulp::audio::BufferView<float> destination,
                       std::uint64_t frames) {
        {
            std::unique_lock lock(mutex_);
            entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [&] { return allowed_; });
        }
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            destination.channel_ptr(0)[frame] = static_cast<float>(start + frame);
        return frames;
    }

    bool wait_until_entered(
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return entered_; });
    }

    void allow() {
        {
            std::lock_guard lock(mutex_);
            allowed_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool allowed_ = false;
};

struct QueueFullDispatchCutpoint {
    static inline DecodeGate* release_gate = nullptr;
    static inline DecodeGate* entered_gate = nullptr;
    static inline std::atomic<bool> timed_out = false;

    static void after_queue_full_before_reserve() noexcept {
        if (release_gate != nullptr) release_gate->allow();
        if (entered_gate != nullptr && !entered_gate->wait_until_entered())
            timed_out.store(true, std::memory_order_release);
    }

    class Scope {
    public:
        Scope(DecodeGate& release, DecodeGate& entered) noexcept {
            timed_out.store(false, std::memory_order_relaxed);
            release_gate = &release;
            entered_gate = &entered;
        }
        ~Scope() {
            release_gate = nullptr;
            entered_gate = nullptr;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
};

SampleStreamPageDemand demand(std::uint64_t generation,
                              std::uint64_t page_index,
                              std::uint64_t source_id = 1) {
    return {
        .source = {source_id, generation},
        .requester = {10, 1},
        .page_index = page_index,
        .resident_source_frames = page_index * 4,
        .consumption_frames_per_second = 48000.0,
    };
}

auto source_config(std::uint64_t generation) {
    return pulp::audio::SampleStreamCacheSourceConfig{
        .token = {1, generation},
        .channels = 1,
        .total_frames = 8,
        .page_frames = 4,
        .cache_page_count = 1,
    };
}

auto one_page_source(std::uint64_t source_id) {
    return pulp::audio::SampleStreamCacheSourceConfig{
        .token = {source_id, 1},
        .channels = 1,
        .total_frames = 4,
        .page_frames = 4,
        .cache_page_count = 1,
    };
}

auto constant_reader(float value) {
    return [value](std::uint64_t,
                   pulp::audio::BufferView<float> destination,
                   std::uint64_t frames) {
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            destination.channel_ptr(0)[frame] = value;
        return frames;
    };
}

}  // namespace

TEST_CASE("Async stream service drains canceled work before generation replacement",
          "[audio][sampler][async-stream][retirement]") {
    SampleStreamAsyncService<1, 1> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 8,
            .page_memory_budget_bytes = 16,
        },
        .decode = {
            .worker_count = 1,
            .source_capacity = 2,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
        },
    }));
    REQUIRE(service.update_audio_generations(1, 0));

    DecodeGate gate;
    const auto added_a = service.add_source(
        source_config(1),
        [&](std::uint64_t start,
            pulp::audio::BufferView<float> destination,
            std::uint64_t frames) {
            return gate.read(start, destination, frames);
        });
    REQUIRE(added_a.added());
    REQUIRE(service.request_page(demand(1, 0)) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(gate.wait_until_entered());

    REQUIRE(service.request_page(demand(1, 1)) == SampleStreamScheduleStatus::Inserted);
    const auto duplicate_dispatch = service.dispatch_once();
    REQUIRE(duplicate_dispatch.status == SampleStreamAsyncDispatchStatus::Deferred);
    REQUIRE(duplicate_dispatch.reserve_status ==
            SampleStreamAsyncReserveStatus::SourceInFlight);

    REQUIRE(service.retire_source_after_asset_unpublish({1, 1}) ==
            SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(service.update_audio_generations(2, 1));
    REQUIRE(service.collect_retired_sources() == 0);
    REQUIRE_FALSE(service.add_source(source_config(2), constant_reader(100.0f)).added());

    gate.allow();
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Discarded);
    REQUIRE(added_a.view.window->page_state(0) == SampleStreamPageState::Empty);
    REQUIRE(service.collect_retired_sources() == 1);

    const auto added_b =
        service.add_source(source_config(2), constant_reader(100.0f));
    REQUIRE(added_b.added());
    REQUIRE(service.request_page(demand(2, 0)) == SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Published);

    const auto page = added_b.view.window->ready_page_for_frame(2, 0);
    REQUIRE(page.valid);
    REQUIRE_FALSE(added_b.view.window->ready_page_for_frame(1, 0).valid);
    REQUIRE(added_b.view.window->ready_channel_data(page, 0)[0] == 100.0f);
    for (std::uint32_t slot = 0; slot < added_b.view.window->page_count(); ++slot)
        REQUIRE(added_b.view.window->page_state(slot) !=
                SampleStreamPageState::Filling);
    REQUIRE(service.retire_source_after_asset_unpublish({1, 2}) ==
            SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(service.update_audio_generations(3, 2));
    REQUIRE(service.collect_retired_sources() == 1);
    REQUIRE(service.add_source(source_config(3), constant_reader(300.0f)).added());
}

TEST_CASE("Async reservation identity cannot cancel a recycled generation slot",
          "[audio][sampler][async-stream][reservation]") {
    SampleStreamCacheService cache;
    REQUIRE(cache.prepare({.scheduler_capacity = 4,
                           .page_memory_budget_bytes = 16}));
    REQUIRE(cache.update_audio_generations(1, 0));
    REQUIRE(cache.add_source(source_config(1), constant_reader(1.0f)).added());
    REQUIRE(cache.request_page(demand(1, 0)) == SampleStreamScheduleStatus::Inserted);
    const auto old = cache.reserve_async_page();
    REQUIRE(old.status == SampleStreamAsyncReserveStatus::Reserved);
    REQUIRE(cache.cancel_async_reservation(old.reservation) ==
            SampleStreamAsyncCompletionStatus::Canceled);
    REQUIRE(cache.retire_source_after_asset_unpublish({1, 1}) ==
            SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(cache.update_audio_generations(2, 1));
    REQUIRE(cache.collect_retired_sources() == 1);

    const auto added_b = cache.add_source(source_config(2), constant_reader(2.0f));
    REQUIRE(added_b.added());
    REQUIRE(cache.request_page(demand(2, 0)) == SampleStreamScheduleStatus::Inserted);
    const auto current = cache.reserve_async_page();
    REQUIRE(current.status == SampleStreamAsyncReserveStatus::Reserved);
    auto wrong_range = current.reservation;
    ++wrong_range.start_frame;
    REQUIRE(cache.cancel_async_reservation(wrong_range) ==
            SampleStreamAsyncCompletionStatus::StaleReservation);
    REQUIRE(cache.cancel_async_reservation(old.reservation) ==
            SampleStreamAsyncCompletionStatus::StaleSource);
    REQUIRE(added_b.view.window->page_state(current.reservation.page_index) ==
            SampleStreamPageState::Filling);

    Buffer<float> decoded(1, 4);
    for (auto& sample : decoded.channel(0)) sample = 2.0f;
    const auto& decoded_const = decoded;
    REQUIRE(cache.commit_async_dispatch(current.reservation));
    REQUIRE(cache.publish_async_reservation(current.reservation,
                                            decoded_const.view()) ==
            SampleStreamAsyncCompletionStatus::Published);
    const auto page = added_b.view.window->ready_page_for_frame(2, 0);
    REQUIRE(page.valid);
    REQUIRE(added_b.view.window->ready_channel_data(page, 0)[0] == 2.0f);
}

TEST_CASE("Async reservation identity survives service reprepare",
          "[audio][sampler][async-stream][reservation]") {
    SampleStreamCacheService cache;
    REQUIRE(cache.prepare({.scheduler_capacity = 4,
                           .page_memory_budget_bytes = 16}));
    REQUIRE(cache.add_source(source_config(1), constant_reader(1.0f)).added());
    REQUIRE(cache.request_page(demand(1, 0)) == SampleStreamScheduleStatus::Inserted);
    const auto old = cache.reserve_async_page();
    REQUIRE(old.status == SampleStreamAsyncReserveStatus::Reserved);

    cache.release();
    REQUIRE(cache.prepare({.scheduler_capacity = 4,
                           .page_memory_budget_bytes = 16}));
    const auto current_source =
        cache.add_source(source_config(1), constant_reader(2.0f));
    REQUIRE(current_source.added());
    REQUIRE(cache.request_page(demand(1, 0)) == SampleStreamScheduleStatus::Inserted);
    const auto current = cache.reserve_async_page();
    REQUIRE(current.status == SampleStreamAsyncReserveStatus::Reserved);
    REQUIRE(current.reservation.registration_epoch !=
            old.reservation.registration_epoch);
    REQUIRE(current.reservation.reservation_serial !=
            old.reservation.reservation_serial);

    Buffer<float> decoded(1, 4);
    for (auto& sample : decoded.channel(0)) sample = 1.0f;
    const auto& decoded_const = decoded;
    REQUIRE(cache.publish_async_reservation(old.reservation,
                                            decoded_const.view()) ==
            SampleStreamAsyncCompletionStatus::StaleRegistration);
    REQUIRE(current_source.view.window->page_state(current.reservation.page_index) ==
            SampleStreamPageState::Filling);
    REQUIRE(cache.cancel_async_reservation(current.reservation) ==
            SampleStreamAsyncCompletionStatus::Canceled);
}

TEST_CASE("Async dispatch retains queue-full tickets without blocking free workers",
          "[audio][sampler][async-stream][queue-pressure]") {
    SampleStreamAsyncService<1, 1> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 8,
            .page_memory_budget_bytes = 5 * 16,
        },
        .decode = {
            .worker_count = 2,
            .source_capacity = 5,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
        },
    }));

    DecodeGate gate;
    const auto active = service.add_source(
        one_page_source(10),
        [&](std::uint64_t start,
            pulp::audio::BufferView<float> destination,
            std::uint64_t frames) {
            return gate.read(start, destination, frames);
        });
    const auto free_worker =
        service.add_source(one_page_source(11), constant_reader(11.0f));
    const auto queued =
        service.add_source(one_page_source(12), constant_reader(12.0f));
    REQUIRE(service.add_source(one_page_source(13), constant_reader(13.0f)).added());
    const auto retained =
        service.add_source(one_page_source(14), constant_reader(14.0f));
    REQUIRE(active.added());
    REQUIRE(free_worker.added());
    REQUIRE(queued.added());
    REQUIRE(retained.added());

    REQUIRE(service.request_page(demand(1, 0, 10)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(gate.wait_until_entered());
    REQUIRE(service.request_page(demand(1, 0, 12)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.request_page(demand(1, 0, 14)) ==
            SampleStreamScheduleStatus::Inserted);
    const auto full = service.dispatch_once();
    REQUIRE(full.status == SampleStreamAsyncDispatchStatus::PoolRejected);
    REQUIRE(full.submit_status ==
            pulp::audio::SampleStreamDecodeSubmitStatus::QueueFull);
    REQUIRE(retained.view.window->page_state(0) == SampleStreamPageState::Filling);

    REQUIRE(service.request_page(demand(1, 0, 11)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.wait_process_completion(1) ==
            SampleStreamAsyncPollStatus::Published);
    REQUIRE(free_worker.view.window->ready_page_for_frame(1, 0).valid);
    REQUIRE(retained.view.window->page_state(0) == SampleStreamPageState::Filling);

    gate.allow();
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Published);
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Published);
    REQUIRE(queued.view.window->ready_page_for_frame(1, 0).valid);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Published);
    REQUIRE(retained.view.window->ready_page_for_frame(1, 0).valid);
    REQUIRE(retained.view.window->page_state(0) != SampleStreamPageState::Filling);
}

TEST_CASE("Async source registration rolls back the pool when cache admission fails",
          "[audio][sampler][async-stream][registration]") {
    SampleStreamAsyncService<1, 1> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 4,
            .page_memory_budget_bytes = 16,
        },
        .decode = {
            .worker_count = 1,
            .source_capacity = 2,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
        },
    }));
    REQUIRE(service.update_audio_generations(1, 0));
    REQUIRE(service.add_source(one_page_source(60), constant_reader(60.0f)).added());
    REQUIRE_FALSE(
        service.add_source(one_page_source(61), constant_reader(61.0f)).added());

    REQUIRE(service.retire_source_after_asset_unpublish({60, 1}) ==
            SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(service.update_audio_generations(2, 1));
    REQUIRE(service.collect_retired_sources() == 1);
    REQUIRE(service.add_source(one_page_source(61), constant_reader(61.0f)).added());
}

TEST_CASE("Async service discards a source that was never published",
          "[audio][sampler][async-stream][registration]") {
    SampleStreamAsyncService<1, 1> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 4,
            .page_memory_budget_bytes = 16,
        },
        .decode = {
            .worker_count = 1,
            .source_capacity = 1,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
        },
    }));

    REQUIRE(service.add_source(one_page_source(70), constant_reader(70.0f)).added());
    REQUIRE(service.discard_unpublished_source({70, 1}));
    REQUIRE_FALSE(service.cache_service().contains_source({70, 1}));
    REQUIRE(service.add_source(one_page_source(71), constant_reader(71.0f)).added());
}

TEST_CASE("Cache bounds serial reservations and coalesces duplicate filling pages",
          "[audio][sampler][async-stream][pipeline]") {
    SampleStreamCacheService cache;
    REQUIRE(cache.prepare({
        .scheduler_capacity = 8,
        .maximum_async_reservations_per_source = 2,
        .page_memory_budget_bytes = 3 * 4 * sizeof(float),
    }));
    REQUIRE(cache.update_audio_generations(1, 0));
    REQUIRE(cache.add_source({
        .token = {90, 1},
        .channels = 1,
        .total_frames = 12,
        .page_frames = 4,
        .cache_page_count = 3,
    }, constant_reader(90.0f)).added());

    REQUIRE(cache.request_page(demand(1, 0, 90)) ==
            SampleStreamScheduleStatus::Inserted);
    const auto first = cache.reserve_async_page();
    REQUIRE(first.status == SampleStreamAsyncReserveStatus::Reserved);
    REQUIRE(cache.commit_async_dispatch(first.reservation));
    REQUIRE(cache.request_page(demand(1, 1, 90)) ==
            SampleStreamScheduleStatus::Inserted);
    const auto second = cache.reserve_async_page();
    REQUIRE(second.status == SampleStreamAsyncReserveStatus::Reserved);
    REQUIRE(cache.commit_async_dispatch(second.reservation));
    REQUIRE(cache.stats().async_reservations_high_water == 2);

    REQUIRE(cache.request_page(demand(1, 0, 90)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(cache.reserve_async_page().status ==
            SampleStreamAsyncReserveStatus::SourceInFlight);

    Buffer<float> decoded(1, 4);
    for (auto& sample : decoded.channel(0)) sample = 90.0f;
    const auto& decoded_const = decoded;
    REQUIRE(cache.publish_async_reservation(first.reservation,
                                            decoded_const.view()) ==
            SampleStreamAsyncCompletionStatus::Published);
    REQUIRE(cache.reserve_async_page().status ==
            SampleStreamAsyncReserveStatus::AlreadyReady);
    REQUIRE(cache.cancel_async_reservation(first.reservation) ==
            SampleStreamAsyncCompletionStatus::StaleReservation);

    REQUIRE(cache.retire_source_after_asset_unpublish({90, 1}) ==
            SampleStreamSourceRetireStatus::Scheduled);
    REQUIRE(cache.update_audio_generations(2, 1));
    REQUIRE(cache.collect_retired_sources() == 0);
    REQUIRE(cache.cancel_async_reservation(second.reservation) ==
            SampleStreamAsyncCompletionStatus::Canceled);
    REQUIRE(cache.collect_retired_sources() == 1);
}

TEST_CASE("Async service pipelines multiple pages from one source",
          "[audio][sampler][async-stream][pipeline]") {
    SampleStreamAsyncService<3, 3> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 8,
            .page_memory_budget_bytes = 3 * 4 * sizeof(float),
        },
        .decode = {
            .worker_count = 1,
            .source_capacity = 1,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
            .maximum_outstanding_jobs_per_source = 3,
        },
    }));
    const auto added = service.add_source({
        .token = {91, 1},
        .channels = 1,
        .total_frames = 12,
        .page_frames = 4,
        .cache_page_count = 3,
    }, constant_reader(91.0f));
    REQUIRE(added.added());
    for (std::uint64_t page = 0; page < 3; ++page) {
        REQUIRE(service.request_page(demand(1, page, 91)) ==
                SampleStreamScheduleStatus::Inserted);
        REQUIRE(service.dispatch_once().status ==
                SampleStreamAsyncDispatchStatus::Queued);
    }
    for (std::uint64_t page = 0; page < 3; ++page) {
        REQUIRE(service.wait_process_completion(0) ==
                SampleStreamAsyncPollStatus::Published);
        REQUIRE(added.view.window->ready_page_for_frame(1, page * 4).valid);
    }
    REQUIRE(service.telemetry().active_reservations_high_water == 3);
    REQUIRE(service.cache_stats().async_reservations_high_water == 3);
    REQUIRE(service.decode_telemetry().source_outstanding_high_water == 3);
    REQUIRE(service.decode_telemetry().same_source_reader_concurrency_high_water == 1);
}

TEST_CASE("Async retry preserves same-source serial order across a reused record hole",
          "[audio][sampler][async-stream][pipeline][queue-full]") {
    SampleStreamAsyncService<2, 2, QueueFullDispatchCutpoint> service;
    REQUIRE(service.prepare({
        .cache = {
            .scheduler_capacity = 16,
            .page_memory_budget_bytes = 9 * 2 * 4 * sizeof(float),
        },
        .decode = {
            .worker_count = 2,
            .source_capacity = 9,
            .maximum_channels = 1,
            .maximum_frames_per_job = 4,
            .maximum_outstanding_jobs_per_source = 2,
        },
    }));

    DecodeGate active_gate;
    DecodeGate queued_gate;
    REQUIRE(service.add_source(one_page_source(100),
                               [&](std::uint64_t start,
                                   pulp::audio::BufferView<float> destination,
                                   std::uint64_t frames) {
                                   return active_gate.read(start, destination, frames);
                               }).added()); // worker 0
    const auto hole = service.add_source(one_page_source(101),
                                         constant_reader(101.0f)); // worker 1
    REQUIRE(hole.added());
    REQUIRE(service.add_source(one_page_source(102),
                               [&](std::uint64_t start,
                                   pulp::audio::BufferView<float> destination,
                                   std::uint64_t frames) {
                                   return queued_gate.read(start, destination, frames);
                               }).added()); // worker 0
    REQUIRE(service.add_source(one_page_source(103), constant_reader(103.0f)).added());
    REQUIRE(service.add_source(one_page_source(104), constant_reader(104.0f)).added());
    REQUIRE(service.add_source(one_page_source(105), constant_reader(105.0f)).added());
    const auto ordered = service.add_source({
        .token = {106, 1},
        .channels = 1,
        .total_frames = 8,
        .page_frames = 4,
        .cache_page_count = 2,
    }, constant_reader(106.0f)); // worker 0
    REQUIRE(ordered.added());

    REQUIRE(service.request_page(demand(1, 0, 101)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.request_page(demand(1, 0, 100)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(active_gate.wait_until_entered());
    REQUIRE(service.request_page(demand(1, 0, 102)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.request_page(demand(1, 0, 104)) ==
            SampleStreamScheduleStatus::Inserted);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);

    REQUIRE(service.request_page(demand(1, 0, 106)) ==
            SampleStreamScheduleStatus::Inserted);
    const auto first_full = service.dispatch_once();
    REQUIRE(first_full.submit_status ==
            pulp::audio::SampleStreamDecodeSubmitStatus::QueueFull);
    REQUIRE(service.wait_process_completion(1) ==
            SampleStreamAsyncPollStatus::Published); // frees record slot zero
    REQUIRE(hole.view.window->ready_page_for_frame(1, 0).valid);

    REQUIRE(service.request_page(demand(1, 1, 106)) ==
            SampleStreamScheduleStatus::Inserted);
    const auto retained_later = [&] {
        QueueFullDispatchCutpoint::Scope cutpoint(active_gate, queued_gate);
        return service.dispatch_once();
    }();
    REQUIRE_FALSE(QueueFullDispatchCutpoint::timed_out.load(
        std::memory_order_acquire));
    REQUIRE(retained_later.submit_status ==
            pulp::audio::SampleStreamDecodeSubmitStatus::QueueFull);

    // The newly freed mailbox slot must go to the older retained serial even
    // though the newer reservation reused record slot zero and is scanned first.
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    queued_gate.allow();
    for (int completion = 0; completion < 4; ++completion) {
        REQUIRE(service.wait_process_completion(0) ==
                SampleStreamAsyncPollStatus::Published);
    }
    REQUIRE(ordered.view.window->ready_page_for_frame(1, 0).valid);
    REQUIRE_FALSE(ordered.view.window->ready_page_for_frame(1, 4).valid);
    REQUIRE(service.dispatch_once().status == SampleStreamAsyncDispatchStatus::Queued);
    REQUIRE(service.wait_process_completion(0) ==
            SampleStreamAsyncPollStatus::Published);
    REQUIRE(ordered.view.window->ready_page_for_frame(1, 4).valid);
    REQUIRE(service.decode_telemetry().stale_reservation == 0);
}
