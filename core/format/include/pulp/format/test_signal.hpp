#pragma once

#include <pulp/audio/audio_file.hpp>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace pulp::format {

enum class TestSignalType { none, sine, file };

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

    // Config sync: UI writes pending, audio thread swaps on next fill()
    TestSignalConfig config_;
    TestSignalConfig pending_config_;
    std::atomic<bool> config_dirty_{false};
    std::mutex config_mutex_;

    // Sine state (audio thread only)
    double sine_phase_ = 0.0;

    // File playback
    std::unique_ptr<audio::AudioFileData> file_data_;
    std::atomic<int64_t> file_position_{0};
    std::atomic<bool> file_playing_{false};
    std::atomic<bool> file_loop_{false};
};

} // namespace pulp::format
