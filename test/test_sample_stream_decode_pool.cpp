#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/sample_stream_decode_pool.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <latch>
#include <mutex>
#include <thread>

using pulp::audio::SampleStreamDecodeCancelStatus;
using pulp::audio::SampleStreamDecodeCompletion;
using pulp::audio::SampleStreamDecodeCompletionMatch;
using pulp::audio::SampleStreamDecodeCompletionStatus;
using pulp::audio::SampleStreamDecodeJob;
using pulp::audio::SampleStreamDecodePool;
using pulp::audio::SampleStreamDecodeSourceAddStatus;
using pulp::audio::SampleStreamDecodeSubmitStatus;
using pulp::audio::FrameReaderBinding;
using pulp::audio::FrameReaderStopMode;
using pulp::audio::match_sample_stream_decode_completion;

namespace {

class ReaderGate {
public:
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
        {
            std::lock_guard lock(mutex_);
            exited_ = true;
        }
        changed_.notify_all();
        return frames;
    }

    void wait_until_entered() {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return entered_; });
    }

    void allow() {
        {
            std::lock_guard lock(mutex_);
            allowed_ = true;
        }
        changed_.notify_all();
    }

    bool exited() const {
        std::lock_guard lock(mutex_);
        return exited_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool allowed_ = false;
    bool exited_ = false;
};

class ConcurrencyGate {
public:
    std::uint64_t read(std::uint64_t start,
                       pulp::audio::BufferView<float> destination,
                       std::uint64_t frames) {
        {
            std::unique_lock lock(mutex_);
            ++entered_;
            ++active_;
            if (active_ > maximum_active_) maximum_active_ = active_;
            changed_.notify_all();
            changed_.wait(lock, [&] { return allowed_; });
            --active_;
        }
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            destination.channel_ptr(0)[frame] = static_cast<float>(start + frame);
        changed_.notify_all();
        return frames;
    }

    void wait_for_entries(std::uint32_t count) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return entered_ >= count; });
    }

    void allow() {
        {
            std::lock_guard lock(mutex_);
            allowed_ = true;
        }
        changed_.notify_all();
    }

    std::uint32_t maximum_active() const {
        std::lock_guard lock(mutex_);
        return maximum_active_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::uint32_t entered_ = 0;
    std::uint32_t active_ = 0;
    std::uint32_t maximum_active_ = 0;
    bool allowed_ = false;
};

auto ramp_reader() {
    return [](std::uint64_t start,
              pulp::audio::BufferView<float> destination,
              std::uint64_t frames) {
        for (std::uint64_t frame = 0; frame < frames; ++frame)
            destination.channel_ptr(0)[frame] = static_cast<float>(start + frame);
        return frames;
    };
}

}  // namespace

TEST_CASE("Sample stream decode pool bounds queued work behind an active reader",
          "[audio][sampler][decode-pool][pressure]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 4,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));

    ReaderGate gate;
    REQUIRE(pool.add_source({1, 1}, 1,
                            [&](std::uint64_t start,
                                pulp::audio::BufferView<float> destination,
                                std::uint64_t frames) {
                                return gate.read(start, destination, frames);
                            }).added());
    REQUIRE(pool.add_source({2, 1}, 1, ramp_reader()).added());
    REQUIRE(pool.add_source({3, 1}, 1, ramp_reader()).added());

    REQUIRE(pool.submit({.source = {1, 1},
                         .reservation_serial = 10,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    gate.wait_until_entered();
    REQUIRE(pool.submit({.source = {1, 1},
                         .reservation_serial = 11,
                         .frame_count = 4}) ==
            SampleStreamDecodeSubmitStatus::SourceInFlight);
    REQUIRE(pool.submit({.source = {2, 1},
                         .reservation_serial = 20,
                         .start_frame = 4,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    REQUIRE(pool.submit({.source = {3, 1},
                         .reservation_serial = 30,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::QueueFull);

    REQUIRE(pool.cancel_source({1, 1}) == SampleStreamDecodeCancelStatus::Canceled);
    REQUIRE(pool.add_source({1, 2}, 1, ramp_reader()).status ==
            SampleStreamDecodeSourceAddStatus::DuplicateSource);
    gate.allow();

    auto canceled = pool.wait_pop_completion(0);
    REQUIRE(canceled.has_value());
    REQUIRE(canceled->completion.status ==
            SampleStreamDecodeCompletionStatus::Canceled);
    REQUIRE(canceled->audio.num_samples() == 0);
    REQUIRE(pool.release_completion(canceled->completion));
    REQUIRE(pool.add_source({1, 2}, 1, ramp_reader()).added());

    auto decoded = pool.wait_pop_completion(0);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->completion.source.source_id == 2);
    REQUIRE(decoded->completion.status ==
            SampleStreamDecodeCompletionStatus::Decoded);
    REQUIRE(decoded->audio.num_samples() == 4);
    REQUIRE(decoded->audio.channel_ptr(0)[0] == 4.0f);
    REQUIRE(pool.release_completion(decoded->completion));
    REQUIRE(pool.submit({.source = {3, 1},
                         .reservation_serial = 30,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    auto retried = pool.wait_pop_completion(0);
    REQUIRE(retried.has_value());
    REQUIRE(retried->completion.source.source_id == 3);
    REQUIRE(retried->completion.reservation_serial == 30);
    REQUIRE(pool.release_completion(retried->completion));
    REQUIRE(pool.submit({.source = {1, 2},
                         .reservation_serial = 11,
                         .start_frame = 8,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);

    auto replacement = pool.wait_pop_completion(0);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->completion.source.source_generation == 2);
    REQUIRE(replacement->completion.reservation_serial == 11);
    REQUIRE(replacement->audio.channel_ptr(0)[0] == 8.0f);
    REQUIRE(pool.release_completion(replacement->completion));

    const auto telemetry = pool.telemetry();
    REQUIRE(telemetry.job_queue_full == 1);
    REQUIRE(telemetry.canceled_completions == 1);
}

TEST_CASE("Sample stream decode pool bounds cross-source concurrency by workers",
          "[audio][sampler][decode-pool][concurrency]") {
    SampleStreamDecodePool<2, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 2,
        .source_capacity = 3,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));
    ConcurrencyGate gate;
    auto add = [&](std::uint64_t source_id) {
        return pool.add_source(
            {source_id, 1},
            1,
            [&](std::uint64_t start,
                pulp::audio::BufferView<float> destination,
                std::uint64_t frames) {
                return gate.read(start, destination, frames);
            });
    };
    REQUIRE(add(50).worker_index == 0);
    REQUIRE(add(51).worker_index == 1);
    REQUIRE(add(52).worker_index == 0);
    REQUIRE(pool.submit({.source = {50, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    REQUIRE(pool.submit({.source = {51, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    gate.wait_for_entries(2);
    REQUIRE(pool.submit({.source = {52, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    REQUIRE(gate.maximum_active() == 2);
    gate.allow();

    auto worker_zero = pool.wait_pop_completion(0);
    auto worker_one = pool.wait_pop_completion(1);
    REQUIRE(worker_zero.has_value());
    REQUIRE(worker_one.has_value());
    REQUIRE(pool.release_completion(worker_zero->completion));
    REQUIRE(pool.release_completion(worker_one->completion));
    auto queued = pool.wait_pop_completion(0);
    REQUIRE(queued.has_value());
    REQUIRE(queued->completion.source.source_id == 52);
    REQUIRE(pool.release_completion(queued->completion));
    REQUIRE(gate.maximum_active() == 2);
}

TEST_CASE("Sample stream decode pool rejects stale generations and reservations",
          "[audio][sampler][decode-pool][generation]") {
    SampleStreamDecodePool<2, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));
    REQUIRE(pool.add_source({10, 4}, 1, ramp_reader()).added());
    REQUIRE(pool.submit({.source = {10, 4},
                         .reservation_serial = 7,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    auto completion = pool.wait_pop_completion(0);
    REQUIRE(completion.has_value());
    REQUIRE(pool.release_completion(completion->completion));

    REQUIRE(pool.submit({.source = {10, 4},
                         .reservation_serial = 7,
                         .frame_count = 4}) ==
            SampleStreamDecodeSubmitStatus::StaleReservation);
    REQUIRE(pool.submit({.source = {10, 5},
                         .reservation_serial = 8,
                         .frame_count = 4}) ==
            SampleStreamDecodeSubmitStatus::StaleGeneration);

    SampleStreamDecodeCompletion candidate{
        .source = {10, 4},
        .reservation_serial = 9,
        .requested_frames = 4,
        .decoded_frames = 4,
        .status = SampleStreamDecodeCompletionStatus::Decoded,
    };
    REQUIRE(match_sample_stream_decode_completion(candidate, {11, 4}, 9) ==
            SampleStreamDecodeCompletionMatch::StaleSource);
    REQUIRE(match_sample_stream_decode_completion(candidate, {10, 5}, 9) ==
            SampleStreamDecodeCompletionMatch::StaleGeneration);
    REQUIRE(match_sample_stream_decode_completion(candidate, {10, 4}, 8) ==
            SampleStreamDecodeCompletionMatch::StaleReservation);
    REQUIRE(match_sample_stream_decode_completion(candidate, {10, 4}, 9) ==
            SampleStreamDecodeCompletionMatch::Accepted);

    const auto telemetry = pool.telemetry();
    REQUIRE(telemetry.stale_generation == 1);
    REQUIRE(telemetry.stale_reservation == 1);
}

TEST_CASE("Sample stream decode pool joins a blocked legacy reader before teardown",
          "[audio][sampler][decode-pool][teardown]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));
    ReaderGate gate;
    REQUIRE(pool.add_source({20, 1}, 1,
                            [&](std::uint64_t start,
                                pulp::audio::BufferView<float> destination,
                                std::uint64_t frames) {
                                return gate.read(start, destination, frames);
                            }).added());
    REQUIRE(pool.submit({.source = {20, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    gate.wait_until_entered();

    std::latch teardown_started(1);
    std::atomic<bool> teardown_returned{false};
    std::thread teardown([&] {
        teardown_started.count_down();
        pool.release();
        teardown_returned.store(true, std::memory_order_release);
    });
    teardown_started.wait();
    REQUIRE_FALSE(teardown_returned.load(std::memory_order_acquire));
    REQUIRE_FALSE(gate.exited());
    gate.allow();
    teardown.join();
    REQUIRE(gate.exited());
    REQUIRE(teardown_returned.load(std::memory_order_acquire));
    REQUIRE(pool.telemetry().stopped_jobs == 1);
}

TEST_CASE("Sample stream decode pool requests cooperative reader stop",
          "[audio][sampler][decode-pool][stop-token]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool observed_stop = false;
    FrameReaderBinding binding{
        .read = [&](std::uint64_t,
            pulp::audio::BufferView<float>,
            std::uint64_t,
            std::stop_token stop_token) {
            std::stop_callback notify(stop_token, [&] { changed.notify_all(); });
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return stop_token.stop_requested(); });
            observed_stop = true;
            return std::uint64_t{0};
        },
        .stop_mode = FrameReaderStopMode::Cooperative,
    };
    REQUIRE(pool.add_source({30, 1}, 1, std::move(binding)).added());
    REQUIRE(pool.submit({.source = {30, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    pool.release();
    REQUIRE(observed_stop);
    REQUIRE(pool.telemetry().stopped_jobs == 1);
}

TEST_CASE("Sample stream decode pool cancellation stops active cooperative I/O",
          "[audio][sampler][decode-pool][cancel][stop-token]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));

    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool observed_stop = false;
    FrameReaderBinding binding{
        .read = [&](std::uint64_t,
                    pulp::audio::BufferView<float>,
                    std::uint64_t,
                    std::stop_token stop_token) {
            std::stop_callback notify(stop_token, [&] { changed.notify_all(); });
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return stop_token.stop_requested(); });
            observed_stop = true;
            return std::uint64_t{0};
        },
        .stop_mode = FrameReaderStopMode::Cooperative,
    };
    REQUIRE(pool.add_source({31, 1}, 1, std::move(binding)).added());
    REQUIRE(pool.submit({.source = {31, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    REQUIRE(pool.cancel_source({31, 1}) == SampleStreamDecodeCancelStatus::Canceled);
    auto completion = pool.wait_pop_completion(0);
    REQUIRE(completion.has_value());
    REQUIRE(completion->completion.status ==
            SampleStreamDecodeCompletionStatus::Canceled);
    REQUIRE(observed_stop);
    REQUIRE(pool.release_completion(completion->completion));
    REQUIRE(pool.add_source({31, 2}, 1, ramp_reader()).added());
}

TEST_CASE("Sample stream decode pool reports reader errors",
          "[audio][sampler][decode-pool][error]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 1,
        .maximum_frames_per_job = 4,
    }));
    REQUIRE(pool.add_source(
        {35, 1},
        1,
        [](std::uint64_t, pulp::audio::BufferView<float>, std::uint64_t) {
            return std::uint64_t{0};
        }).added());
    REQUIRE(pool.submit({.source = {35, 1},
                         .reservation_serial = 1,
                         .frame_count = 4}) == SampleStreamDecodeSubmitStatus::Queued);
    auto completion = pool.wait_pop_completion(0);
    REQUIRE(completion.has_value());
    REQUIRE(completion->completion.status ==
            SampleStreamDecodeCompletionStatus::ShortRead);
    REQUIRE(match_sample_stream_decode_completion(completion->completion,
                                                  {35, 1},
                                                  1) ==
            SampleStreamDecodeCompletionMatch::DecodeError);
    REQUIRE(pool.telemetry().decode_errors == 1);
    REQUIRE(pool.release_completion(completion->completion));
}

TEST_CASE("Sample stream decode pool keeps prepared memory fixed across jobs",
          "[audio][sampler][decode-pool][bounded-memory]") {
    SampleStreamDecodePool<1, 1> pool;
    REQUIRE(pool.prepare({
        .worker_count = 1,
        .source_capacity = 1,
        .maximum_channels = 2,
        .maximum_frames_per_job = 8,
    }));
    REQUIRE(pool.reserved_scratch_samples() == 16);
    REQUIRE(pool.add_source({40, 1}, 1, ramp_reader()).added());

    bool all_completed = true;
    std::size_t owner_allocations = 0;
    {
        pulp::test::RtAllocationProbe allocation_probe;
        for (std::uint64_t serial = 1; serial <= 100; ++serial) {
            all_completed =
                pool.submit({.source = {40, 1},
                             .reservation_serial = serial,
                             .frame_count = 8}) == SampleStreamDecodeSubmitStatus::Queued &&
                all_completed;
            auto completion = pool.wait_pop_completion(0);
            all_completed = completion.has_value() && all_completed;
            if (completion)
                all_completed =
                    pool.release_completion(completion->completion) && all_completed;
        }
        owner_allocations = allocation_probe.allocation_count();
    }
    REQUIRE(all_completed);
    REQUIRE(owner_allocations == 0);
    REQUIRE(pool.reserved_scratch_samples() == 16);
    REQUIRE(pool.telemetry().completions_published == 100);
}
