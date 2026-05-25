#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::signal {

// ADSR envelope generator
// Real-time safe. Call note_on()/note_off(), then next() per sample.
class Adsr {
public:
    struct Params {
        float attack = 0.01f;   // seconds
        float decay = 0.1f;     // seconds
        float sustain = 0.7f;   // level 0-1
        float release = 0.3f;   // seconds
    };

    void set_params(const Params& p) { params_ = p; }
    void set_sample_rate(float sr) { sample_rate_ = sr; }

    void note_on() {
        stage_ = Stage::attack;
        // Don't reset level_ — allows retriggering from current position
    }

    void note_off() {
        if (stage_ != Stage::idle)
            stage_ = Stage::release;
    }

    void reset() {
        stage_ = Stage::idle;
        level_ = 0.0f;
    }

    float next() {
        switch (stage_) {
            case Stage::idle:
                return 0.0f;

            case Stage::attack: {
                float rate = rate_for(params_.attack);
                level_ += rate;
                if (level_ >= 1.0f) {
                    level_ = 1.0f;
                    stage_ = Stage::decay;
                }
                return level_;
            }

            case Stage::decay: {
                float rate = rate_for(params_.decay);
                level_ -= rate;
                if (level_ <= params_.sustain) {
                    level_ = params_.sustain;
                    stage_ = Stage::sustain;
                }
                return level_;
            }

            case Stage::sustain:
                return level_;

            case Stage::release: {
                float rate = rate_for(params_.release);
                level_ -= rate;
                if (level_ <= 0.0f) {
                    level_ = 0.0f;
                    stage_ = Stage::idle;
                }
                return level_;
            }
        }
        return 0.0f;
    }

    bool is_active() const { return stage_ != Stage::idle; }

    enum class Stage { idle, attack, decay, sustain, release };
    Stage stage() const { return stage_; }

    /// Multiply @p num_samples of @p buffer (starting at @p start_sample)
    /// by successive envelope values. Advances the envelope by @p num_samples.
    /// Real-time safe; no allocations.
    ///
    /// @code
    /// adsr.apply_to_buffer(audio_data, 0, block_size);  // amplitude envelope
    /// @endcode
    void apply_to_buffer(float* buffer, int start_sample, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[start_sample + i] *= next();
    }

    /// Multiply each channel of a planar multi-channel buffer by the same
    /// envelope. All channels see the same envelope progression — the
    /// envelope advances once per sample, not per (sample, channel) pair.
    void apply_to_buffer(float* const* channels, int num_channels,
                         int start_sample, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            const float env = next();
            for (int ch = 0; ch < num_channels; ++ch)
                channels[ch][start_sample + i] *= env;
        }
    }

private:
    Params params_;
    float sample_rate_ = 44100.0f;
    float level_ = 0.0f;
    Stage stage_ = Stage::idle;

    float rate_for(float time_seconds) const {
        if (time_seconds <= 0.0f) return 1.0f;
        return 1.0f / (time_seconds * sample_rate_);
    }
};

} // namespace pulp::signal
