#pragma once

#include <pulp/runtime/triple_buffer.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>

namespace pulp::view {

// ── Metering ─────────────────────────────────────────────────────────────────

// Per-channel level data sent from audio thread to UI
struct MeterData {
    static constexpr int max_channels = 8;

    float peak[max_channels] = {};     // Peak level (linear, 0-1+)
    float rms[max_channels] = {};      // RMS level (linear, 0-1+)
    int num_channels = 0;
};

// Ballistics processor for smooth meter display (runs on UI thread)
struct MeterBallistics {
    float display_peak = 0;
    float display_rms = 0;
    float held_peak = 0;
    float hold_counter = 0;

    // Update with new audio data. Call once per UI frame.
    // attack/release in seconds, hold_time in seconds, dt = frame time
    void update(float new_peak, float new_rms, float dt,
                float attack = 0.001f, float release = 0.3f,
                float hold_time = 1.5f) {
        // Attack/release envelope
        float attack_coeff = 1.0f - std::exp(-dt / attack);
        float release_coeff = 1.0f - std::exp(-dt / release);

        if (new_peak > display_peak)
            display_peak += (new_peak - display_peak) * attack_coeff;
        else
            display_peak += (new_peak - display_peak) * release_coeff;

        if (new_rms > display_rms)
            display_rms += (new_rms - display_rms) * attack_coeff;
        else
            display_rms += (new_rms - display_rms) * release_coeff;

        // Peak hold
        if (new_peak >= held_peak) {
            held_peak = new_peak;
            hold_counter = hold_time;
        } else {
            hold_counter -= dt;
            if (hold_counter <= 0) {
                held_peak += (0.0f - held_peak) * release_coeff;
            }
        }

        // Clamp to zero for very small values. `held_peak` decays exponentially
        // and so approaches zero without ever reaching it — left unclamped it
        // changes by a fraction of a bit every frame FOREVER, which is invisible
        // on screen but keeps a meter's idle gate (Meter::update) from ever
        // seeing a still frame. Snap it like the other two so a silent meter
        // truly settles.
        if (display_peak < 1e-6f) display_peak = 0;
        if (display_rms < 1e-6f) display_rms = 0;
        if (held_peak < 1e-6f) held_peak = 0;
    }
};

// ── Audio→UI Bridge ──────────────────────────────────────────────────────────

// Lock-free bridge for sending audio data from the audio thread to the UI.
// Uses TripleBuffer instead of FIFO: the audio thread always publishes the
// latest meter data without risk of overflow, and the UI thread always reads
// the most recent value. No data loss regardless of UI thread stalls.
class AudioBridge {
public:
    AudioBridge() = default;

    // Called from audio thread: publish new meter data
    void push_meter(const MeterData& data) {
        meter_buf_.write(data);
    }

    // Called from UI thread: get latest meter data
    // Returns true if data is available (always true after first push)
    bool pop_latest_meter(MeterData& out) {
        out = meter_buf_.read();
        return out.num_channels > 0;
    }

    // Convenience: compute peak and RMS from a buffer and push
    // Call from the audio callback after processing
    void analyze_and_push(const float* const* channels, int num_channels, int num_samples) {
        MeterData data;
        data.num_channels = (std::min)(num_channels, MeterData::max_channels);

        for (int ch = 0; ch < data.num_channels; ++ch) {
            float peak = 0;
            float sum_sq = 0;
            for (int i = 0; i < num_samples; ++i) {
                float s = std::abs(channels[ch][i]);
                if (s > peak) peak = s;
                sum_sq += channels[ch][i] * channels[ch][i];
            }
            data.peak[ch] = peak;
            data.rms[ch] = num_samples > 0 ? std::sqrt(sum_sq / num_samples) : 0;
        }

        push_meter(data);
    }

private:
    runtime::TripleBuffer<MeterData> meter_buf_;
};

} // namespace pulp::view
