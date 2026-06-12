#pragma once

/// @file stft.hpp
/// Short-Time Fourier Transform abstraction for audio visualization.
/// Accumulates samples from the audio thread and emits windowed FFT frames.

#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>
#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>

namespace pulp::signal {

/// Configuration for an STFT processor.
struct StftConfig {
    int fft_size = 1024;                              // Must be power of 2, 256–8192
    int hop_size = 256;                               // Samples between frames
    WindowFunction::Type window = WindowFunction::Type::hann;
    float window_param = 0.0f;                        // For Kaiser alpha
};

/// A single completed STFT frame: magnitude (linear or dB) and phase.
struct StftFrame {
    std::vector<float> magnitude;   // num_bins values (fft_size/2 + 1)
    std::vector<float> phase;       // num_bins values
    int num_bins = 0;
};

/// Audio-thread-safe STFT processor.
///
/// Feed samples via push_samples(). When enough samples accumulate for a
/// hop, a complete windowed FFT frame is computed. The latest frame is
/// available via latest_frame().
///
/// Intended usage:
///   - Audio thread calls push_samples() from the process callback.
///   - UI thread reads latest_frame() for visualization.
///   - For lock-free publication, wrap the output in a TripleBuffer externally
///     (VisualizationBridge does this).
///
/// The STFT itself is NOT thread-safe for concurrent push/read. Thread
/// safety is provided by VisualizationBridge wrapping it with TripleBuffer.
class Stft {
public:
    Stft() = default;

    explicit Stft(const StftConfig& config) { configure(config); }

    /// RT contract: configure() allocates FFT/window/ring/frame storage and is
    /// not audio-thread safe. After configure(), push_samples(), latest_frame(),
    /// frame_ready(), accessors, to_db(), and reset() are allocation-free.
    /// latest_magnitude_db() returns a vector copy and is not RT-safe.
    void configure(const StftConfig& config) {
        assert(config.fft_size >= 256 && config.fft_size <= 8192);
        assert((config.fft_size & (config.fft_size - 1)) == 0); // power of 2

        config_ = config;
        fft_ = Fft(config.fft_size);
        window_ = WindowFunction::generate(config.fft_size, config.window, config.window_param);
        num_bins_ = config.fft_size / 2 + 1;

        // Ring buffer for sample accumulation
        ring_.resize(config.fft_size, 0.0f);
        ring_pos_ = 0;
        hop_counter_ = 0;
        samples_fed_ = 0;

        // Working buffers
        windowed_.resize(config.fft_size, 0.0f);
        freq_.resize(config.fft_size);

        frame_.magnitude.resize(num_bins_, 0.0f);
        frame_.phase.resize(num_bins_, 0.0f);
        frame_.num_bins = num_bins_;
        frame_ready_ = false;
    }

    /// Feed audio samples. Call from the audio thread.
    /// Returns true if a new frame was computed during this call.
    bool push_samples(const float* samples, int count) {
        bool computed = false;
        for (int i = 0; i < count; ++i) {
            ring_[ring_pos_] = samples[i];
            ring_pos_ = (ring_pos_ + 1) % config_.fft_size;
            ++samples_fed_;
            ++hop_counter_;

            if (hop_counter_ >= config_.hop_size && samples_fed_ >= config_.fft_size) {
                compute_frame();
                hop_counter_ = 0;
                computed = true;
            }
        }
        return computed;
    }

    /// Access the latest computed frame.
    const StftFrame& latest_frame() const { return frame_; }

    /// Whether at least one frame has been computed.
    bool frame_ready() const { return frame_ready_; }

    /// Number of frequency bins (fft_size/2 + 1).
    int num_bins() const { return num_bins_; }

    /// Current FFT size.
    int fft_size() const { return config_.fft_size; }

    /// Current hop size.
    int hop_size() const { return config_.hop_size; }

    /// Convert linear magnitudes to dB in-place.
    static void to_db(float* magnitudes, int count, float floor_db = -120.0f) {
        float floor_linear = std::pow(10.0f, floor_db / 20.0f);
        for (int i = 0; i < count; ++i) {
            magnitudes[i] = 20.0f * std::log10(std::max(magnitudes[i], floor_linear));
        }
    }

    /// Get the latest frame magnitudes in dB. Not RT-safe; returns a vector.
    std::vector<float> latest_magnitude_db(float floor_db = -120.0f) const {
        auto db = frame_.magnitude;
        to_db(db.data(), static_cast<int>(db.size()), floor_db);
        return db;
    }

    void reset() {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        ring_pos_ = 0;
        hop_counter_ = 0;
        samples_fed_ = 0;
        frame_ready_ = false;
        std::fill(frame_.magnitude.begin(), frame_.magnitude.end(), 0.0f);
        std::fill(frame_.phase.begin(), frame_.phase.end(), 0.0f);
    }

private:
    void compute_frame() {
        // Copy ring buffer contents in order, starting from the oldest sample
        int start = ring_pos_; // ring_pos_ points to the next write position = oldest
        for (int i = 0; i < config_.fft_size; ++i) {
            windowed_[i] = ring_[(start + i) % config_.fft_size];
        }

        // Apply window
        WindowFunction::apply(windowed_.data(), window_);

        // Forward FFT
        fft_.forward_real(windowed_.data(), freq_.data());

        // Extract magnitude and phase for positive frequencies
        for (int i = 0; i < num_bins_; ++i) {
            frame_.magnitude[i] = std::abs(freq_[i]);
            frame_.phase[i] = std::arg(freq_[i]);
        }

        frame_ready_ = true;
    }

    StftConfig config_;
    Fft fft_{1024};
    std::vector<float> window_;
    int num_bins_ = 0;

    // Ring buffer for sample accumulation
    std::vector<float> ring_;
    int ring_pos_ = 0;
    int hop_counter_ = 0;
    int samples_fed_ = 0;

    // Working buffers (reused to avoid allocation)
    std::vector<float> windowed_;
    std::vector<std::complex<float>> freq_;

    // Latest computed frame
    StftFrame frame_;
    bool frame_ready_ = false;
};

} // namespace pulp::signal
