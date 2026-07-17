#pragma once

// The 808 maracas: a short, bright, filtered white-noise burst.
//
// The simplest of the noise voices -- noise into a high-pass into a fast VCA.
// A shaker is a swarm of small beads striking a shell, which is broadband hiss
// weighted strongly toward the high end and gone almost as soon as it starts;
// there is no body resonance and no flam structure. So there is one envelope
// (a fast exponential, no attack ramp needed since noise carries no DC step)
// and one high-pass that tilts the flat hiss bright. The band-pass voices put a
// resonant peak on the noise; the maracas deliberately does not -- keeping the
// spectrum broad and flat above the corner is what makes it read as a shaker
// rather than a pitched tick.
//
// Character: a broadband burst (spectral flatness stays high -- no resonance to
// concentrate the energy), centroid up in the several-kHz range from the
// high-pass tilt, and a short decay (T60 ~80 ms) so it sits tight in a groove.
// The corner, decay and level are calibrated; the hiss is generated sample by
// sample by the noise source and high-pass below.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>

#include "noise_source.hpp"

#include <algorithm>
#include <cmath>

namespace pulp::examples {

/// Maracas configuration: a high-pass at @ref highpass_hz / @ref highpass_q
/// tilts the noise bright, a single exponential of 60 dB time @ref t60_s shapes
/// the burst, and @ref gain sets the level.
struct MaracasConfig {
    float highpass_hz = 4000.0f;
    float highpass_q = 0.707f;
    float t60_s = 0.080f;
    float gain = 0.30f;
};

/// The maracas voice. prepare() sizes state; trigger()/process() are
/// audio-thread safe and allocate nothing.
class MaracasVoice {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        apply_config();
        reset();
    }

    void set_config(const MaracasConfig& cfg) {
        cfg_ = cfg;
        apply_config();
    }
    const MaracasConfig& config() const noexcept { return cfg_; }

    void set_t60(float t60_s) noexcept {
        cfg_.t60_s = t60_s;
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);
    }

    void set_gain(float gain) noexcept { cfg_.gain = gain; }

    // -- Uniform kit control surface -----------------------------------------
    //
    // The four normalized setters every kit voice exposes, mapped onto this
    // voice's existing MaracasConfig. Each moves exactly one audible axis and is
    // centered so the calibrated defaults -- set_level(1), set_tune(1),
    // set_decay(0.5), set_tone(0.5) -- reproduce the stock sound bit-for-bit
    // (each reduces to a multiply by exactly 1.0). All are audio-thread safe:
    // they recompute scalars and filter coefficients in place, never allocate
    // or lock. The base_* values are the calibration captured by apply_config,
    // so 1.0/0.5 always refers to the current preset, not a fixed constant.
    //
    // The maracas has no pitched resonator, so "tune" has no fundamental to
    // shift -- its nearest pitch analogue is the high-pass corner, the frequency
    // where the broadband hiss begins. tune scales that corner as a pitch ratio;
    // tone scales the same corner as a brightness control. Because both act on
    // the one high-pass this voice owns, they are tracked as independent
    // multipliers (tune_ratio_ and tone01_) and composed, so setting one never
    // clobbers the other.

    /// Output level. 0..1; 1.0 is the calibrated loudness, 0.0 silences. Scales
    /// the master gain linearly, so set_level(0.5) halves the peak.
    void set_level(float level01) noexcept {
        cfg_.gain = base_gain_ * std::clamp(level01, 0.0f, 1.0f);
    }

    /// Pitch. `ratio` multiplies the high-pass corner (the shaker's only pitch
    /// analogue); 1.0 is the calibrated corner, >1 pushes the hiss brighter and
    /// higher. Clamped to a musical [0.5, 1.5]. Composes with set_tone.
    void set_tune(float ratio) noexcept {
        tune_ratio_ = std::clamp(std::isfinite(ratio) ? ratio : 1.0f, kTuneMin, kTuneMax);
        apply_highpass();
    }

    /// Decay. 0..1; 0.5 is the calibrated ~80 ms tail. Scales t60 geometrically
    /// over a 4x range each way (base/4 .. base*4), so both extremes stay
    /// musical: higher lengthens the T60, lower shortens it.
    void set_decay(float decay01) noexcept {
        const float d = std::clamp(decay01, 0.0f, 1.0f);
        set_t60(base_t60_ * std::pow(kDecayRange, (d - 0.5f) * 2.0f));
    }

    /// Tone/brightness. 0..1; 0.5 is the calibrated high-pass corner. Sweeps the
    /// corner geometrically over a 2x range each way: raising it lifts the
    /// spectral centroid (fewer lows pass), lowering it darkens. Composes with
    /// set_tune. Audio-thread safe (recomputes coefficients in place; filter
    /// state is preserved so a live burst keeps ringing through the change).
    void set_tone(float tone01) noexcept {
        tone01_ = std::clamp(tone01, 0.0f, 1.0f);
        apply_highpass();
    }

    void reset() noexcept {
        noise_.reset();
        highpass_.reset();
        env_ = 0.0f;
    }

    /// Strike at @p velocity (0..1). The noise stream free-runs; only the
    /// envelope restarts.
    void trigger(float velocity) noexcept {
        env_ = std::clamp(velocity, 0.0f, 1.0f);
    }

    float process() noexcept {
        const float shaped = highpass_.process(noise_.process());
        const float out = cfg_.gain * env_ * shaped;
        env_ = signal::snap_to_zero(env_ * decay_coeff_);
        return out;
    }

    /// Upper bound on |process()|. |noise * env| <= gain (env starts at <= 1),
    /// and a linear filter can only raise a bounded input by the L1 norm of its
    /// impulse response, so gain * L1(high-pass) is a true bound. A filter's
    /// worst-case time-domain gain is that L1 norm, not its pass-band
    /// magnitude: a high-pass overshoots unity on a step even at Q = 0.707,
    /// which is exactly why the naive pass-band bound would be violated.
    float peak_bound() const noexcept { return cfg_.gain * filter_l1_gain_; }

private:
    float coeff_for_t60(float t60_s) const noexcept {
        if (t60_s <= 0.0f) return 0.0f;
        return static_cast<float>(std::pow(10.0, -3.0 / (t60_s * sample_rate_)));
    }

    void apply_config() {
        // Snapshot the calibrated defaults the uniform setters center on, so
        // set_level(1)/set_decay(0.5)/set_tune(1)/set_tone(0.5) always mean
        // "this preset". Capture before apply_highpass, which writes the
        // (tune/tone-scaled) effective corner back into cfg_.highpass_hz.
        base_highpass_hz_ = cfg_.highpass_hz;
        base_t60_ = cfg_.t60_s;
        base_gain_ = cfg_.gain;
        apply_highpass();
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);
    }

    /// Recompute the high-pass corner from the base calibration and the current
    /// tune/tone multipliers, load the coefficients, and refresh the L1 peak
    /// bound. At the defaults (tune_ratio_ = 1, tone01_ = 0.5) the scale factor
    /// is pow(kToneRange, 0) * 1 == 1, so the corner is bit-identical to the
    /// preset. Reconfiguring does not reset the filter state, so a live burst
    /// keeps ringing through a control change.
    void apply_highpass() noexcept {
        const float hp = base_highpass_hz_ * tune_ratio_ *
                         std::pow(kToneRange, (tone01_ - 0.5f) * 2.0f);
        highpass_.set_coefficients(signal::Biquad::Type::highpass, hp,
                                   cfg_.highpass_q, static_cast<float>(sample_rate_));
        cfg_.highpass_hz = hp;
        filter_l1_gain_ = measure_filter_l1(highpass_);
    }

    /// L1 norm of a configured biquad's impulse response: the exact worst-case
    /// |output| for a unit-bounded input. Runs at prepare time on a copy (so it
    /// does not disturb the live filter state) over a fixed window long enough
    /// for a stable second-order section to settle; allocation-free.
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

    static constexpr float kDecayRange = 4.0f;  ///< set_decay 0..1 spans base/4..base*4
    static constexpr float kToneRange = 2.0f;   ///< set_tone 0..1 spans base/2..base*2
    static constexpr float kTuneMin = 0.5f;     ///< set_tune lower clamp (pitch ratio)
    static constexpr float kTuneMax = 1.5f;     ///< set_tune upper clamp (pitch ratio)

    signal::Biquad highpass_{};
    WhiteNoise noise_{};

    double sample_rate_ = 48000.0;
    MaracasConfig cfg_{};

    float env_ = 0.0f;
    float decay_coeff_ = 0.0f;
    float filter_l1_gain_ = 1.0f;

    // Uniform control state. The multipliers default to the calibrated preset:
    // tune_ratio_ = 1 and tone01_ = 0.5 leave the corner untouched. The base_*
    // values are the calibration the setters center on (set by apply_config).
    float tune_ratio_ = 1.0f;
    float tone01_ = 0.5f;
    float base_gain_ = 0.30f;
    float base_t60_ = 0.080f;
    float base_highpass_hz_ = 4000.0f;
};

/// Reference maracas preset (48 kHz): a bright high-passed hiss with a short
/// ~80 ms decay.
inline MaracasConfig maracas_config() { return MaracasConfig{}; }

}  // namespace pulp::examples
