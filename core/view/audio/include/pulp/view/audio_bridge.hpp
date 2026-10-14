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

    /// Update with new audio data. Call once per UI frame.
    /// attack/release in seconds, hold_time in seconds, dt = frame time.
    ///
    /// RETURNS TRUE ONLY WHEN A DISPLAYED VALUE MOVED, and a meter must repaint
    /// only when it does. A UI thread polls this every frame for as long as it
    /// is bound — through silence, when the ballistics have already settled and
    /// the next frame would be pixel-identical. Repainting anyway costs a
    /// composite per meter per frame forever, and on the plug-in-view-host path
    /// (a DAW) each of those is a FULL-SURFACE repaint of the whole editor: an
    /// idle window then renders at the display's refresh rate for as long as it
    /// is open. Any real level change fails the compare on the very next frame,
    /// so the gate never swallows motion — only the still frames after it.
    ///
    /// `MultiChannelBallistics::update` reports movement the same way. If you
    /// are only advancing the ballistics and will never paint, say so with
    /// `(void)`.
    [[nodiscard]] bool update(float new_peak, float new_rms, float dt,
                              float attack = 0.001f, float release = 0.3f,
                              float hold_time = 1.5f) {
        const float prev_peak = display_peak;
        const float prev_rms = display_rms;
        const float prev_held = held_peak;

        // ONE NON-FINITE SAMPLE WOULD DISABLE THE GATE PERMANENTLY. A NaN takes
        // the release branch, poisons the display value, survives the snaps
        // below (every comparison against NaN is false), and then compares
        // unequal to itself forever — so `changed` would be true on every frame
        // for the rest of the process. Sanitise the input, and self-heal state
        // that arrived non-finite by another route.
        if (!std::isfinite(new_peak)) new_peak = 0.0f;
        if (!std::isfinite(new_rms)) new_rms = 0.0f;
        if (!std::isfinite(display_peak)) display_peak = 0.0f;
        if (!std::isfinite(display_rms)) display_rms = 0.0f;
        if (!std::isfinite(held_peak)) held_peak = 0.0f;

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
            // Floored: left to run it decrements without bound for as long as
            // the meter is polled, which is forever.
            hold_counter = std::max(0.0f, hold_counter - dt);
            if (hold_counter <= 0) {
                held_peak += (0.0f - held_peak) * release_coeff;
            }
        }

        // Clamp to zero for very small values. Each of these decays
        // exponentially and so approaches zero without ever reaching it — left
        // unclamped it changes by a fraction of a bit every frame FOREVER, which
        // is invisible on screen but means the meter never reports a still
        // frame. Snapping is what makes a silent meter actually settle.
        //
        // These three rest at ZERO, so a floor test is enough. A smoother whose
        // resting value is NOT zero needs the general form —
        // `if (std::abs(v - target) < eps) v = target` — see
        // CorrelationMeter::update, which settles wherever the signal is.
        if (display_peak < 1e-6f) display_peak = 0;
        if (display_rms < 1e-6f) display_rms = 0;
        if (held_peak < 1e-6f) held_peak = 0;

        return display_peak != prev_peak || display_rms != prev_rms ||
               held_peak != prev_held;
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
