// test_streaming_sample_source.cpp — coverage for the streaming sample
// playback primitive (preload window + background-filled ring + sequential
// RT-safe pull). Exercises the resident fast path, deterministic manual-pump
// streaming, the real background reader thread, and underrun behavior.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/audio/streaming_sample_source.hpp>
#include <pulp/audio/streaming_sample_source_file.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using pulp::audio::Buffer;
using pulp::audio::BufferView;
using pulp::audio::StreamingSampleSource;
using pulp::audio::StreamingSampleSourceConfig;

namespace {

// Deterministic ground-truth sample value for (frame, channel), kept within
// [-1, 1] so it survives 16-bit PCM round-trips in the WAV test without clipping.
float expected_sample(std::uint64_t frame, std::uint32_t ch) {
    return (static_cast<float>(frame % 997) / 997.0f) * 0.5f - 0.25f +
           static_cast<float>(ch) * 0.1f;
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

}  // namespace

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
    auto fr = pulp::audio::make_memory_mapped_frame_reader("/no/such/file.wav");
    REQUIRE_FALSE(fr.valid);
}

TEST_CASE("StreamingSampleSource streams a real WAV file from disk",
          "[audio][streaming][issue-streaming]") {
    // Write a deterministic stereo WAV, then stream it via the memory-mapped
    // FrameReader. This exercises the true zero-copy disk-streaming path.
    const std::uint32_t channels = 2;
    const std::uint64_t total = 12000;
    const std::string path =
        std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
        "/pulp_streaming_src_test.wav";

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
