#pragma once

/// @file load_measurer.hpp
/// Real-time CPU load measurement for audio callbacks.

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::audio {

/// Measures the CPU load of an audio processing callback.
///
/// Call begin() at the start and end() at the end of your process() method.
/// The load is the fraction of available time consumed by processing.
///
/// @code
/// void MyPlugin::process(BufferView& buffer) {
///     load_measurer_.begin(buffer.num_frames(), sample_rate_);
///     // ... DSP work ...
///     load_measurer_.end();
///     float cpu = load_measurer_.load(); // 0.0 to 1.0+
/// }
/// @endcode
class AudioProcessLoadMeasurer {
public:
    /// Call at the start of the audio callback.
    /// @param num_frames Number of samples in this buffer.
    /// @param sample_rate Current sample rate in Hz.
    void begin(int num_frames, float sample_rate) {
        start_time_ = clock::now();

        if (num_frames <= 0 || !(sample_rate > 0.0f)) {
            available_ns_ = 0;
            return;
        }

        const double available_ns =
            static_cast<double>(num_frames) / static_cast<double>(sample_rate) * 1e9;

        if (!std::isfinite(available_ns) || available_ns <= 0.0 ||
            available_ns > static_cast<double>(std::numeric_limits<int64_t>::max())) {
            available_ns_ = 0;
            return;
        }

        available_ns_ = static_cast<int64_t>(available_ns);
    }

    /// Call at the end of the audio callback.
    void end() {
        auto elapsed = clock::now() - start_time_;
        int64_t elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();

        if (available_ns_ > 0) {
            float raw = static_cast<float>(elapsed_ns) / static_cast<float>(available_ns_);
            // Smooth with exponential moving average
            load_ = load_ + smoothing_ * (raw - load_);
            peak_load_ = std::max(peak_load_, raw);
        }
    }

    /// Current smoothed CPU load (0.0 = idle, 1.0 = full buffer, >1.0 = overrun).
    float load() const { return load_; }

    /// Peak load since last reset.
    float peak_load() const { return peak_load_; }

    /// Reset peak load tracker.
    void reset_peak() { peak_load_ = 0.0f; }

    /// Reset all state.
    void reset() {
        load_ = 0.0f;
        peak_load_ = 0.0f;
        available_ns_ = 0;
        start_time_ = {};
    }

    /// Set smoothing factor (0 = no smoothing, 1 = no averaging).
    /// Default is 0.1 (responsive but stable).
    void set_smoothing(float alpha) {
        smoothing_ = std::clamp(alpha, 0.0f, 1.0f);
    }

private:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    time_point start_time_{};
    int64_t available_ns_ = 0;
    float load_ = 0.0f;
    float peak_load_ = 0.0f;
    float smoothing_ = 0.1f;
};

} // namespace pulp::audio
