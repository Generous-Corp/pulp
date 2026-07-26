#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/playback/buffered_content_source.hpp>
#include <pulp/timeline/production_mode.hpp>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using namespace pulp;

namespace {

constexpr std::uint32_t kSampleRate = 48'000;
constexpr std::uint64_t kDeclaredFrames = 4096;
constexpr std::uint64_t kChunkFrames = 512;

// Read at run time, and written back through a volatile pointer, so the
// positive-control allocation below cannot be folded away.
volatile std::size_t g_escaping_size = 64;
float* volatile g_escaping_block = nullptr;

timeline::ProductionDeclaration buffered_declaration() {
    timeline::ProductionDeclaration declaration;
    declaration.mode = timeline::ProductionMode::Buffered;
    declaration.reproducibility = timeline::ReproducibilityClass::BestEffort;
    declaration.lookahead_ms = 100;
    return declaration;
}

playback::BufferedContentSourceConfig default_config() {
    playback::BufferedContentSourceConfig config;
    config.channels = 1;
    config.declared_frames = kDeclaredFrames;
    config.sample_rate = kSampleRate;
    config.produce_chunk_frames = kChunkFrames;
    config.start_background_thread = false;
    return config;
}

/// Produces `available_frames` frames of a fixed value and then nothing.
playback::BufferedContentProducer bounded_producer(std::uint64_t available_frames, float value) {
    return [available_frames, value](std::uint64_t start_frame, audio::BufferView<float> dest,
                                     std::uint64_t frames,
                                     audio::FrameReaderStopToken) -> std::uint64_t {
        if (start_frame >= available_frames)
            return 0;
        const auto produced = std::min(frames, available_frames - start_frame);
        for (std::size_t channel = 0; channel < dest.num_channels(); ++channel)
            for (std::uint64_t frame = 0; frame < produced; ++frame)
                dest.channel(channel)[static_cast<std::size_t>(frame)] = value;
        return produced;
    };
}

/// Drives the source to its declared length in fixed blocks, pumping the
/// producer between blocks the way a host would.
void drain(playback::BufferedContentSource& source, audio::Buffer<float>& block,
           std::uint64_t total_frames, std::uint64_t block_frames) {
    for (std::uint64_t emitted = 0; emitted < total_frames; emitted += block_frames) {
        source.pump_background();
        for (std::size_t channel = 0; channel < block.view().num_channels(); ++channel)
            for (auto& sample : block.view().channel(channel))
                sample = 1.0f;
        source.pull(block.view(), block_frames);
    }
}

bool all_zero(const audio::Buffer<float>& block) {
    auto view = block.view();
    for (std::size_t channel = 0; channel < view.num_channels(); ++channel)
        for (const auto sample : view.channel(channel))
            if (sample != 0.0f)
                return false;
    return true;
}

} // namespace

TEST_CASE("a buffered source refuses an incoherent production declaration",
          "[playback][production]") {
    playback::BufferedContentSource source;

    timeline::ProductionDeclaration synchronous;
    REQUIRE_FALSE(
        source.prepare(synchronous, default_config(), bounded_producer(kDeclaredFrames, 0.5f)));
    REQUIRE_FALSE(source.prepared());

    auto no_lookahead = buffered_declaration();
    no_lookahead.lookahead_ms = 0;
    REQUIRE_FALSE(
        source.prepare(no_lookahead, default_config(), bounded_producer(kDeclaredFrames, 0.5f)));

    REQUIRE(source.prepare(buffered_declaration(), default_config(),
                           bounded_producer(kDeclaredFrames, 0.5f)));
    REQUIRE(source.prepared());
    REQUIRE(source.declaration().reproducibility == timeline::ReproducibilityClass::BestEffort);
}

TEST_CASE("a buffered source that keeps up counts no starvation", "[playback][production]") {
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), default_config(),
                           bounded_producer(kDeclaredFrames, 0.5f)));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    drain(source, block, kDeclaredFrames, kChunkFrames);

    const auto stats = source.stats();
    REQUIRE(stats.produced_frames == kDeclaredFrames);
    REQUIRE(stats.starved_frames == 0);
    REQUIRE(stats.starvation_events == 0);
    REQUIRE(source.exhausted());
}

TEST_CASE("a buffered source that never produces zero-fills and counts every declared frame",
          "[playback][production]") {
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), default_config(), bounded_producer(0, 0.5f)));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    drain(source, block, kDeclaredFrames, kChunkFrames);

    const auto stats = source.stats();
    REQUIRE(stats.produced_frames == 0);
    REQUIRE(stats.starved_frames == kDeclaredFrames);
    REQUIRE(stats.starvation_events == kDeclaredFrames / kChunkFrames);
    // The last block was pre-filled with ones before the pull, so an all-zero
    // block proves the source actually zero-filled rather than passing stale
    // audio through.
    REQUIRE(all_zero(block));
    // Deadline-bound streaming advances across every missing frame, so the
    // underlying ring also records the full shortfall. The wrapper still owns
    // the declaration-aware count used by production diagnostics.
    REQUIRE(stats.ring_underrun_frames == kDeclaredFrames);
    REQUIRE(source.position() == kDeclaredFrames);
    REQUIRE(source.exhausted());
}

TEST_CASE("a buffered source counts the shortfall of a producer that stops early",
          "[playback][production]") {
    constexpr std::uint64_t kAvailable = 1024;
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), default_config(),
                           bounded_producer(kAvailable, 0.25f)));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    drain(source, block, kDeclaredFrames, kChunkFrames);

    const auto stats = source.stats();
    REQUIRE(stats.produced_frames == kAvailable);
    REQUIRE(stats.starved_frames == kDeclaredFrames - kAvailable);
    REQUIRE(stats.starvation_events == (kDeclaredFrames - kAvailable) / kChunkFrames);
    REQUIRE(stats.producer_errors == 1);
    REQUIRE(source.position() == kDeclaredFrames);
    REQUIRE(source.exhausted());
}

TEST_CASE("a buffered source discards frames that miss their playhead deadline",
          "[playback][production]") {
    auto config = default_config();
    config.ring_capacity_frames = kChunkFrames;
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [](std::uint64_t start_frame, audio::BufferView<float> dest,
           std::uint64_t frames, audio::FrameReaderStopToken) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);

    // Do not pump: this block misses its deadline and becomes silence.
    REQUIRE(source.pull(block.view(), kChunkFrames) == 0);
    REQUIRE(all_zero(block));
    REQUIRE(source.position() == 2 * kChunkFrames);

    // The next refill starts at the current playhead, not at the missed block.
    REQUIRE(source.pump_background() == kChunkFrames);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(2 * kChunkFrames));

    const auto stats = source.stats();
    REQUIRE(stats.produced_frames == 2 * kChunkFrames);
    REQUIRE(stats.starved_frames == kChunkFrames);
    REQUIRE(stats.produced_frames + stats.starved_frames == source.position());
}

TEST_CASE("a concurrent producer cannot publish a missed block into the next deadline",
          "[playback][production]") {
    auto config = default_config();
    config.ring_capacity_frames = kChunkFrames;
    config.start_background_thread = true;

    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> late_read_started{false};
    std::atomic<bool> release_late_read{false};
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [&](std::uint64_t start_frame, audio::BufferView<float> dest,
            std::uint64_t frames, audio::FrameReaderStopToken stop) {
            const auto call = calls.fetch_add(1, std::memory_order_relaxed);
            if (call == 1) {
                late_read_started.store(true, std::memory_order_release);
                while (!release_late_read.load(std::memory_order_acquire) &&
                       !stop.stop_requested())
                    std::this_thread::yield();
            }
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && !late_read_started.load(std::memory_order_acquire);
         ++spins)
        std::this_thread::yield();
    REQUIRE(late_read_started.load(std::memory_order_acquire));

    // Advance through the block while its producer call is still in flight.
    REQUIRE(source.pull(block.view(), kChunkFrames) == 0);
    REQUIRE(source.position() == 2 * kChunkFrames);
    release_late_read.store(true, std::memory_order_release);

    for (std::uint32_t spins = 0;
         spins < 1'000'000 && source.stats().ring_available_frames == 0;
         ++spins)
        std::this_thread::yield();
    REQUIRE(source.stats().ring_available_frames != 0);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(2 * kChunkFrames));
    source.release();
}

TEST_CASE("a partial underrun cannot retag a concurrent producer block",
          "[playback][production]") {
    auto config = default_config();
    config.ring_capacity_frames = kChunkFrames;
    config.start_background_thread = true;

    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> refill_started{false};
    std::atomic<bool> release_refill{false};
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [&](std::uint64_t start_frame, audio::BufferView<float> dest,
            std::uint64_t frames, audio::FrameReaderStopToken stop) {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
                refill_started.store(true, std::memory_order_release);
                while (!release_refill.load(std::memory_order_acquire) &&
                       !stop.stop_requested())
                    std::this_thread::yield();
            }
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> half_block(1, static_cast<std::size_t>(kChunkFrames / 2));
    REQUIRE(source.pull(half_block.view(), kChunkFrames / 2) == kChunkFrames / 2);
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && !refill_started.load(std::memory_order_acquire);
         ++spins)
        std::this_thread::yield();
    REQUIRE(refill_started.load(std::memory_order_acquire));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames / 2);
    REQUIRE(source.position() == kChunkFrames + kChunkFrames / 2);
    release_refill.store(true, std::memory_order_release);

    for (std::uint32_t spins = 0;
         spins < 1'000'000 && source.stats().ring_available_frames == 0;
         ++spins)
        std::this_thread::yield();
    REQUIRE(source.stats().ring_available_frames != 0);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() ==
            static_cast<float>(kChunkFrames + kChunkFrames / 2));
    source.release();
}

TEST_CASE("the audio-thread pull path allocates nothing", "[playback][production][rt-safety]") {
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), default_config(),
                           bounded_producer(kDeclaredFrames, 0.5f)));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));

    bool control_saw_allocation = false;
    {
        // Positive control. The allocation has to escape or -O3 elides it and
        // the probe silently proves nothing.
        test::RtAllocationProbe control;
        auto* escaping = new float[g_escaping_size];
        escaping[0] = 1.0f;
        g_escaping_block = escaping;
        control_saw_allocation = control.saw_allocation();
        delete[] g_escaping_block;
    }
    REQUIRE(control_saw_allocation);

    bool pull_saw_allocation = false;
    std::uint64_t produced = 0;
    {
        test::RtAllocationProbe probe;
        produced = source.pull(block.view(), kChunkFrames);
        pull_saw_allocation = probe.saw_allocation();
    }
    REQUIRE(produced == kChunkFrames);
    REQUIRE_FALSE(pull_saw_allocation);
}

TEST_CASE("counters stay consistent with the play cursor across the producer thread",
          "[playback][production]") {
    constexpr std::uint64_t kLongFrames = 48'000;
    constexpr std::uint64_t kBlockFrames = 512;

    auto config = default_config();
    config.declared_frames = kLongFrames;
    config.produce_chunk_frames = 1024;
    config.start_background_thread = true;

    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), config, bounded_producer(kLongFrames, 0.5f)));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kBlockFrames));
    // Bounded so a producer thread that falls behind ends the loop instead of
    // spinning: the invariants below hold whether or not it kept up, which is
    // what keeps this free of a timing assumption.
    const std::uint64_t max_pulls = (kLongFrames / kBlockFrames) * 4;
    for (std::uint64_t pulls = 0; pulls < max_pulls && !source.exhausted(); ++pulls)
        source.pull(block.view(), kBlockFrames);

    const auto position = source.position();
    const auto stats = source.stats();
    source.release(); // Joins the producer thread before anything is asserted.

    // Every frame that reached the playhead was either produced on time or
    // permanently counted as starved.
    REQUIRE(stats.produced_frames + stats.starved_frames == position);
    REQUIRE(stats.produced_frames <= kLongFrames);
    // The producer was never asked past its declared end, so nothing it
    // returned should have registered as a failed read.
    REQUIRE(stats.producer_errors == 0);
}

TEST_CASE("a buffered source sizes its ring from the declared wall-clock lookahead",
          "[playback][production]") {
    auto declaration = buffered_declaration();
    declaration.lookahead_ms = 250;
    REQUIRE(playback::BufferedContentSource::lookahead_frames(declaration, kSampleRate) == 12'000);
    REQUIRE(playback::BufferedContentSource::lookahead_frames(declaration, 44'100) == 11'025);

    timeline::ProductionDeclaration synchronous;
    REQUIRE(playback::BufferedContentSource::lookahead_frames(synchronous, kSampleRate) == 0);
}
