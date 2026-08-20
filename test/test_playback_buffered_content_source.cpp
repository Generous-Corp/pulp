#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "harness/scoped_rt_process_probe.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/playback/buffered_content_source.hpp>
#include <pulp/timeline/production_mode.hpp>

#include <atomic>
#include <cstdint>
#include <stdexcept>
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
    config.max_pull_frames = kChunkFrames;
    config.produce_chunk_frames = kChunkFrames;
    config.start_background_thread = false;
    return config;
}

/// Produces `available_frames` frames of a fixed value and then nothing.
playback::BufferedContentProducer bounded_producer(std::uint64_t available_frames, float value) {
    return [available_frames, value](playback::BufferedContentEpoch, std::uint64_t start_frame,
                                     audio::BufferView<float> dest, std::uint64_t frames,
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
    REQUIRE(stats.producer_errors == 0);
    REQUIRE(source.position() == kDeclaredFrames);
    REQUIRE(source.exhausted());
}

TEST_CASE("a buffered producer can recover after returning no frames",
          "[playback][production]") {
    playback::BufferedContentSource source;
    std::uint32_t calls = 0;
    REQUIRE(source.prepare(
        buffered_declaration(), default_config(),
        [&calls](playback::BufferedContentEpoch, std::uint64_t, audio::BufferView<float> dest,
                 std::uint64_t frames, audio::FrameReaderStopToken) {
            if (calls++ == 0)
                return std::uint64_t{0};
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] = 0.5f;
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == 0);
    REQUIRE(source.position() == kChunkFrames);
    REQUIRE(source.pump_background() == kChunkFrames);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(source.position() == 2 * kChunkFrames);
    REQUIRE(source.stats().producer_errors == 0);
}

TEST_CASE("a buffered source reports producer exceptions as errors",
          "[playback][production]") {
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), default_config(),
        [](playback::BufferedContentEpoch, std::uint64_t, audio::BufferView<float>, std::uint64_t,
           audio::FrameReaderStopToken) -> std::uint64_t {
            throw std::runtime_error("producer failed");
        }));

    REQUIRE(source.stats().producer_errors == 1);
}

TEST_CASE("a buffered source discards frames that miss their playhead deadline",
          "[playback][production]") {
    auto config = default_config();
    config.ring_capacity_frames = kChunkFrames;
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [](playback::BufferedContentEpoch, std::uint64_t start_frame,
           audio::BufferView<float> dest, std::uint64_t frames, audio::FrameReaderStopToken) {
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
        [&](playback::BufferedContentEpoch, std::uint64_t start_frame,
            audio::BufferView<float> dest, std::uint64_t frames,
            audio::FrameReaderStopToken stop) {
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
        [&](playback::BufferedContentEpoch, std::uint64_t start_frame,
            audio::BufferView<float> dest, std::uint64_t frames,
            audio::FrameReaderStopToken stop) {
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

TEST_CASE("the audio-thread pull path allocates nothing and takes no lock",
          "[playback][production][rt-safety]") {
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

    std::size_t pull_allocations = 1;
    std::uint64_t produced = 0;
    {
        // The combined probe adds lock-freedom to the allocation assertion:
        // the trap backend aborts if the pull path takes a blocking lock while
        // the producer thread owns the other side of the ring.
        test::ScopedRtProcessProbe probe;
        produced = source.pull(block.view(), kChunkFrames);
        pull_allocations = probe.allocation_count();
    }
    REQUIRE(produced == kChunkFrames);
    REQUIRE(pull_allocations == 0);
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

    declaration.lookahead_ms = 1;
    auto config = default_config();
    config.declared_frames = 2 * kChunkFrames;
    config.produce_chunk_frames = kChunkFrames;
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(declaration, config,
                           bounded_producer(config.declared_frames, 0.5f)));
    REQUIRE(source.stats().ring_available_frames ==
            playback::BufferedContentSource::lookahead_frames(declaration, kSampleRate));

    config.ring_capacity_frames =
        playback::BufferedContentSource::lookahead_frames(declaration, kSampleRate);
    REQUIRE_FALSE(source.prepare(
        declaration, config, bounded_producer(config.declared_frames, 0.5f)));
}

TEST_CASE("a seek invalidates in flight production and no stale epoch frame reaches the ring",
          "[playback][production]") {
    auto config = default_config();
    config.declared_frames = 8 * kChunkFrames;
    config.ring_capacity_frames = kChunkFrames;
    config.start_background_thread = true;

    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> late_call_started{false};
    std::atomic<bool> late_call_saw_stop{false};
    std::atomic<playback::BufferedContentEpoch> last_epoch{0};
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [&](playback::BufferedContentEpoch epoch, std::uint64_t start_frame,
            audio::BufferView<float> dest, std::uint64_t frames,
            audio::FrameReaderStopToken stop) {
            last_epoch.store(epoch, std::memory_order_relaxed);
            if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
                // The second refill is in flight when the seek below lands.
                // It waits only on its stop token, so the seek alone ends it;
                // bounded so a broken stop signal fails the test, not hangs it.
                late_call_started.store(true, std::memory_order_release);
                for (std::uint32_t spins = 0;
                     spins < 50'000'000 && !stop.stop_requested();
                     ++spins)
                    std::this_thread::yield();
                late_call_saw_stop.store(stop.stop_requested(), std::memory_order_release);
            }
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    // Priming served the first chunk; consuming it opens the room the
    // in-flight second refill is producing for.
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && !late_call_started.load(std::memory_order_acquire);
         ++spins)
        std::this_thread::yield();
    REQUIRE(late_call_started.load(std::memory_order_acquire));

    REQUIRE(source.seek(3 * kChunkFrames, 2));
    REQUIRE(late_call_saw_stop.load(std::memory_order_acquire));
    REQUIRE(source.current_epoch() == 2);
    REQUIRE(source.position() == 3 * kChunkFrames);

    // Every frame the audio thread sees from here belongs to the new epoch:
    // the first pull carries the seek target, and the in-flight block's
    // position (kChunkFrames) never appears.
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(3 * kChunkFrames));
    REQUIRE(last_epoch.load(std::memory_order_relaxed) == 2);
    REQUIRE(source.pump_background() == kChunkFrames);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(4 * kChunkFrames));
    source.release();
}

TEST_CASE("cancellation stops the producer within one chunk and leaves counters coherent",
          "[playback][production]") {
    auto config = default_config();
    config.declared_frames = 8 * kChunkFrames;
    config.ring_capacity_frames = kChunkFrames;
    config.start_background_thread = true;

    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> cancel_target_started{false};
    std::atomic<bool> cancel_target_saw_stop{false};
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [&](playback::BufferedContentEpoch, std::uint64_t start_frame,
            audio::BufferView<float> dest, std::uint64_t frames,
            audio::FrameReaderStopToken stop) {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
                // Waits only on its stop token, so cancel_production() alone
                // ends the call; bounded so a broken signal fails, not hangs.
                cancel_target_started.store(true, std::memory_order_release);
                for (std::uint32_t spins = 0;
                     spins < 50'000'000 && !stop.stop_requested();
                     ++spins)
                    std::this_thread::yield();
                cancel_target_saw_stop.store(stop.stop_requested(), std::memory_order_release);
                return std::uint64_t{0};
            }
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && !cancel_target_started.load(std::memory_order_acquire);
         ++spins)
        std::this_thread::yield();
    REQUIRE(cancel_target_started.load(std::memory_order_acquire));

    // The interrupted chunk publishes nothing; cancellation does not latch,
    // so the same position is produced again once a pump runs.
    source.cancel_production();
    REQUIRE(cancel_target_saw_stop.load(std::memory_order_acquire));

    for (std::uint32_t spins = 0;
         spins < 1'000'000 && source.stats().ring_available_frames == 0;
         ++spins)
        std::this_thread::yield();
    REQUIRE(source.stats().ring_available_frames != 0);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(kChunkFrames));

    // Every frame the play cursor consumed is accounted exactly once.
    while (!source.exhausted())
        source.pull(block.view(), kChunkFrames);
    const auto stats = source.stats();
    source.release();
    REQUIRE(stats.produced_frames + stats.starved_frames == 8 * kChunkFrames);
}

TEST_CASE("a producer that ignores the stop token publishes nothing after cancellation",
          "[playback][production]") {
    auto config = default_config();
    config.declared_frames = 8 * kChunkFrames;
    config.ring_capacity_frames = kChunkFrames;
    config.start_background_thread = true;

    std::atomic<std::uint32_t> calls{0};
    std::atomic<bool> poison_call_started{false};
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [&](playback::BufferedContentEpoch, std::uint64_t start_frame,
            audio::BufferView<float> dest, std::uint64_t frames,
            audio::FrameReaderStopToken stop) {
            if (calls.fetch_add(1, std::memory_order_relaxed) == 1) {
                // The cancel target: waits for the stop request, then IGNORES
                // it and returns a full chunk of poison frames. Only the
                // source's discard keeps them out of the ring. Bounded so a
                // broken stop signal fails the test, not hangs it.
                poison_call_started.store(true, std::memory_order_release);
                for (std::uint32_t spins = 0;
                     spins < 50'000'000 && !stop.stop_requested();
                     ++spins)
                    std::this_thread::yield();
                for (std::uint64_t frame = 0; frame < frames; ++frame)
                    dest.channel(0)[static_cast<std::size_t>(frame)] = -1.0f;
                return frames;
            }
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && !poison_call_started.load(std::memory_order_acquire);
         ++spins)
        std::this_thread::yield();
    REQUIRE(poison_call_started.load(std::memory_order_acquire));

    source.cancel_production();

    // The poison chunk must never reach the audio thread: the next pull is the
    // retried production of the same position, not the -1 markers.
    for (std::uint32_t spins = 0;
         spins < 1'000'000 && source.stats().ring_available_frames == 0;
         ++spins)
        std::this_thread::yield();
    REQUIRE(source.stats().ring_available_frames != 0);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(kChunkFrames));
    source.release();
}

TEST_CASE("preroll frames are available before the first pull", "[playback][production]") {
    constexpr std::uint64_t kPreroll = 4 * kChunkFrames;
    auto declaration = buffered_declaration();
    // Lookahead below the preroll, so the preroll alone drives the sizing.
    declaration.lookahead_ms = 10;
    auto config = default_config();
    config.declared_frames = 8 * kChunkFrames;
    config.preroll_frames = kPreroll;

    playback::BufferedContentSource source;
    REQUIRE(source.prepare(declaration, config, bounded_producer(8 * kChunkFrames, 0.5f)));
    REQUIRE(source.stats().ring_available_frames >= kPreroll);

    // No pump between prepare and these pulls: the preroll alone serves them.
    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(source.stats().starved_frames == 0);

    // A seek refills the preroll before returning, so the first pull after
    // one is served without a pump as well.
    REQUIRE(source.seek(2 * kChunkFrames, 2));
    REQUIRE(source.stats().ring_available_frames >= kPreroll);
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(source.position() == 3 * kChunkFrames);

    // A producer that concedes every refill cannot meet a declared preroll.
    playback::BufferedContentSource starving;
    REQUIRE_FALSE(starving.prepare(declaration, config, bounded_producer(0, 0.5f)));
    REQUIRE_FALSE(starving.prepared());
}

TEST_CASE("loop wrap production restarts at the loop head epoch", "[playback][production]") {
    auto config = default_config();
    config.declared_frames = 4 * kChunkFrames;
    config.ring_capacity_frames = kChunkFrames;
    config.loop_start_frame = kChunkFrames;
    config.loop_end_frame = 3 * kChunkFrames;

    playback::BufferedContentSource source;
    REQUIRE(source.prepare(
        buffered_declaration(), config,
        [](playback::BufferedContentEpoch, std::uint64_t start_frame,
           audio::BufferView<float> dest, std::uint64_t frames, audio::FrameReaderStopToken) {
            for (std::uint64_t frame = 0; frame < frames; ++frame)
                dest.channel(0)[static_cast<std::size_t>(frame)] =
                    static_cast<float>(start_frame + frame);
            return frames;
        }));

    audio::Buffer<float> block(1, static_cast<std::size_t>(kChunkFrames));
    // Play through the loop end.
    for (std::uint64_t emitted = 0; emitted < 3 * kChunkFrames; emitted += kChunkFrames) {
        REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
        source.pump_background();
    }
    REQUIRE(source.position() == 3 * kChunkFrames);

    REQUIRE(source.loop_wrap(2));
    REQUIRE(source.current_epoch() == 2);
    REQUIRE(source.position() == kChunkFrames);

    // The first pull after the wrap is loop-head content, not the old pass's
    // tail.
    REQUIRE(source.pull(block.view(), kChunkFrames) == kChunkFrames);
    REQUIRE(block.view().channel(0).front() == static_cast<float>(kChunkFrames));

    playback::BufferedContentSource unlooped;
    REQUIRE(unlooped.prepare(buffered_declaration(), default_config(),
                             bounded_producer(kDeclaredFrames, 0.5f)));
    REQUIRE_FALSE(unlooped.loop_wrap(2));
}

TEST_CASE("a seek or loop wrap rejects an invalid epoch or range", "[playback][production]") {
    playback::BufferedContentSource source;
    REQUIRE(source.prepare(buffered_declaration(), default_config(),
                           bounded_producer(kDeclaredFrames, 0.5f)));
    REQUIRE(source.current_epoch() == 1);

    REQUIRE_FALSE(source.seek(0, 0));      // zero is never a valid epoch
    REQUIRE_FALSE(source.seek(0, 1));      // not strictly newer
    REQUIRE_FALSE(source.seek(kDeclaredFrames, 2));  // nothing left to produce
    REQUIRE(source.prepared());
    REQUIRE(source.position() == 0);
    REQUIRE(source.current_epoch() == 1);

    REQUIRE(source.seek(kChunkFrames, 2));
    REQUIRE(source.current_epoch() == 2);
    REQUIRE(source.position() == kChunkFrames);
    REQUIRE_FALSE(source.seek(0, 2));      // a used epoch is not newer
    REQUIRE(source.position() == kChunkFrames);

    // A preroll that cannot fit the remaining content is a validation error,
    // not a shortfall, and leaves the running source untouched.
    auto config = default_config();
    config.preroll_frames = 3 * kChunkFrames;
    playback::BufferedContentSource prerolled;
    REQUIRE(prerolled.prepare(buffered_declaration(), config,
                              bounded_producer(kDeclaredFrames, 0.5f)));
    REQUIRE_FALSE(prerolled.seek(kDeclaredFrames - kChunkFrames, 2));
    REQUIRE(prerolled.prepared());
    REQUIRE(prerolled.position() == 0);
}
