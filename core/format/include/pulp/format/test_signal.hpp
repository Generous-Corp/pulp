#pragma once

#include <pulp/audio/audio_file.hpp>
#include <pulp/runtime/triple_buffer.hpp>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace pulp::format {

enum class TestSignalType { none, sine, file, noise };

struct TestSignalConfig {
    TestSignalType type = TestSignalType::none;
    float sine_frequency_hz = 440.0f;
    float sine_amplitude = 0.5f;  // linear, 0–1
};

/// Generates test audio for the standalone host: sine tone or file playback.
/// Thread-safe: config changes from UI thread, fill() called on audio thread.
class TestSignalSource {
public:
    TestSignalSource() = default;

    void set_sample_rate(double sr) { sample_rate_ = sr; }

    // ── Config (UI thread) ──────────────────────────────────────────────
    void set_config(const TestSignalConfig& cfg);
    TestSignalConfig config() const;
    bool is_active() const { return active_.load(std::memory_order_relaxed); }

    // ── File playback (UI thread) ───────────────────────────────────────
    bool load_file(const std::string& path);
    void unload_file();
    bool has_file() const { return file_data_ != nullptr; }
    const audio::AudioFileData* file_data() const { return file_data_.get(); }

    void play();
    void stop();
    void set_loop(bool loop) { file_loop_.store(loop, std::memory_order_relaxed); }
    bool is_playing() const { return file_playing_.load(std::memory_order_relaxed); }
    bool is_looping() const { return file_loop_.load(std::memory_order_relaxed); }
    int64_t file_position() const { return file_position_.load(std::memory_order_relaxed); }

    // ── Audio thread ────────────────────────────────────────────────────
    /// Fill output buffers with test signal. Called from audio callback.
    /// output is an array of channel pointers (non-interleaved).
    void fill(float* const* output, int num_channels, int num_samples);

    void reset();

private:
    double sample_rate_ = 48000.0;
    std::atomic<bool> active_{false};

    // Config sync: UI writes via TripleBuffer, audio thread reads latest
    TestSignalConfig config_;  // Audio thread's working copy
    runtime::TripleBuffer<TestSignalConfig> config_buf_;

    // Sine state (audio thread only)
    double sine_phase_ = 0.0;

    // White-noise generator state (audio thread only). Seeded per channel so the
    // two output channels are decorrelated.
    std::array<std::uint32_t, 2> noise_state_ = {0x9e3779b9u, 0x1b56c4e9u};

    // File playback
    std::unique_ptr<audio::AudioFileData> file_data_;
    std::atomic<int64_t> file_position_{0};
    std::atomic<bool> file_playing_{false};
    std::atomic<bool> file_loop_{false};
};

} // namespace pulp::format
