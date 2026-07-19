// test_streaming_sample_source.cpp — coverage for the streaming sample
// playback primitive (preload window + background-filled ring + sequential
// RT-safe pull). Exercises the resident fast path, deterministic manual-pump
// streaming, the real background reader thread, and underrun behavior.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/format_registry.hpp>
#include <pulp/audio/sample_preload_contract.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::StreamingSampleSource;
using pulp::audio::StreamingSampleSourceConfig;

TEST_CASE("Sampler preload contract includes service, pitch, block, and read guards",
          "[audio][streaming][preload]") {
    const auto result = pulp::audio::evaluate_sample_preload_contract({
        .source_sample_rate = 48000.0,
        .host_sample_rate = 48000.0,
        .maximum_playback_ratio = 4.0,
        .certified_io_latency_seconds = 0.010,
        .scheduler_margin_seconds = 0.002,
        .decoder_latency_seconds = 0.003,
        .maximum_host_block_frames = 512,
        .interpolation_guard_frames = 32,
        .loop_prefetch_guard_frames = 128,
        .configured_preload_frames = 5088,
    });

    REQUIRE(result.valid());
    REQUIRE(result.latency_guard_frames == 2880);
    REQUIRE(result.block_guard_frames == 2048);
    REQUIRE(result.required_preload_frames == 5088);
    REQUIRE(result.sufficient);
}

TEST_CASE("Sampler preload contract rounds source-rate block demand upward",
          "[audio][streaming][preload]") {
    const auto result = pulp::audio::evaluate_sample_preload_contract({
        .source_sample_rate = 44100.0,
        .host_sample_rate = 48000.0,
        .maximum_playback_ratio = 2.0,
        .maximum_host_block_frames = 128,
        .configured_preload_frames = 235,
    });

    REQUIRE(result.valid());
    REQUIRE(result.block_guard_frames == 236);
    REQUIRE(result.required_preload_frames == 236);
    REQUIRE_FALSE(result.sufficient);
}

TEST_CASE("Sampler preload contract rejects invalid and overflowing inputs",
          "[audio][streaming][preload]") {
    using Status = pulp::audio::SamplePreloadContractStatus;

    auto invalid_rate = pulp::audio::evaluate_sample_preload_contract({});
    REQUIRE(invalid_rate.status == Status::InvalidSourceSampleRate);

    auto invalid_latency = pulp::audio::evaluate_sample_preload_contract({
        .source_sample_rate = 48000.0,
        .host_sample_rate = 48000.0,
        .certified_io_latency_seconds = -0.001,
    });
    REQUIRE(invalid_latency.status == Status::InvalidLatency);

    auto overflow = pulp::audio::evaluate_sample_preload_contract({
        .source_sample_rate = 48000.0,
        .host_sample_rate = 48000.0,
        .maximum_playback_ratio = 1.0,
        .maximum_host_block_frames = 1,
        .interpolation_guard_frames = std::numeric_limits<std::uint64_t>::max(),
    });
    REQUIRE(overflow.status == Status::Overflow);
}

namespace {

// Deterministic ground-truth sample value for (frame, channel), kept within
// [-1, 1] so it survives 16-bit PCM round-trips in the WAV test without clipping.
float expected_sample(std::uint64_t frame, std::uint32_t ch) {
    return (static_cast<float>(frame % 997) / 997.0f) * 0.5f - 0.25f +
           static_cast<float>(ch) * 0.1f;
}

std::string temp_wav(const char* suffix) {
    static int counter = 0;
    auto name = "pulp_streaming_src_test_"
              + std::to_string(reinterpret_cast<std::uintptr_t>(&counter))
              + "_" + std::to_string(counter++) + suffix;
    return (std::filesystem::temp_directory_path() / name).string();
}

// A FrameReader that synthesizes expected_sample() for any requested range.
// Counts how many times it is invoked and from which thread is irrelevant —
// it must never be called on the audio (pull) thread, which the tests assert
// indirectly by checking the resident path never reads.
pulp::audio::FrameReader make_synth_reader(std::uint32_t channels,
                                           std::uint64_t total,
                                           std::atomic<int>* calls = nullptr) {
    return [channels, total, calls](std::uint64_t start, BufferView<float> dest,
                                    std::uint64_t frames) -> std::uint64_t {
        if (calls) calls->fetch_add(1);
        const std::uint64_t avail = start >= total ? 0 : total - start;
        const std::uint64_t n = std::min(frames, avail);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            float* out = dest.channel_ptr(ch);
            for (std::uint64_t i = 0; i < n; ++i)
                out[i] = expected_sample(start + i, ch);
        }
        return n;
    };
}

bool approx(float a, float b) { return std::fabs(a - b) < 1e-5f; }

struct TeardownTrace {
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> callback_exited{0};
    std::atomic<std::uint64_t> owner_destroyed{0};
    std::atomic<std::uint64_t> teardown_completed{0};

    std::uint64_t next() noexcept {
        return sequence.fetch_add(1, std::memory_order_seq_cst) + 1;
    }
};

struct BlockingReaderGate {
    std::mutex mutex;
    std::condition_variable condition;
    bool block_tail_read = false;
    bool tail_read_entered = false;
    bool allow_tail_read_to_return = false;
};

struct CapturedReaderOwner {
    std::shared_ptr<TeardownTrace> trace;

    ~CapturedReaderOwner() {
        trace->owner_destroyed.store(trace->next(), std::memory_order_seq_cst);
    }
};

struct TeardownState {
    std::mutex mutex;
    std::condition_variable condition;
    bool started = false;
    bool completed = false;
};

void verify_active_reader_teardown(bool destroy_source) {
    auto trace = std::make_shared<TeardownTrace>();
    auto gate = std::make_shared<BlockingReaderGate>();
    auto owner = std::make_shared<CapturedReaderOwner>();
    owner->trace = trace;
    std::weak_ptr<CapturedReaderOwner> weak_owner = owner;

    auto source = std::make_unique<StreamingSampleSource>();
    StreamingSampleSourceConfig config;
    config.channels = 1;
    config.total_frames = 16;
    config.preload_frames = 4;
    config.ring_capacity_frames = 4;
    config.read_chunk_frames = 4;
    config.start_background_thread = true;

    REQUIRE(source->prepare(
        config,
        [owner, gate](std::uint64_t start,
                      BufferView<float> destination,
                      std::uint64_t frames) -> std::uint64_t {
            if (start >= 8) {
                std::unique_lock lock(gate->mutex);
                if (gate->block_tail_read) {
                    gate->tail_read_entered = true;
                    gate->condition.notify_all();
                    gate->condition.wait(lock, [&gate] {
                        return gate->allow_tail_read_to_return;
                    });
                }
            }

            for (std::uint64_t frame = 0; frame < frames; ++frame) {
                destination.channel_ptr(0)[frame] = expected_sample(start + frame, 0);
            }
            if (start >= 8) {
                owner->trace->callback_exited.store(
                    owner->trace->next(), std::memory_order_seq_cst);
            }
            return frames;
        }));
    owner.reset();

    {
        std::lock_guard lock(gate->mutex);
        gate->block_tail_read = true;
    }
    Buffer<float> output(1, 8);
    const auto produced = source->pull(output.view(), 8);
    {
        std::unique_lock lock(gate->mutex);
        gate->condition.wait(lock, [&gate] { return gate->tail_read_entered; });
    }

    TeardownState teardown;
    auto perform_teardown = [&teardown, &trace](auto operation) mutable {
        {
            std::lock_guard lock(teardown.mutex);
            teardown.started = true;
            teardown.condition.notify_all();
        }
        operation();
        trace->teardown_completed.store(trace->next(), std::memory_order_seq_cst);
        {
            std::lock_guard lock(teardown.mutex);
            teardown.completed = true;
            teardown.condition.notify_all();
        }
    };

    std::thread teardown_thread;
    if (destroy_source) {
        teardown_thread = std::thread(
            [owned = std::move(source), perform_teardown]() mutable {
                perform_teardown([&owned] { owned.reset(); });
            });
    } else {
        auto* source_ptr = source.get();
        teardown_thread = std::thread([source_ptr, perform_teardown]() mutable {
            perform_teardown([source_ptr] { source_ptr->release(); });
        });
    }

    bool completed_while_reader_blocked = false;
    {
        std::unique_lock lock(teardown.mutex);
        teardown.condition.wait(lock, [&teardown] { return teardown.started; });
        completed_while_reader_blocked = teardown.completed;
    }
    const bool owner_alive_while_reader_blocked = !weak_owner.expired();
    const auto callback_exit_before_unblock =
        trace->callback_exited.load(std::memory_order_seq_cst);
    const auto owner_destroyed_before_unblock =
        trace->owner_destroyed.load(std::memory_order_seq_cst);

    {
        std::lock_guard lock(gate->mutex);
        gate->allow_tail_read_to_return = true;
        gate->condition.notify_all();
    }
    teardown_thread.join();

    const auto callback_exit = trace->callback_exited.load(std::memory_order_seq_cst);
    const auto owner_destroyed = trace->owner_destroyed.load(std::memory_order_seq_cst);
    const auto teardown_completed =
        trace->teardown_completed.load(std::memory_order_seq_cst);

    REQUIRE(produced == 8);
    REQUIRE_FALSE(completed_while_reader_blocked);
    REQUIRE(owner_alive_while_reader_blocked);
    REQUIRE(callback_exit_before_unblock == 0);
    REQUIRE(owner_destroyed_before_unblock == 0);
    REQUIRE(callback_exit != 0);
    REQUIRE(owner_destroyed > callback_exit);
    REQUIRE(teardown_completed > owner_destroyed);
    REQUIRE(weak_owner.expired());
}

void verify_cooperative_reader_teardown(bool destroy_source) {
    auto trace = std::make_shared<TeardownTrace>();
    auto gate = std::make_shared<BlockingReaderGate>();
    auto owner = std::make_shared<CapturedReaderOwner>();
    owner->trace = trace;
    std::weak_ptr<CapturedReaderOwner> weak_owner = owner;

    auto source = std::make_unique<StreamingSampleSource>();
    StreamingSampleSourceConfig config;
    config.channels = 1;
    config.total_frames = 16;
    config.preload_frames = 4;
    config.ring_capacity_frames = 4;
    config.read_chunk_frames = 4;
    config.start_background_thread = true;

    pulp::audio::FrameReaderBinding binding;
    binding.stop_mode = pulp::audio::FrameReaderStopMode::Cooperative;
    binding.read = [owner, gate](std::uint64_t start,
                                 BufferView<float> destination,
                                 std::uint64_t frames,
                                 std::stop_token stop_token) -> std::uint64_t {
        if (start >= 8) {
            std::unique_lock lock(gate->mutex);
            if (gate->block_tail_read) {
                gate->tail_read_entered = true;
                gate->condition.notify_all();
                std::stop_callback notify_stop(stop_token, [&gate] {
                    gate->condition.notify_all();
                });
                gate->condition.wait(lock, [&gate, stop_token] {
                    return gate->allow_tail_read_to_return ||
                           stop_token.stop_requested();
                });
            }
        }

        for (std::uint64_t frame = 0; frame < frames; ++frame) {
            destination.channel_ptr(0)[frame] = expected_sample(start + frame, 0);
        }
        if (start >= 8) {
            owner->trace->callback_exited.store(
                owner->trace->next(), std::memory_order_seq_cst);
        }
        return frames;
    };
    REQUIRE(source->prepare(config, std::move(binding)));
    owner.reset();

    {
        std::lock_guard lock(gate->mutex);
        gate->block_tail_read = true;
    }
    Buffer<float> output(1, 8);
    REQUIRE(source->pull(output.view(), 8) == 8);
    {
        std::unique_lock lock(gate->mutex);
        gate->condition.wait(lock, [&gate] { return gate->tail_read_entered; });
    }

    if (destroy_source) {
        std::thread teardown([owned = std::move(source)]() mutable {
            owned.reset();
        });
        teardown.join();
    } else {
        std::thread teardown([source_ptr = source.get()] { source_ptr->release(); });
        teardown.join();
    }

    const auto callback_exit = trace->callback_exited.load(std::memory_order_seq_cst);
    const auto owner_destroyed = trace->owner_destroyed.load(std::memory_order_seq_cst);
    REQUIRE(callback_exit != 0);
    REQUIRE(owner_destroyed > callback_exit);
    REQUIRE(weak_owner.expired());
}

}  // namespace

TEST_CASE("StreamingSampleSource teardown joins an entered FrameReader",
          "[audio][streaming][thread][teardown]") {
    // FrameReader has no cancellation channel, so release cannot interrupt
    // arbitrary reader-owned I/O. Once that I/O returns, teardown must join the
    // worker and destroy its captured state before returning to the caller.
    SECTION("explicit release") {
        verify_active_reader_teardown(false);
    }

    SECTION("destruction") {
        verify_active_reader_teardown(true);
    }
}

TEST_CASE("StreamingSampleSource teardown requests cooperative reader stop",
          "[audio][streaming][thread][teardown][stop]") {
    SECTION("explicit release") {
        verify_cooperative_reader_teardown(false);
    }

    SECTION("destruction") {
        verify_cooperative_reader_teardown(true);
    }
}

TEST_CASE("StreamingSampleSource resident fast path reproduces the source",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 2;
    const std::uint64_t total = 4096;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = total;  // fully resident
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total)));
    REQUIRE(src.fully_resident());

    Buffer<float> out(channels, total);
    const std::uint64_t produced = src.pull(out.view(), total);
    REQUIRE(produced == total);

    for (std::uint64_t f = 0; f < total; ++f) {
        REQUIRE(approx(out.channel(0)[f], expected_sample(f, 0)));
        REQUIRE(approx(out.channel(1)[f], expected_sample(f, 1)));
    }
    // One-shot resident source is finished after the last frame.
    REQUIRE(src.finished());
}

TEST_CASE("StreamingSampleSource resident loop wraps within the loop region",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 1;
    const std::uint64_t total = 1000;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = total;
    cfg.loop = true;
    cfg.loop_start = 100;
    cfg.loop_end = 300;  // 200-frame loop
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total)));

    // Pull more than two loop lengths; never finishes. Standard sampler loop:
    // the attack [0, loop_end) plays once, then [loop_start, loop_end) repeats.
    const std::uint64_t span = 500;
    const std::uint64_t loop_len = 300 - 100;  // loop_end - loop_start
    Buffer<float> out(channels, span);
    REQUIRE(src.pull(out.view(), span) == span);
    REQUIRE_FALSE(src.finished());

    for (std::uint64_t i = 0; i < span; ++i) {
        const std::uint64_t src_frame =
            (i < 300) ? i : 100 + ((i - 300) % loop_len);
        REQUIRE(approx(out.channel(0)[i], expected_sample(src_frame, 0)));
    }
}

TEST_CASE("StreamingSampleSource streams the tail (deterministic manual pump)",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 2;
    const std::uint64_t total = 50000;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = 4096;       // small resident head
    cfg.ring_capacity_frames = 8192;
    cfg.read_chunk_frames = 2048;
    cfg.start_background_thread = false;  // drive refills by hand

    StreamingSampleSource src;
    std::atomic<int> reader_calls{0};
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total, &reader_calls)));
    REQUIRE_FALSE(src.fully_resident());
    // Preload was read synchronously during prepare; the ring was primed too.
    REQUIRE(reader_calls.load() > 0);

    const std::uint64_t block = 512;
    Buffer<float> out(channels, block);
    std::uint64_t pos = 0;
    while (pos < total) {
        // Keep the tail topped up before consuming (deterministic: no thread).
        while (src.pump_background() > 0) { /* fill until full or exhausted */ }

        const std::uint64_t want = std::min(block, total - pos);
        const std::uint64_t got = src.pull(out.view(), want);
        REQUIRE(got == want);
        for (std::uint64_t i = 0; i < want; ++i) {
            REQUIRE(approx(out.channel(0)[i], expected_sample(pos + i, 0)));
            REQUIRE(approx(out.channel(1)[i], expected_sample(pos + i, 1)));
        }
        pos += want;
    }
    REQUIRE(src.finished());
    REQUIRE(src.stats().underrun_frames == 0);
}

TEST_CASE("StreamingSampleSource streams correctly via the background thread",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 1;
    const std::uint64_t total = 80000;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = 2048;
    cfg.ring_capacity_frames = 16384;
    cfg.read_chunk_frames = 4096;
    cfg.start_background_thread = true;  // real reader thread

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total)));

    const std::uint64_t block = 256;
    Buffer<float> out(channels, block);
    std::uint64_t pos = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (pos < total) {
        const std::uint64_t want = std::min(block, total - pos);
        const bool tail_streamed = src.stats().streamed_frames + cfg.preload_frames >= total;
        // Deterministically avoid an underrun by waiting for the reader thread
        // to make the next block available (this is what proves the thread
        // actually fills). Past the preload head we need ring data; while still
        // inside the preload window the data is already resident.
        if (pos >= cfg.preload_frames && !tail_streamed) {
            while (src.stats().ring_available_frames < want &&
                   src.stats().streamed_frames + cfg.preload_frames < total) {
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        const std::uint64_t got = src.pull(out.view(), want);
        REQUIRE(got == want);
        for (std::uint64_t i = 0; i < want; ++i)
            REQUIRE(approx(out.channel(0)[i], expected_sample(pos + i, 0)));
        pos += want;
    }
    REQUIRE(src.finished());
    REQUIRE(src.stats().underrun_frames == 0);
}

TEST_CASE("StreamingSampleSource underruns are silent and counted, never a crash",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 1;
    const std::uint64_t total = 20000;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = 1024;
    cfg.ring_capacity_frames = 1024;
    cfg.read_chunk_frames = 512;
    cfg.start_background_thread = false;  // never refill past the initial prime

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total)));

    // Consume well past the preload + primed ring without pumping. The pull
    // must stay valid: resident/primed frames are correct, the rest zero-fills
    // and is counted as an underrun.
    Buffer<float> out(channels, total);
    src.pull(out.view(), total);

    // First preload frames are always exact.
    for (std::uint64_t f = 0; f < cfg.preload_frames; ++f)
        REQUIRE(approx(out.channel(0)[f], expected_sample(f, 0)));
    REQUIRE(src.stats().underrun_frames > 0);
}

TEST_CASE("make_memory_mapped_frame_reader fails gracefully on a bad path",
          "[audio][streaming][issue-streaming]") {
    auto retained = std::make_shared<pulp::audio::MemoryMappedAudioReader>();
    REQUIRE(retained);
    auto fr = pulp::audio::make_memory_mapped_frame_reader(
        "/no/such/file.wav", false, false, std::numeric_limits<std::uint64_t>::max(), &retained);
    REQUIRE_FALSE(fr.valid);
    REQUIRE_FALSE(fr.supports_ranged_read);
    REQUIRE_FALSE(retained);
}

TEST_CASE("make_memory_mapped_frame_reader tightens a retained snapshot without reopening",
          "[audio][streaming][issue-streaming][snapshot]") {
    const std::string path = temp_wav("_retained.wav");
    pulp::audio::AudioFileData data;
    data.sample_rate = 48000;
    data.channels = {{0.0f, 0.25f, -0.25f, 0.5f}};
    REQUIRE(pulp::audio::write_wav_file(path, data));

    std::shared_ptr<pulp::audio::MemoryMappedAudioReader> retained;
    auto probe = pulp::audio::make_memory_mapped_frame_reader(
        path, false, false, std::numeric_limits<std::uint64_t>::max(), &retained);
    REQUIRE(probe.valid);
    REQUIRE(retained);
    REQUIRE(std::filesystem::remove(path));

    auto strict = pulp::audio::make_retained_memory_mapped_frame_reader(
        retained, true, true);
    REQUIRE(strict.valid);
    REQUIRE(strict.supports_ranged_read);
    REQUIRE(strict.has_content_identity);
    Buffer<float> output(1, 4);
    REQUIRE(strict.reader(0, output.view(), 4) == 4);
    REQUIRE(std::fabs(output.channel(0)[1] - 0.25f) < 2e-4f);
}

TEST_CASE("StreamingSampleSource streams a real WAV file from disk",
          "[audio][streaming][issue-streaming]") {
    // Write a deterministic stereo WAV, then stream it via a retained mapped
    // ranged reader without materializing the complete decoded file.
    const std::uint32_t channels = 2;
    const std::uint64_t total = 12000;
    const std::string path = temp_wav(".wav");

    pulp::audio::AudioFileData data;
    data.sample_rate = 48000;
    data.channels.resize(channels);
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        data.channels[ch].resize(total);
        for (std::uint64_t f = 0; f < total; ++f)
            data.channels[ch][f] = expected_sample(f, ch);
    }
    REQUIRE(pulp::audio::write_wav_file(path, data));

    auto fr = pulp::audio::make_memory_mapped_frame_reader(path);
    REQUIRE(fr.valid);
    REQUIRE(fr.channels == channels);
    REQUIRE(fr.total_frames == total);
    REQUIRE(fr.sample_rate == 48000);
    REQUIRE(fr.supports_ranged_read);
    REQUIRE(fr.binding.stop_mode == pulp::audio::FrameReaderStopMode::Cooperative);

    Buffer<float> ranged(channels, 37);
    REQUIRE(fr.reader(7891, ranged.view(), 37) == 37);
    for (std::uint64_t i = 0; i < 37; ++i) {
        REQUIRE(std::fabs(ranged.channel(0)[i] - expected_sample(7891 + i, 0)) < 2e-4f);
        REQUIRE(std::fabs(ranged.channel(1)[i] - expected_sample(7891 + i, 1)) < 2e-4f);
    }

    StreamingSampleSourceConfig cfg;
    cfg.channels = fr.channels;
    cfg.total_frames = fr.total_frames;
    cfg.sample_rate = fr.sample_rate;
    cfg.preload_frames = 2048;
    cfg.ring_capacity_frames = 8192;
    cfg.read_chunk_frames = 2048;
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, fr.reader));

    const std::uint64_t block = 512;
    Buffer<float> out(channels, block);
    std::uint64_t pos = 0;
    while (pos < total) {
        while (src.pump_background() > 0) { /* keep tail full */ }
        const std::uint64_t want = std::min(block, total - pos);
        REQUIRE(src.pull(out.view(), want) == want);
        for (std::uint64_t i = 0; i < want; ++i) {
            // WAV is 16-bit PCM by default; allow quantization tolerance.
            REQUIRE(std::fabs(out.channel(0)[i] - expected_sample(pos + i, 0)) < 2e-4f);
            REQUIRE(std::fabs(out.channel(1)[i] - expected_sample(pos + i, 1)) < 2e-4f);
        }
        pos += want;
    }
    REQUIRE(src.finished());
    std::remove(path.c_str());
}

TEST_CASE("File frame reader range-reads uncompressed AIFF",
          "[audio][streaming][file-reader]") {
    const std::string path = temp_wav(".aiff");
    pulp::audio::AudioFileData data;
    data.sample_rate = 44100;
    data.channels = {{0.125f, -0.25f, 0.5f, -0.75f}};
    REQUIRE(pulp::audio::FormatRegistry::instance().write(path, data));

    auto reader = pulp::audio::make_memory_mapped_frame_reader(path);
    REQUIRE(reader.valid);
    REQUIRE(reader.supports_ranged_read);
    REQUIRE(reader.binding.stop_mode ==
            pulp::audio::FrameReaderStopMode::Cooperative);
    REQUIRE(reader.channels == 1);
    REQUIRE(reader.total_frames == 4);

    Buffer<float> out(1, 2);
    REQUIRE(reader.reader(1, out.view(), 2) == 2);
    REQUIRE(std::fabs(out.channel(0)[0] + 0.25f) < 2e-4f);
    REQUIRE(std::fabs(out.channel(0)[1] - 0.5f) < 2e-4f);
    REQUIRE(reader.reader(0, out.view(), 20) == 2);
    std::remove(path.c_str());
}

TEST_CASE("StreamingSampleSource stays positionally aligned across underruns",
          "[audio][streaming][issue-streaming]") {
    // Regression: on a mid-stream ring underrun, pull() must advance the play
    // cursor only by frames actually read, so late-arriving frames still play in
    // order (no time-shift, no truncated tail). Deliberately under-pump to force
    // underruns, then verify every produced frame matches ground truth at its
    // source index and the whole source is eventually delivered.
    const std::uint32_t channels = 1;
    const std::uint64_t total = 30000;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = total;
    cfg.preload_frames = 1024;
    cfg.ring_capacity_frames = 4096;
    cfg.read_chunk_frames = 1024;
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, total)));

    const std::uint64_t block = 700;  // not a divisor of chunk/ring — exercises edges
    Buffer<float> out(channels, block);
    std::uint64_t consumed = 0;
    int iter = 0;
    const int max_iters = 100000;
    while (consumed < total && iter++ < max_iters) {
        // Under-pump: only a single chunk every 3rd iteration → frequent misses.
        if (iter % 3 == 0) src.pump_background();

        const std::uint64_t got = src.pull(out.view(), block);
        for (std::uint64_t i = 0; i < got; ++i) {
            REQUIRE(approx(out.channel(0)[i], expected_sample(consumed + i, 0)));
        }
        consumed += got;  // advance only by produced — must stay aligned

        if (got == 0 && !src.finished()) {
            while (src.pump_background() > 0) { /* recover */ }
        }
    }
    REQUIRE(consumed == total);   // no frames lost/truncated
    REQUIRE(src.finished());
    REQUIRE(src.stats().underrun_frames > 0);  // we really did underrun
}

TEST_CASE("StreamingSampleSource honors a short reader return",
          "[audio][streaming][issue-streaming]") {
    const std::uint32_t channels = 1;
    const std::uint64_t declared = 10000;
    const std::uint64_t realized = 3000;  // reader yields fewer than declared

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = declared;
    cfg.preload_frames = declared;  // would be resident if reader had the data
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, realized)));
    // Realized length becomes authoritative.
    REQUIRE(src.total_frames() == realized);

    Buffer<float> out(channels, declared);
    const std::uint64_t produced = src.pull(out.view(), declared);
    REQUIRE(produced == realized);
    REQUIRE(src.finished());
}

TEST_CASE("StreamingSampleSource terminates when a streamed source ends early",
          "[audio][streaming][issue-streaming]") {
    // Regression: a NON-resident source whose reader returns 0 mid-tail (a decode
    // error or short stream before the declared total) must still finish once its
    // realized frames have played out — not stall forever zero-filling while
    // finished() stays false (which would leak a sampler voice gated on
    // finished()). The pre-fix code bumped the reader cursor to total_frames_ but
    // left the audio thread comparing play_pos_ against the optimistic declared
    // total, so finished() never fired.
    const std::uint32_t channels = 1;
    const std::uint64_t declared = 50000;
    const std::uint64_t realized = 20000;  // reader yields this, then returns 0

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = declared;
    cfg.preload_frames = 4096;            // non-resident: forces the streamed tail
    cfg.ring_capacity_frames = 8192;
    cfg.read_chunk_frames = 2048;
    cfg.start_background_thread = false;  // deterministic manual pump

    StreamingSampleSource src;
    // The reader's own length is `realized`, so it returns 0 for start >= realized.
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, realized)));
    REQUIRE_FALSE(src.fully_resident());

    const std::uint64_t block = 512;
    Buffer<float> out(channels, block);
    std::uint64_t consumed = 0;
    int iter = 0;
    const int max_iters = 100000;
    while (!src.finished() && iter++ < max_iters) {
        while (src.pump_background() > 0) { /* keep the tail topped up */ }
        const std::uint64_t got = src.pull(out.view(), block);
        for (std::uint64_t i = 0; i < got; ++i)
            REQUIRE(approx(out.channel(0)[i], expected_sample(consumed + i, 0)));
        consumed += got;
        if (got == 0) break;  // nothing more will ever come — finished() must be set
    }
    REQUIRE(src.finished());          // the bug: never became true before the fix
    REQUIRE(consumed == realized);    // exactly the realized frames, in order
    REQUIRE(src.stats().read_errors > 0);
}

TEST_CASE("StreamingSampleSource zero-fills surplus destination channels",
          "[audio][streaming][issue-streaming]") {
    // A mono source pulled into a stereo destination must leave the extra channel
    // silent (matching the streamed ring path), not stale/garbage. Regression for
    // the resident fast path, which previously copied only the source channels and
    // left wider destination channels untouched.
    const std::uint32_t src_ch = 1;
    const std::uint64_t total = 2048;

    StreamingSampleSourceConfig cfg;
    cfg.channels = src_ch;
    cfg.total_frames = total;
    cfg.preload_frames = total;  // resident fast path
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    REQUIRE(src.prepare(cfg, make_synth_reader(src_ch, total)));
    REQUIRE(src.fully_resident());

    // Stereo destination, pre-dirtied on both channels.
    Buffer<float> out(2, total);
    for (std::uint64_t f = 0; f < total; ++f) {
        out.channel(0)[f] = 123.0f;
        out.channel(1)[f] = 123.0f;
    }
    REQUIRE(src.pull(out.view(), total) == total);
    for (std::uint64_t f = 0; f < total; ++f) {
        REQUIRE(approx(out.channel(0)[f], expected_sample(f, 0)));  // source channel
        REQUIRE(out.channel(1)[f] == 0.0f);                         // zeroed surplus
    }
}

TEST_CASE("StreamingSampleSource finishes a looping source that yields no frames",
          "[audio][streaming][issue-streaming]") {
    // Regression: a LOOPING voice whose preload read returns 0 frames (empty or
    // failed source) shrinks total_frames_ to 0 and becomes resident. Without the
    // zero-length guard, the resident-loop path would emit silence forever and
    // finished() would never fire — leaking the voice. A zero-length source must
    // report finished immediately, loop flag notwithstanding.
    const std::uint32_t channels = 1;

    StreamingSampleSourceConfig cfg;
    cfg.channels = channels;
    cfg.total_frames = 8000;
    cfg.preload_frames = 8000;            // would be fully resident
    cfg.loop = true;                       // looping
    cfg.start_background_thread = false;

    StreamingSampleSource src;
    // Reader's realized length is 0 → preload read yields nothing.
    REQUIRE(src.prepare(cfg, make_synth_reader(channels, 0)));
    REQUIRE(src.total_frames() == 0);
    REQUIRE(src.finished());  // the bug: looped silence forever before the fix

    Buffer<float> out(channels, 256);
    REQUIRE(src.pull(out.view(), 256) == 0);  // nothing to emit
    REQUIRE(src.finished());                  // still finished after a pull
}
