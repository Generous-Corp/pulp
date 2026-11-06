#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/async_block_worker.hpp>
#include <pulp/audio/pull_rate_detector.hpp>

#include <chrono>
#include <cstdint>
#include <random>
#include <thread>
#include <vector>

using pulp::audio::AsyncBlockMode;
using pulp::audio::AsyncBlockWorker;
using pulp::audio::AsyncBlockWorkerConfig;
using pulp::audio::BufferView;
using pulp::audio::PullRateDetector;

namespace {

using Worker = AsyncBlockWorker<int>;
using TimedEvent = Worker::TimedEvent;

// An effect job that copies input to output and echoes every input event at
// its own offset, so the expected output is the input delayed by exactly the
// reported latency.
void identity_job(Worker::Chunk& chunk) {
    for (std::uint32_t i = 0; i < chunk.frames; ++i)
        chunk.output.channel_ptr(0)[i] = chunk.input.channel_ptr(0)[i];
    for (const auto& timed : chunk.events)
        chunk.emit(timed.offset, timed.event);
}

AsyncBlockWorkerConfig mono_config(std::uint32_t latency_blocks, bool thread = false) {
    AsyncBlockWorkerConfig config;
    config.sample_rate = 48000.0;
    config.input_channels = 1;
    config.output_channels = 1;
    config.max_block = 256;
    config.latency_blocks = latency_blocks;
    config.max_latency_blocks = 4;
    config.max_chunk = 64; // worker chunks unrelated to host blocks
    config.run_worker_thread = thread;
    return config;
}

// Drives one mono block whose input sample at stream position p is p + 1, so
// an output sample names the position it came from (0: silence).
struct Driver {
    Worker& worker;
    std::uint64_t position = 0;
    std::vector<float> in = std::vector<float>(4096);
    std::vector<float> out = std::vector<float>(4096);
    std::vector<std::pair<std::uint64_t, int>> events_out; // (delivered position, event)
    std::uint64_t delivered = 0;

    void block(std::uint32_t frames, std::span<const TimedEvent> events = {},
               AsyncBlockMode mode = AsyncBlockMode::Realtime) {
        for (std::uint32_t i = 0; i < frames; ++i)
            in[i] = static_cast<float>(position + i + 1);
        const float* in_ptr = in.data();
        float* out_ptr = out.data();
        BufferView<const float> input(&in_ptr, 1, frames);
        BufferView<float> output(&out_ptr, 1, frames);
        const auto base = delivered;
        worker.process(
            input, output, frames, events,
            [&](std::uint32_t offset, int event) { events_out.push_back({base + offset, event}); },
            mode);
        position += frames;
        delivered += frames;
    }
};

} // namespace

TEST_CASE("AsyncBlockWorker: latency is exact for random host block sizes",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(2), identity_job));
    const std::uint32_t latency = 2 * 256;
    REQUIRE(worker.latency_samples() == latency);
    Driver driver{worker};
    std::mt19937 rng(1234);
    std::uniform_int_distribution<std::uint32_t> size(1, 256);
    for (int call = 0; call < 3000; ++call) {
        const auto frames = size(rng);
        const auto start = driver.delivered;
        driver.block(frames);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto t = start + i;
            const float expected = t >= latency ? static_cast<float>(t - latency + 1) : 0.0f;
            REQUIRE(driver.out[i] == expected);
        }
        worker.pump(); // the worker keeps up between callbacks
    }
    REQUIRE(worker.stats().miss_frames == 0);
}

TEST_CASE("AsyncBlockWorker: a stalled worker costs silence, never drift",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(1), identity_job));
    const std::uint32_t latency = 256;
    Driver driver{worker};
    std::mt19937 rng(99);
    std::uniform_int_distribution<std::uint32_t> size(16, 256);
    std::uint64_t silent_after_preroll = 0;
    for (int call = 0; call < 4000; ++call) {
        const auto frames = size(rng);
        const auto start = driver.delivered;
        driver.block(frames);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const auto t = start + i;
            if (driver.out[i] == 0.0f) {
                if (t >= latency)
                    ++silent_after_preroll;
                continue;
            }
            // Whatever plays is the frame exactly `latency` earlier.
            REQUIRE(driver.out[i] == static_cast<float>(t - latency + 1));
        }
        // Stall the worker on every 7th callback.
        if (call % 7 != 0)
            worker.pump();
    }
    REQUIRE(worker.stats().miss_frames > 0);
    REQUIRE(silent_after_preroll == worker.stats().miss_frames);
    // Caught up: the next blocks are complete again.
    worker.pump();
    const auto start = driver.delivered;
    driver.block(128);
    for (std::uint32_t i = 0; i < 128; ++i)
        REQUIRE(driver.out[i] == static_cast<float>(start + i - latency + 1));
}

TEST_CASE("AsyncBlockWorker: events keep their frame through the delay",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(1), identity_job));
    Driver driver{worker};
    std::vector<std::uint64_t> sent_at;
    std::mt19937 rng(7);
    std::uniform_int_distribution<std::uint32_t> size(1, 256);
    for (int call = 0; call < 500; ++call) {
        const auto frames = size(rng);
        std::vector<TimedEvent> events;
        for (std::uint32_t offset = 0; offset < frames; offset += 37) {
            events.push_back({offset, static_cast<int>(sent_at.size())});
            sent_at.push_back(driver.position + offset);
        }
        driver.block(frames, events);
        worker.pump();
    }
    REQUIRE(worker.stats().late_events == 0);
    REQUIRE(driver.events_out.size() <= sent_at.size());
    REQUIRE(driver.events_out.size() > sent_at.size() / 2);
    for (const auto& [delivered_at, id] : driver.events_out)
        REQUIRE(delivered_at == sent_at[static_cast<std::size_t>(id)] + 256);
}

TEST_CASE("AsyncBlockWorker: an event whose frame already played arrives late, not lost",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(1), identity_job));
    Driver driver{worker};
    const TimedEvent note_off{10, 42};
    driver.block(128, std::span<const TimedEvent>(&note_off, 1));
    // No pump: the frame of the event plays (as silence) before the worker
    // renders it.
    for (int i = 0; i < 4; ++i)
        driver.block(128);
    REQUIRE(driver.events_out.empty());
    worker.pump();
    driver.block(128);
    REQUIRE(driver.events_out.size() == 1);
    REQUIRE(driver.events_out[0].second == 42);
    REQUIRE(driver.events_out[0].first == driver.delivered - 128); // offset 0 of this call
    REQUIRE(worker.stats().late_events == 1);
}

TEST_CASE("AsyncBlockWorker: a latency change is signalled once and never replays audio",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(1), identity_job));
    int signalled = 0;
    worker.set_latency_listener([](void* context) noexcept { ++*static_cast<int*>(context); },
                                &signalled);
    Driver driver{worker};
    for (int i = 0; i < 8; ++i) {
        driver.block(200);
        worker.pump();
    }
    REQUIRE(signalled == 0);

    REQUIRE(worker.request_latency_blocks(3));
    REQUIRE_FALSE(worker.request_latency_blocks(5)); // past max_latency_blocks
    REQUIRE(worker.latency_samples() == 3 * 256);
    float last = 0.0f;
    std::uint64_t silent = 0;
    for (int i = 0; i < 8; ++i) {
        const auto start = driver.delivered;
        driver.block(200);
        worker.pump();
        for (std::uint32_t k = 0; k < 200; ++k) {
            const float v = driver.out[k];
            if (v == 0.0f) {
                ++silent;
                continue;
            }
            REQUIRE(v > last); // no frame plays twice
            last = v;
            REQUIRE(v == static_cast<float>(start + k - 3 * 256 + 1));
        }
    }
    REQUIRE(signalled == 1);
    REQUIRE(silent == 2 * 256); // the window moved back two blocks once

    worker.set_worker_latency_samples(17);
    REQUIRE(worker.latency_samples() == 3 * 256 + 17);
    driver.block(64);
    REQUIRE(signalled == 2);
    REQUIRE(worker.stats().latency_changes == 2);
}

TEST_CASE("AsyncBlockWorker: offline and zero latency wait for the worker",
          "[audio][async-block-worker]") {
    SECTION("offline mode") {
        Worker worker;
        REQUIRE(worker.prepare(mono_config(1), identity_job));
        Driver driver{worker};
        for (int i = 0; i < 50; ++i) {
            const auto start = driver.delivered;
            driver.block(256, {}, AsyncBlockMode::Offline); // never pumped by the test
            for (std::uint32_t k = 0; k < 256; ++k) {
                const auto t = start + k;
                REQUIRE(driver.out[k] == (t >= 256 ? static_cast<float>(t - 256 + 1) : 0.0f));
            }
        }
        REQUIRE(worker.stats().miss_frames == 0);
        REQUIRE(worker.stats().waits > 0);
    }
    SECTION("zero latency") {
        Worker worker;
        REQUIRE(worker.prepare(mono_config(0), identity_job));
        REQUIRE(worker.latency_samples() == 0);
        Driver driver{worker};
        for (int i = 0; i < 20; ++i) {
            const auto start = driver.delivered;
            driver.block(100);
            for (std::uint32_t k = 0; k < 100; ++k)
                REQUIRE(driver.out[k] == static_cast<float>(start + k + 1));
        }
        REQUIRE(worker.stats().miss_frames == 0);
    }
}

TEST_CASE("AsyncBlockWorker: a worker a whole ring behind skips ahead and recovers",
          "[audio][async-block-worker]") {
    Worker worker;
    REQUIRE(worker.prepare(mono_config(1), identity_job));
    Driver driver{worker};
    for (int i = 0; i < 64; ++i) // 64 * 256 frames, far beyond the ring
        driver.block(256);
    worker.pump();
    REQUIRE(worker.stats().overrun_frames > 0);
    driver.block(256);
    worker.pump();
    for (int i = 0; i < 4; ++i) {
        const auto start = driver.delivered;
        driver.block(256);
        worker.pump();
        for (std::uint32_t k = 0; k < 256; ++k)
            REQUIRE(driver.out[k] == static_cast<float>(start + k - 256 + 1));
    }
}

TEST_CASE("AsyncBlockWorker: a fast pull without an offline flag waits in Auto mode",
          "[audio][async-block-worker][concurrent]") {
    // A slow job on a real worker thread, pulled back to back: Realtime
    // misses, Auto detects the pull rate and waits instead.
    const auto slow_job = [](Worker::Chunk& chunk) {
        std::this_thread::sleep_for(std::chrono::microseconds(150));
        identity_job(chunk);
    };
    auto run = [&](AsyncBlockMode mode) {
        Worker worker;
        auto config = mono_config(1, true);
        config.max_chunk =
            256; // one chunk per block: a slow sleep still leaves the pull well over 2x
        REQUIRE(worker.prepare(config, slow_job));
        Driver driver{worker};
        std::uint64_t misses_after_warmup = 0;
        for (int call = 0; call < 200; ++call) {
            const auto before = worker.stats().miss_frames;
            driver.block(256, {}, mode);
            if (call >= 2 * static_cast<int>(PullRateDetector::kWindow))
                misses_after_warmup += worker.stats().miss_frames - before;
        }
        const auto stats = worker.stats();
        worker.release();
        return std::pair{misses_after_warmup, stats};
    };
    const auto [realtime_misses, realtime_stats] = run(AsyncBlockMode::Realtime);
    const auto [auto_misses, auto_stats] = run(AsyncBlockMode::Auto);
    REQUIRE(realtime_misses > 0);
    REQUIRE(auto_stats.fast_pull);
    REQUIRE(auto_misses == 0);
    REQUIRE(auto_stats.wait_timeouts == 0);
}

TEST_CASE("PullRateDetector: real time stays live, a fast pull is detected with hysteresis",
          "[audio][pull-rate]") {
    using Clock = PullRateDetector::Clock;
    const auto block = std::chrono::microseconds(5333); // 256 frames at 48 kHz
    PullRateDetector detector;
    auto now = Clock::time_point{};
    auto feed = [&](double speed, int calls) {
        bool fast = false;
        for (int i = 0; i < calls; ++i) {
            now += std::chrono::duration_cast<Clock::duration>(block / speed);
            fast = detector.observe(256, 48000.0, now);
        }
        return fast;
    };
    REQUIRE_FALSE(feed(1.0, 32));
    REQUIRE_FALSE(feed(1.8, 32)); // below the entry threshold
    REQUIRE(feed(4.0, 32));
    REQUIRE(feed(1.5, 32));       // above the exit threshold: still fast
    REQUIRE_FALSE(feed(1.0, 32)); // back to real time
    // A host that splits one device cycle into two back-to-back calls still
    // averages to real time over the window.
    detector.reset();
    bool fast = false;
    for (int i = 0; i < 64; ++i) {
        now += std::chrono::microseconds(10);
        fast = detector.observe(128, 48000.0, now);
        now += std::chrono::duration_cast<Clock::duration>(block) - std::chrono::microseconds(10);
        fast = detector.observe(128, 48000.0, now) || fast;
    }
    REQUIRE_FALSE(fast);
}
