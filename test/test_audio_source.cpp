#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/source.hpp>
#include <pulp/audio/source_player.hpp>
#include <pulp/audio/transport_source.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

// ────────────────────────────────────────────────────────────────────────
// macOS plan item 1.8 — AudioSource hierarchy
//
// Verifies the abstract bases + AudioTransportSource transport
// control + AudioSourcePlayer host-callback bridge.
// AudioFormatReaderSource has its own real-file coverage path via
// the existing MemoryMappedAudioReader tests; here we use a fake
// constant-tone PositionableAudioSource that doesn't need a real
// audio file.
// ────────────────────────────────────────────────────────────────────────

namespace {

/// Toy PositionableAudioSource — produces a constant `value` sample
/// for `total_length` frames. Records prepare/release lifecycle for
/// the tests.
class ConstantSource : public PositionableAudioSource {
public:
    ConstantSource(float value, uint64_t total_length, std::size_t channels)
        : value_(value), total_length_(total_length), channels_(channels) {}

    void prepare_to_play(int samples_per_block, double sample_rate) override {
        last_block_size = samples_per_block;
        last_sample_rate = sample_rate;
        ++prepare_calls;
    }
    void release_resources() override { ++release_calls; }

    void get_next_audio_block(BufferView<float> out,
                                int start_sample,
                                int num_samples) override {
        ++block_calls;
        for (std::size_t ch = 0; ch < std::min(out.num_channels(), channels_); ++ch) {
            float* ptr = out.channel_ptr(ch) + start_sample;
            for (int i = 0; i < num_samples; ++i) {
                if (read_pos_ >= total_length_) {
                    if (looping_) read_pos_ = 0;
                    else { ptr[i] = 0.0f; continue; }
                }
                ptr[i] = value_;
                ++read_pos_; // advance per-sample (per-channel is fine here
                             // because all channels read the same constant)
            }
        }
        // Walk back the per-sample advance for the extra channels —
        // we only want read_pos_ to advance by num_samples once.
        // Simpler: rewind, then advance once.
        read_pos_ -= static_cast<uint64_t>(num_samples)
                     * std::min(out.num_channels(), channels_);
        // Advance exactly num_samples worth.
        const auto safe_advance = std::min<uint64_t>(
            static_cast<uint64_t>(num_samples), total_length_ - read_pos_);
        read_pos_ += safe_advance;
        if (looping_ && read_pos_ >= total_length_) read_pos_ = 0;
    }

    void set_next_read_position(uint64_t pos) override {
        read_pos_ = std::min(pos, total_length_);
    }
    uint64_t get_next_read_position() const override { return read_pos_; }
    uint64_t get_total_length() const override { return total_length_; }
    bool is_looping() const override { return looping_; }
    void set_looping(bool should_loop) override { looping_ = should_loop; }

    int prepare_calls = 0;
    int release_calls = 0;
    int block_calls = 0;
    int last_block_size = 0;
    double last_sample_rate = 0.0;

private:
    float value_;
    uint64_t total_length_;
    std::size_t channels_;
    uint64_t read_pos_ = 0;
    bool looping_ = false;
};

} // namespace

TEST_CASE("ConstantSource sanity: emits constant + advances position",
          "[audio][source]") {
    ConstantSource src(0.5f, /*total=*/100, /*channels=*/2);
    Buffer<float> buf(2, 16);
    src.get_next_audio_block(buf.view(), 0, 16);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 16; ++i) REQUIRE_THAT(buf.channel(ch)[i], WithinAbs(0.5f, 1e-6f));
    }
    REQUIRE(src.get_next_read_position() == 16);
}

TEST_CASE("PositionableAudioSource seek + looping defaults", "[audio][source]") {
    ConstantSource src(1.0f, 50, 1);
    REQUIRE(src.get_next_read_position() == 0);
    REQUIRE(src.get_total_length() == 50);
    REQUIRE_FALSE(src.is_looping());
    src.set_next_read_position(25);
    REQUIRE(src.get_next_read_position() == 25);
    src.set_next_read_position(9999); // clamped
    REQUIRE(src.get_next_read_position() == 50);
    src.set_looping(true);
    REQUIRE(src.is_looping());
}

TEST_CASE("AudioTransportSource: stopped → silence", "[audio][transport]") {
    ConstantSource src(0.5f, 1000, 2);
    AudioTransportSource t;
    t.set_source(&src);
    t.prepare_to_play(128, 48000.0);
    REQUIRE_FALSE(t.is_playing());
    Buffer<float> buf(2, 128);
    // Pre-fill with garbage so we can verify it's overwritten with zeros.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 128; ++i) buf.channel(ch)[i] = 0.99f;
    }
    t.get_next_audio_block(buf.view(), 0, 128);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 128; ++i) REQUIRE_THAT(buf.channel(ch)[i], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("AudioTransportSource: start → emits source audio scaled by gain",
          "[audio][transport]") {
    ConstantSource src(0.5f, 1000, 2);
    AudioTransportSource t;
    t.set_source(&src);
    t.set_gain(0.5f);
    t.prepare_to_play(64, 48000.0); // snaps current_gain → target (0.5)
    t.start();
    REQUIRE(t.is_playing());
    Buffer<float> buf(2, 64);
    t.get_next_audio_block(buf.view(), 0, 64);
    // After prepare_to_play snaps current to target, no ramp needed.
    // Source value 0.5 * gain 0.5 = 0.25 throughout.
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 64; ++i) REQUIRE_THAT(buf.channel(ch)[i], WithinAbs(0.25f, 1e-6f));
    }
}

TEST_CASE("AudioTransportSource: gain change is sample-accurate ramp",
          "[audio][transport][click-free]") {
    ConstantSource src(1.0f, 10000, 1);
    AudioTransportSource t;
    t.set_source(&src);
    t.prepare_to_play(128, 48000.0);
    t.set_gain(1.0f);
    t.start();
    // Drain one block to settle at gain 1.0.
    Buffer<float> warmup(1, 128);
    t.get_next_audio_block(warmup.view(), 0, 128);

    // Now change target to 0.0 — should ramp across the block.
    t.set_gain(0.0f);
    Buffer<float> ramp(1, 128);
    t.get_next_audio_block(ramp.view(), 0, 128);

    // Ramp uses *= then += step, so sample[0] takes the starting gain
    // (1.0) and sample[1] takes 1.0 + step ≈ 0.992. Last sample takes
    // current_gain + step*127 = 1.0 + (-1/128)*127 ≈ 0.0078.
    REQUIRE_THAT(ramp.channel(0)[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(ramp.channel(0)[127], WithinAbs(1.0f / 128.0f, 1e-3f));
    // Adjacent samples differ by ≈ step (click-free).
    for (int i = 1; i < 128; ++i) {
        const float delta = std::fabs(ramp.channel(0)[i] - ramp.channel(0)[i - 1]);
        REQUIRE(delta < 0.05f); // step magnitude bound
    }
}

TEST_CASE("AudioTransportSource: prepare_to_play forwards to source",
          "[audio][transport]") {
    ConstantSource src(1.0f, 100, 1);
    AudioTransportSource t;
    t.set_source(&src);
    t.prepare_to_play(256, 44100.0);
    REQUIRE(src.prepare_calls == 1);
    REQUIRE(src.last_block_size == 256);
    REQUIRE(src.last_sample_rate == 44100.0);
    t.release_resources();
    REQUIRE(src.release_calls == 1);
}

TEST_CASE("AudioTransportSource: position pass-through", "[audio][transport]") {
    ConstantSource src(1.0f, 1000, 1);
    AudioTransportSource t;
    t.set_source(&src);
    REQUIRE(t.get_total_length() == 1000);
    t.set_next_read_position(500);
    REQUIRE(t.get_next_read_position() == 500);
    t.set_looping(true);
    REQUIRE(t.is_looping());
}

TEST_CASE("AudioTransportSource: detached state returns zero / nulls",
          "[audio][transport]") {
    AudioTransportSource t;
    REQUIRE(t.source() == nullptr);
    REQUIRE(t.get_total_length() == 0);
    REQUIRE(t.get_next_read_position() == 0);
    REQUIRE_FALSE(t.is_looping());
    REQUIRE_FALSE(t.is_playing());
    // start() with no source must remain stopped.
    t.start();
    REQUIRE_FALSE(t.is_playing());

    Buffer<float> buf(1, 64);
    for (int i = 0; i < 64; ++i) buf.channel(0)[i] = 0.5f;
    t.get_next_audio_block(buf.view(), 0, 64);
    for (int i = 0; i < 64; ++i) REQUIRE_THAT(buf.channel(0)[i], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("AudioSourcePlayer: routes source into output, prepares once",
          "[audio][source-player]") {
    ConstantSource src(0.25f, 10000, 2);
    AudioSourcePlayer player;
    player.set_source(&src);
    Buffer<float> buf(2, 128);
    player.audio_callback(buf.view(), 128, 48000.0);
    REQUIRE(src.prepare_calls == 1);
    REQUIRE(src.last_block_size == 128);
    REQUIRE(src.last_sample_rate == 48000.0);
    for (std::size_t ch = 0; ch < 2; ++ch) {
        for (int i = 0; i < 128; ++i) REQUIRE_THAT(buf.channel(ch)[i], WithinAbs(0.25f, 1e-6f));
    }
    // Same params again — no re-prepare.
    player.audio_callback(buf.view(), 128, 48000.0);
    REQUIRE(src.prepare_calls == 1);
}

TEST_CASE("AudioSourcePlayer: re-prepares on sample-rate or block-size change",
          "[audio][source-player]") {
    ConstantSource src(1.0f, 10000, 1);
    AudioSourcePlayer player;
    player.set_source(&src);
    Buffer<float> b1(1, 128);
    player.audio_callback(b1.view(), 128, 48000.0);
    REQUIRE(src.prepare_calls == 1);
    // Sample-rate change → re-prepare.
    player.audio_callback(b1.view(), 128, 96000.0);
    REQUIRE(src.prepare_calls == 2);
    // Larger block-size → re-prepare.
    Buffer<float> b2(1, 256);
    player.audio_callback(b2.view(), 256, 96000.0);
    REQUIRE(src.prepare_calls == 3);
    // Smaller block-size → no re-prepare (last was big enough).
    player.audio_callback(b1.view(), 128, 96000.0);
    REQUIRE(src.prepare_calls == 3);
}

TEST_CASE("AudioSourcePlayer: detached source emits silence",
          "[audio][source-player]") {
    AudioSourcePlayer player;
    Buffer<float> buf(1, 64);
    for (int i = 0; i < 64; ++i) buf.channel(0)[i] = 0.5f;
    player.audio_callback(buf.view(), 64, 48000.0);
    for (int i = 0; i < 64; ++i) REQUIRE_THAT(buf.channel(0)[i], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("AudioSourcePlayer: gain scales output", "[audio][source-player]") {
    ConstantSource src(0.5f, 10000, 1);
    AudioSourcePlayer player;
    player.set_source(&src);
    player.set_gain(2.0f);
    Buffer<float> buf(1, 32);
    player.audio_callback(buf.view(), 32, 48000.0);
    for (int i = 0; i < 32; ++i) REQUIRE_THAT(buf.channel(0)[i], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("AudioSourcePlayer: set_source(nullptr) releases the prior source",
          "[audio][source-player]") {
    ConstantSource src(1.0f, 100, 1);
    AudioSourcePlayer player;
    player.set_source(&src);
    Buffer<float> buf(1, 32);
    player.audio_callback(buf.view(), 32, 48000.0);
    REQUIRE(src.prepare_calls == 1);
    REQUIRE(src.release_calls == 0);
    player.set_source(nullptr);
    REQUIRE(src.release_calls == 1);
}

TEST_CASE("AudioSourcePlayer: release() works without nulling source",
          "[audio][source-player]") {
    ConstantSource src(1.0f, 100, 1);
    AudioSourcePlayer player;
    player.set_source(&src);
    Buffer<float> buf(1, 16);
    player.audio_callback(buf.view(), 16, 48000.0);
    player.release();
    REQUIRE(src.release_calls == 1);
    // Next callback re-prepares.
    player.audio_callback(buf.view(), 16, 48000.0);
    REQUIRE(src.prepare_calls == 2);
}

TEST_CASE("AudioTransportSource: continues ramp toward target even when stopped",
          "[audio][transport]") {
    // If gain is changed while stopped, the transport caches the new
    // target as `current` so resuming doesn't pop.
    ConstantSource src(1.0f, 10000, 1);
    AudioTransportSource t;
    t.set_source(&src);
    t.prepare_to_play(64, 48000.0);
    t.set_gain(0.0f); // target while stopped
    Buffer<float> silent(1, 64);
    t.get_next_audio_block(silent.view(), 0, 64); // produces silence
    REQUIRE_THAT(t.current_gain(), WithinAbs(0.0f, 1e-6f));
    // Now start; gain stays at 0 (target unchanged).
    t.start();
    Buffer<float> still_silent(1, 64);
    t.get_next_audio_block(still_silent.view(), 0, 64);
    for (int i = 0; i < 64; ++i) {
        REQUIRE_THAT(still_silent.channel(0)[i], WithinAbs(0.0f, 1e-6f));
    }
}
