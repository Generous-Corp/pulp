// Background readers that the audio thread feeds through runtime::WakeSignal:
// they sleep while there is nothing to do (no idle wakeups) and refill
// promptly when the audio thread consumes.
//
// Idle wakeups are measured on macOS from the process's own counters
// (proc_pid_rusage: interrupt + package-idle wakeups, what Activity Monitor
// reports as "Idle Wake Ups"). Each measurement first proves the instrument
// can see a 100 Hz polling thread; elsewhere the wakeup checks skip with a
// named reason, never pass.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffering_reader.hpp>
#include <pulp/audio/streaming_sample_source.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using pulp::audio::BufferingReader;
using pulp::audio::BufferView;
using pulp::audio::StreamingSampleSource;
using pulp::audio::StreamingSampleSourceConfig;

namespace {

#if defined(__APPLE__)
std::uint64_t process_wakeups() {
    rusage_info_v4 info{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) != 0)
        return 0;
    return info.ri_interrupt_wkups + info.ri_pkg_idle_wkups;
}

// Wakeups per second of the whole process over `window`, the test thread
// itself sleeping throughout.
double wakeups_per_second(std::chrono::milliseconds window) {
    const auto before = process_wakeups();
    std::this_thread::sleep_for(window);
    const auto after = process_wakeups();
    return static_cast<double>(after - before) * 1000.0 / static_cast<double>(window.count());
}

// The instrument must see a thread that wakes every 10 ms; otherwise a low
// reading below means nothing.
bool instrument_sees_polling() {
    std::atomic<bool> run{true};
    std::thread poller([&] {
        while (run.load())
            std::this_thread::sleep_for(10ms);
    });
    const double rate = wakeups_per_second(1000ms);
    run = false;
    poller.join();
    return rate >= 50.0;
}
#endif

bool wait_until(const std::function<bool()>& done, std::chrono::milliseconds limit) {
    const auto end = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// A source that never ends: frame values follow the frame index.
int endless_source(float* dest, int frames, int channels, std::uint64_t& next) {
    for (int f = 0; f < frames; ++f, ++next)
        for (int c = 0; c < channels; ++c)
            dest[f * channels + c] = static_cast<float>(next % 1000);
    return frames;
}

} // namespace

TEST_CASE("BufferingReader: a full buffer refills promptly when the audio thread reads",
          "[audio][buffering][wake-signal]") {
    BufferingReader reader;
    std::uint64_t next = 0;
    reader.set_read_callback([&](float* dest, int frames, int channels) {
        return endless_source(dest, frames, channels, next);
    });
    reader.start(2, 4096);
    REQUIRE(wait_until([&] { return reader.frames_available() == 4096; }, 2000ms));
    // Let the background thread park on the full buffer.
    std::this_thread::sleep_for(50ms);

    std::vector<float> out(2 * 2048);
    REQUIRE(reader.read(out.data(), 2048, 2) == 2048);
    // The read wakes the reader at once; only a lost wake would leave it to
    // its 1 s backstop.
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(wait_until([&] { return reader.frames_available() == 4096; }, 2000ms));
    REQUIRE(std::chrono::steady_clock::now() - start < 300ms);
    // No sample lost or repeated across the refill.
    REQUIRE(reader.read(out.data(), 2048, 2) == 2048);
    REQUIRE(out[0] == 2048.0f);
    REQUIRE(out[2 * 2047] == static_cast<float>(4095 % 1000));
    reader.stop();
}

TEST_CASE("StreamingSampleSource: pulling wakes the reader to refill the ring",
          "[audio][streaming][wake-signal]") {
    StreamingSampleSource source;
    StreamingSampleSourceConfig config;
    config.channels = 1;
    config.total_frames = 48000 * 20;
    config.sample_rate = 48000;
    config.preload_frames = 256;
    config.ring_capacity_frames = 4096;
    config.read_chunk_frames = 1024;
    REQUIRE(source.prepare(config,
                           [](std::uint64_t start, BufferView<float> dest, std::uint64_t frames) {
                               for (std::uint64_t i = 0; i < frames; ++i)
                                   dest.channel(0)[i] = static_cast<float>((start + i) % 1000);
                               return frames;
                           }));
    std::this_thread::sleep_for(50ms);
    REQUIRE(source.stats().ring_available_frames == 4096);

    std::vector<float> storage(2048);
    float* channel = storage.data();
    BufferView<float> view(&channel, 1, 2048);
    REQUIRE(source.pull(view, 2048) == 2048);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(wait_until([&] { return source.stats().ring_available_frames == 4096; }, 2000ms));
    REQUIRE(std::chrono::steady_clock::now() - start < 300ms);
    // Sequential, gapless frames through the refill.
    REQUIRE(source.pull(view, 2048) == 2048);
    REQUIRE(storage[0] == static_cast<float>(2048 % 1000));
    REQUIRE(storage[2047] == static_cast<float>(4095 % 1000));
    REQUIRE(source.stats().underrun_frames == 0);
    source.release();
}

TEST_CASE("Audio background readers do not wake the CPU while idle",
          "[audio][wake-signal][idle-wakeups]") {
#if !defined(__APPLE__)
    SKIP("idle wakeups are measured from macOS process counters only");
#else
    REQUIRE(instrument_sees_polling());
    const double floor = wakeups_per_second(1000ms);

    SECTION("BufferingReader with a full, unread buffer") {
        BufferingReader reader;
        std::uint64_t next = 0;
        reader.set_read_callback([&](float* dest, int frames, int channels) {
            return endless_source(dest, frames, channels, next);
        });
        reader.start(2, 4096);
        REQUIRE(wait_until([&] { return reader.frames_available() == 4096; }, 2000ms));
        std::this_thread::sleep_for(100ms);
        const double rate = wakeups_per_second(2000ms) - floor;
        INFO("BufferingReader idle wakeups/s above the process floor: " << rate);
        CHECK(rate < 5.0);
        reader.stop();
    }
    SECTION("StreamingSampleSource with a full ring and no pulls") {
        StreamingSampleSource source;
        StreamingSampleSourceConfig config;
        config.channels = 2;
        config.total_frames = 48000 * 20;
        config.sample_rate = 48000;
        config.preload_frames = 256;
        config.ring_capacity_frames = 8192;
        REQUIRE(
            source.prepare(config, [](std::uint64_t, BufferView<float> dest, std::uint64_t frames) {
                for (std::size_t c = 0; c < dest.num_channels(); ++c)
                    std::fill_n(dest.channel_ptr(c), frames, 0.25f);
                return frames;
            }));
        std::this_thread::sleep_for(100ms);
        const double rate = wakeups_per_second(2000ms) - floor;
        INFO("StreamingSampleSource idle wakeups/s above the process floor: " << rate);
        CHECK(rate < 5.0);
        source.release();
    }
#endif
}
