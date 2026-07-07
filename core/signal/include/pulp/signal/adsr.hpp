#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::signal {

// ADSR envelope generator.
//
// RT contract: fixed-state and allocation-free. Call set_params() /
// set_sample_rate() from prepare/control code, then note_on(), note_off(),
// next(), apply_to_buffer(), reset(), and accessors are audio-thread safe.
template <typename SampleType = float>
class AdsrT {
public:
    struct Params {
        SampleType attack = SampleType{0.01f};   // seconds
        SampleType decay = SampleType{0.1f};     // seconds
        SampleType sustain = SampleType{0.7f};   // level 0-1
        SampleType release = SampleType{0.3f};   // seconds
    };

    void set_params(const Params& p) { params_ = p; }
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }

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
        level_ = SampleType{0.0f};
    }

    SampleType next() {
        switch (stage_) {
            case Stage::idle:
                return SampleType{0.0f};

            case Stage::attack: {
                SampleType rate = rate_for(params_.attack);
                level_ += rate;
                if (level_ >= SampleType{1.0f}) {
                    level_ = SampleType{1.0f};
                    stage_ = Stage::decay;
                }
                return level_;
            }

            case Stage::decay: {
                SampleType rate = rate_for(params_.decay);
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
                SampleType rate = rate_for(params_.release);
                level_ -= rate;
                if (level_ <= SampleType{0.0f}) {
                    level_ = SampleType{0.0f};
                    stage_ = Stage::idle;
                }
                return level_;
            }
        }
        return SampleType{0.0f};
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
    void apply_to_buffer(SampleType* buffer, int start_sample, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[start_sample + i] *= next();
    }

    /// Multiply each channel of a planar multi-channel buffer by the same
    /// envelope. All channels see the same envelope progression — the
    /// envelope advances once per sample, not per (sample, channel) pair.
    void apply_to_buffer(SampleType* const* channels, int num_channels,
                         int start_sample, int num_samples) {
        for (int i = 0; i < num_samples; ++i) {
            const SampleType env = next();
            for (int ch = 0; ch < num_channels; ++ch)
                channels[ch][start_sample + i] *= env;
        }
    }

private:
    Params params_;
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType level_ = SampleType{0.0f};
    Stage stage_ = Stage::idle;

    SampleType rate_for(SampleType time_seconds) const {
        if (time_seconds <= SampleType{0.0f}) return SampleType{1.0f};
        return SampleType{1.0f} / (time_seconds * sample_rate_);
    }
};

using Adsr = AdsrT<float>;
using Adsr64 = AdsrT<double>;

} // namespace pulp::signal
