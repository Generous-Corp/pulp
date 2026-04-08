#if defined(__ANDROID__)

#include "demo_synth.hpp"
#include <oboe/Oboe.h>
#include <android/log.h>
#include <cmath>
#include <algorithm>

#define PULP_LOG_TAG "PulpAudio"
#define PULP_LOGI(...) __android_log_print(ANDROID_LOG_INFO, PULP_LOG_TAG, __VA_ARGS__)

namespace pulp::demo {

static SynthParams g_params;
SynthParams& synth_params() { return g_params; }

// ── Simple Subtractive Synth ─────────────────────────────────────────────
// Two oscillators (saw + square mix), one-pole lowpass filter, ADSR envelope.
// All parameters read from g_params via relaxed atomics — lock-free.

class DemoSynth : public oboe::AudioStreamDataCallback,
                  public oboe::AudioStreamErrorCallback {
public:
    bool start() {
        oboe::AudioStreamBuilder builder;
        builder.setDirection(oboe::Direction::Output)
            ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
            ->setSharingMode(oboe::SharingMode::Exclusive)
            ->setFormat(oboe::AudioFormat::Float)
            ->setChannelCount(oboe::ChannelCount::Stereo)
            ->setSampleRateConversionQuality(oboe::SampleRateConversionQuality::Best)
            ->setDataCallback(this)
            ->setErrorCallback(this);

        auto result = builder.openManagedStream(stream_);
        if (result != oboe::Result::OK) {
            PULP_LOGI("DemoSynth: failed to open stream: %s", oboe::convertToText(result));
            return false;
        }

        sample_rate_ = static_cast<float>(stream_->getSampleRate());
        PULP_LOGI("DemoSynth: stream opened — %.0f Hz, %d frames/burst",
                  sample_rate_, stream_->getFramesPerBurst());

        result = stream_->requestStart();
        if (result != oboe::Result::OK) {
            PULP_LOGI("DemoSynth: failed to start: %s", oboe::convertToText(result));
            return false;
        }

        playing_.store(true);
        PULP_LOGI("DemoSynth: playing");
        return true;
    }

    void stop() {
        playing_.store(false);
        if (stream_) {
            stream_->requestStop();
            stream_->close();
            stream_.reset();
        }
        PULP_LOGI("DemoSynth: stopped");
    }

    bool is_playing() const { return playing_.load(); }
    float peak_level() const { return peak_level_.load(std::memory_order_relaxed); }

private:
    // Map 0..1 knob to frequency (MIDI note range 36..96)
    float pitch_to_hz(float norm) {
        float note = 36.0f + norm * 60.0f;  // C2 to C7
        return 440.0f * std::pow(2.0f, (note - 69.0f) / 12.0f);
    }

    // Map 0..1 to filter cutoff Hz (20..20000, exponential)
    float cutoff_to_hz(float norm) {
        return 20.0f * std::pow(1000.0f, norm);
    }

    // One-pole lowpass coefficient from cutoff frequency
    float lp_coeff(float cutoff_hz) {
        float w = 2.0f * 3.14159265f * cutoff_hz / sample_rate_;
        return std::clamp(w / (1.0f + w), 0.0f, 1.0f);
    }

    // Band-limited saw via naive with mild rolloff
    float saw(float& phase, float freq) {
        phase += freq / sample_rate_;
        if (phase >= 1.0f) phase -= 2.0f;
        return phase;
    }

    // Square from saw
    float square(float phase) {
        return phase >= 0.0f ? 0.5f : -0.5f;
    }

    // Simple ADSR (linear segments, per-sample)
    float process_env(float a, float d, float s, float r) {
        float a_rate = a > 0.001f ? 1.0f / (a * sample_rate_) : 1.0f;
        float d_rate = d > 0.001f ? 1.0f / (d * sample_rate_) : 1.0f;
        float r_rate = r > 0.001f ? 1.0f / (r * sample_rate_) : 1.0f;

        switch (env_stage_) {
            case 0: // attack
                env_level_ += a_rate;
                if (env_level_ >= 1.0f) { env_level_ = 1.0f; env_stage_ = 1; }
                break;
            case 1: // decay
                env_level_ -= d_rate * (1.0f - s);
                if (env_level_ <= s) { env_level_ = s; env_stage_ = 2; }
                break;
            case 2: // sustain
                env_level_ = s;
                break;
            case 3: // release
                env_level_ -= r_rate;
                if (env_level_ <= 0.0f) { env_level_ = 0.0f; env_stage_ = 0; }
                break;
        }
        return env_level_;
    }

    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream*, void* audio_data, int32_t num_frames) override {

        auto* out = static_cast<float*>(audio_data);
        auto& p = g_params;

        // Read all params once per buffer (relaxed is fine for audio)
        float pitch = p.osc_pitch.load(std::memory_order_relaxed);
        float detune = p.osc_detune.load(std::memory_order_relaxed);
        float mix = p.osc_mix.load(std::memory_order_relaxed);
        float level = p.osc_level.load(std::memory_order_relaxed);
        float cutoff = p.filter_cutoff.load(std::memory_order_relaxed);
        float reso = p.filter_reso.load(std::memory_order_relaxed);
        float att = p.env_attack.load(std::memory_order_relaxed) * 2.0f;  // 0..2s
        float dec = p.env_decay.load(std::memory_order_relaxed) * 2.0f;
        float sus = p.env_sustain.load(std::memory_order_relaxed);
        float rel = p.env_release.load(std::memory_order_relaxed) * 3.0f;
        float master = p.master.load(std::memory_order_relaxed);
        bool osc1 = p.osc1_on.load(std::memory_order_relaxed);
        bool osc3 = p.osc3_on.load(std::memory_order_relaxed);

        float freq1 = pitch_to_hz(pitch);
        float freq2 = freq1 * (1.0f + detune * 0.02f);  // slight detune
        float cut_hz = cutoff_to_hz(cutoff);
        float coeff = lp_coeff(cut_hz);
        float feedback = reso * 0.9f;  // resonance feedback
        float peak = 0.0f;

        for (int i = 0; i < num_frames; ++i) {
            // Oscillators
            float osc = 0.0f;
            if (osc1) {
                float s1 = saw(phase1_, freq1);
                float sq1 = square(phase1_);
                osc += (1.0f - mix) * s1 + mix * sq1;
            }
            if (osc3) {
                float s2 = saw(phase2_, freq2);
                float sq2 = square(phase2_);
                osc += ((1.0f - mix) * s2 + mix * sq2) * 0.7f;
            }

            // Envelope
            float env = process_env(att, dec, sus, rel);

            // Filter (one-pole with resonance feedback)
            float input = osc * env * level - filter_state_ * feedback;
            filter_state_ += coeff * (input - filter_state_);
            float filtered = filter_state_;

            // Output — level is 0..1 from UI, scale up for audibility
            float sample = filtered * master * 0.7f;
            sample = std::clamp(sample, -1.0f, 1.0f);
            peak = std::max(peak, std::abs(sample));

            // Stereo (slight pan spread)
            out[i * 2]     = sample * 0.95f;  // L
            out[i * 2 + 1] = sample;          // R
        }

        // Update peak for visual metering (relaxed — UI reads it)
        peak_level_.store(peak, std::memory_order_relaxed);

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream*, oboe::Result error) override {
        PULP_LOGI("DemoSynth: stream error — %s, restarting", oboe::convertToText(error));
        if (playing_.load()) start();
    }

    oboe::ManagedStream stream_;
    std::atomic<bool> playing_{false};
    float sample_rate_ = 48000.0f;

    // Oscillator state
    float phase1_ = 0.0f;
    float phase2_ = 0.0f;

    // Filter state
    float filter_state_ = 0.0f;

    // Envelope state
    float env_level_ = 0.0f;
    int env_stage_ = 0;  // 0=attack, 1=decay, 2=sustain, 3=release

    // Peak metering (for visual indicator)
    std::atomic<float> peak_level_{0.0f};
};

static DemoSynth g_synth;

bool synth_start() { return g_synth.start(); }
void synth_stop() { g_synth.stop(); }
bool synth_is_playing() { return g_synth.is_playing(); }
float synth_peak_level() { return g_synth.peak_level(); }

} // namespace pulp::demo

#endif // __ANDROID__
