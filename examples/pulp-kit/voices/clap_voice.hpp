#pragma once

// The 808 handclap: white noise gated by a fast repeating flam envelope, then a
// longer diffuse tail, all through one resonant band-pass.
//
// The circuit is a noise source feeding a VCA whose control is a small three-
// pulse burst -- an oscillator that fires a few closely spaced gates and stops
// -- summed with a separate slow-decay envelope. The three quick gates are the
// "flams" that give a hand-clap its stuttered, several-hands-not-quite-together
// attack; the slow envelope is the room-like tail after the hands part. Both
// drive the same band-pass, so the whole event has one timbre, only its
// amplitude contour changes across the flams and the tail.
//
// Why gate the noise *before* the band-pass rather than after: the filter turns
// the hard gate edges into a short ring instead of a click, and it is the same
// ring on every flam, which is what fuses the three gates into one perceived
// clap rather than three separate ticks.
//
// Reference character (48 kHz, v120): a resonant band-pass peak at ~1150 Hz
// (measured, ~-6.5 dB one octave to either side, so Q ~ 2), three amplitude
// flams ~10 ms apart in the first ~30 ms, then a diffuse tail with T60 ~1.0 s;
// broadband (spectral flatness ~0.33), peak ~0.42. The band-pass centre, Q,
// flam spacing, decays and level are calibrated to those measured numbers; the
// sound itself is generated sample by sample by the noise source and filter.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>

#include "noise_source.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::examples {

/// Handclap configuration. The flam burst is @ref flam_count gates spaced
/// @ref flam_spacing_s apart, each decaying with 60 dB time @ref flam_t60_s;
/// the tail is one gate fired @ref tail_delay_s after the burst starts, with
/// 60 dB time @ref tail_t60_s and relative weight @ref tail_level. Everything
/// runs through a band-pass at @ref bandpass_hz / @ref bandpass_q.
struct ClapConfig {
    int flam_count = 3;
    float flam_spacing_s = 0.010f;   ///< gap between flam gates
    float flam_t60_s = 0.012f;       ///< fast per-flam decay
    float tail_delay_s = 0.030f;     ///< tail fires one flam-slot after the burst
    float tail_t60_s = 1.00f;        ///< long diffuse tail decay
    float tail_level = 0.85f;        ///< tail gate height relative to a flam
    float bandpass_hz = 1150.0f;
    float bandpass_q = 2.0f;
    float gain = 1.72f;
};

/// The handclap voice. prepare() sizes state; trigger()/process() are
/// audio-thread safe and allocate nothing.
class ClapVoice {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        apply_config();
        reset();
    }

    void set_config(const ClapConfig& cfg) {
        cfg_ = cfg;
        apply_config();
    }
    const ClapConfig& config() const noexcept { return cfg_; }

    void set_gain(float gain) noexcept {
        cfg_.gain = gain;
        eff_gain_ = cfg_.gain * level_;
    }

    // --- Uniform kit control surface -------------------------------------
    // The four setters every kit voice exposes with these exact signatures.
    // Their defaults -- set_level(1)/set_tune(1)/set_decay(0.5)/set_tone(0.5) --
    // reproduce the calibrated handclap bit-for-bit; each only widens a musical
    // range around today's sound, it never shifts the default. set_config()
    // stays as the richer, voice-specific escape hatch.

    /// Output gain. 1.0 == the calibrated loudness (ClapConfig::gain). Linear,
    /// so set_level(0.5) halves the peak and 0 is silent.
    void set_level(float level01) noexcept {
        level_ = std::clamp(level01, 0.0f, 1.0f);
        eff_gain_ = cfg_.gain * level_;
    }

    /// Body resonance. A handclap has no strong pitch, so its nearest pitch
    /// analogue is the band-pass centre -- the resonant "body" of the event.
    /// set_tune scales that centre, moving the body up or down. 1.0 == the
    /// calibrated ~1150 Hz; the range spans two octaves either side.
    void set_tune(float ratio) noexcept {
        tune_ = std::clamp(ratio, 0.25f, 4.0f);
        apply_config();
    }

    /// Tail length. 0.5 == the calibrated diffuse-tail T60 (~1.0 s). 0..1 spans
    /// a decade of 60 dB decay time (~0.5 s .. ~2.0 s) around it.
    void set_decay(float decay01) noexcept {
        decay_ = std::clamp(decay01, 0.0f, 1.0f);
        apply_config();
    }

    /// Brightness. 0.5 == the calibrated band-pass (1150 Hz, Q 2). Higher pushes
    /// the centre up and opens the Q (wider, brighter); lower drops the centre
    /// and tightens the Q (darker) -- so it moves the spectral centroid.
    void set_tone(float tone01) noexcept {
        tone_ = std::clamp(tone01, 0.0f, 1.0f);
        apply_config();
    }

    float level() const noexcept { return level_; }
    float tune() const noexcept { return tune_; }
    float decay() const noexcept { return decay_; }
    float tone() const noexcept { return tone_; }

    void reset() noexcept {
        noise_.reset();
        bandpass_.reset();
        flam_env_ = 0.0f;
        tail_env_ = 0.0f;
        velocity_ = 0.0f;
        sample_counter_ = -1;  // inactive
        flams_left_ = 0;
        next_flam_sample_ = 0;
        tail_sample_ = 0;
        tail_pending_ = false;
    }

    /// Strike at @p velocity (0..1). Restarts the flam burst and schedules the
    /// tail; the noise stream free-runs so successive claps are not identical.
    void trigger(float velocity) noexcept {
        velocity_ = std::clamp(velocity, 0.0f, 1.0f);
        sample_counter_ = 0;
        flams_left_ = std::max(1, cfg_.flam_count);
        next_flam_sample_ = 0;             // first flam fires immediately
        tail_sample_ = tail_delay_samples_;
        tail_pending_ = true;
        flam_env_ = 0.0f;
        tail_env_ = 0.0f;
    }

    float process() noexcept {
        // Fire scheduled flam gates and the tail gate.
        if (sample_counter_ >= 0) {
            if (flams_left_ > 0 && sample_counter_ == next_flam_sample_) {
                flam_env_ = 1.0f;
                --flams_left_;
                next_flam_sample_ += flam_spacing_samples_;
            }
            if (tail_pending_ && sample_counter_ == tail_sample_) {
                tail_env_ = cfg_.tail_level;
                tail_pending_ = false;
            }
            ++sample_counter_;
        }

        const float env = (flam_env_ + tail_env_) * velocity_;
        const float shaped = bandpass_.process(noise_.process() * env);

        flam_env_ = signal::snap_to_zero(flam_env_ * flam_decay_coeff_);
        tail_env_ = signal::snap_to_zero(tail_env_ * tail_decay_coeff_);
        return eff_gain_ * shaped;
    }

    /// Upper bound on |process()|. The gated noise is bounded by
    /// |noise * env| <= env_max, where a flam (peak 1) and the tail (peak
    /// tail_level) can coincide, and a linear filter raises a bounded input by
    /// at most the L1 norm of its impulse response. So gain * env_max * L1 is a
    /// true bound -- the L1 norm, not the band-pass peak magnitude, is the
    /// worst-case time-domain gain.
    float peak_bound() const noexcept {
        const float env_max = 1.0f + std::max(0.0f, cfg_.tail_level);
        return eff_gain_ * env_max * filter_l1_gain_;
    }

private:
    float coeff_for_t60(float t60_s) const noexcept {
        if (t60_s <= 0.0f) return 0.0f;
        return static_cast<float>(std::pow(10.0, -3.0 / (t60_s * sample_rate_)));
    }

    void apply_config() {
        flam_spacing_samples_ =
            std::max(1, static_cast<int>(cfg_.flam_spacing_s * sample_rate_));
        tail_delay_samples_ =
            std::max(0, static_cast<int>(cfg_.tail_delay_s * sample_rate_));
        flam_decay_coeff_ = coeff_for_t60(cfg_.flam_t60_s);

        // set_decay walks the tail's 60 dB time a decade around the calibrated
        // value; decay_ == 0.5 lands exactly on cfg_.tail_t60_s (exp2(0) == 1).
        const float tail_t60 =
            cfg_.tail_t60_s * std::exp2((decay_ - 0.5f) * kDecayOctaveSpan);
        tail_decay_coeff_ = coeff_for_t60(tail_t60);

        // set_tune scales the band-pass centre (the clap's "body"); set_tone
        // additionally shifts the centre and opens/tightens the Q for
        // brightness. Both fold onto the one band-pass so the event keeps a
        // single timbre. tune_ == 1 and tone_ == 0.5 leave centre/Q at the
        // calibrated 1150 Hz / Q 2 (both exp2(0) == 1).
        const float nyq = 0.45f * static_cast<float>(sample_rate_);
        const float centre = std::clamp(
            cfg_.bandpass_hz * tune_ * std::exp2((tone_ - 0.5f) * kToneOctaveSpan),
            20.0f, nyq);
        const float q = std::clamp(
            cfg_.bandpass_q * std::exp2(-(tone_ - 0.5f) * kToneQSpan), 0.3f, 20.0f);
        bandpass_.set_coefficients(signal::Biquad::Type::bandpass, centre, q,
                                   static_cast<float>(sample_rate_));
        filter_l1_gain_ = measure_filter_l1(bandpass_);

        eff_gain_ = cfg_.gain * level_;
    }

    /// L1 norm of the band-pass impulse response: the exact worst-case
    /// |output| for a unit-bounded input. Runs at prepare time on a copy over a
    /// fixed settle window; allocation-free.
    static float measure_filter_l1(signal::Biquad filter) noexcept {
        filter.reset();
        double sum = 0.0;
        float in = 1.0f;
        for (int n = 0; n < 8192; ++n) {
            sum += std::fabs(static_cast<double>(filter.process(in)));
            in = 0.0f;
        }
        return static_cast<float>(sum);
    }

    // Control-range spans for the uniform surface, in octaves across the full
    // 0..1 (or ratio) knob travel. Chosen so the extremes stay musical and the
    // band-pass stays stable well inside Nyquist.
    static constexpr float kDecayOctaveSpan = 2.0f;  ///< tail T60: +/-1 octave
    static constexpr float kToneOctaveSpan = 2.0f;   ///< centre: +/-1 octave
    static constexpr float kToneQSpan = 1.0f;        ///< Q: +/-half octave

    signal::Biquad bandpass_{};
    WhiteNoise noise_{};

    double sample_rate_ = 48000.0;
    ClapConfig cfg_{};

    // Uniform control state. Defaults reproduce the calibrated sound exactly.
    float level_ = 1.0f;
    float tune_ = 1.0f;
    float decay_ = 0.5f;
    float tone_ = 0.5f;
    float eff_gain_ = 1.72f;  ///< cfg_.gain * level_, recomputed in apply_config

    // Scheduling state (samples).
    int flam_spacing_samples_ = 480;
    int tail_delay_samples_ = 1440;
    int sample_counter_ = -1;
    int flams_left_ = 0;
    int next_flam_sample_ = 0;
    int tail_sample_ = 0;
    bool tail_pending_ = false;

    // Envelope state.
    float flam_env_ = 0.0f;
    float tail_env_ = 0.0f;
    float flam_decay_coeff_ = 0.0f;
    float tail_decay_coeff_ = 0.0f;
    float velocity_ = 0.0f;
    float filter_l1_gain_ = 1.0f;
};

/// Reference handclap preset (48 kHz, v120): three flams ~10 ms apart into a
/// diffuse tail, band-pass peak ~1150 Hz, T60 ~1.0 s, peak ~0.42.
inline ClapConfig clap_config() { return ClapConfig{}; }

}  // namespace pulp::examples
