#pragma once

// The 808 cowbell: two square-wave oscillators into a band-pass and an
// amplitude envelope.
//
// Where the hi-hat/cymbal stack six oscillators for a dense metallic cluster,
// the cowbell uses only two -- a low pair whose ratio gives the hollow,
// clangy two-tone. Both are square waves, so each contributes a stack of odd
// harmonics; the band-pass sits up on those harmonics (not on the
// fundamentals) which is why the measured spectral centroid lands well above
// either oscillator's pitch. The envelope is a medium exponential decay.
//
// The output is dominated by a clear tonal pitch (the reference measures a
// confident fundamental near 818 Hz with near-zero spectral flatness), so
// unlike the noise voices this one is verified by reading its two partials
// straight back out of the spectrum.
//
// Circuit lineage: the TR-808 cowbell -- two square oscillators summed into a
// band-pass filter and a VCA. The two frequencies, band-pass centre/Q and
// decay are calibrated to the reference instrument's measured output; the
// tone is generated sample by sample by the oscillators and filter below.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/square_osc_bank.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::examples {

/// Cowbell configuration: the two oscillator frequencies (Hz), the band-pass
/// centre/Q that shapes the harmonics, the envelope's 60 dB decay time, and an
/// output gain that matches the reference level.
struct CowbellConfig {
    float low_hz = 545.0f;
    float high_hz = 814.0f;
    /// The high oscillator dominates on the reference; weight it above the low.
    float low_amp = 0.6f;
    float high_amp = 1.0f;
    float bandpass_hz = 2640.0f;
    float bandpass_q = 1.2f;
    /// A gentle low-pass after the band-pass rolls off the squares' high odd
    /// harmonics, which a single 6 dB/oct band-pass skirt leaves audible and
    /// which would otherwise drag the spectral centroid far above the
    /// reference's ~2.4 kHz.
    float lowpass_hz = 4200.0f;
    float lowpass_q = 0.707f;
    float t60_s = 0.981f;
    float gain = 0.185f;
};

/// The cowbell voice. prepare() sizes state; trigger()/process() are
/// audio-thread safe and allocate nothing.
class CowbellVoice {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        osc_.prepare(static_cast<float>(sample_rate_));
        apply_config();
        reset();
    }

    void set_config(const CowbellConfig& cfg) {
        cfg_ = cfg;
        apply_config();
    }
    const CowbellConfig& config() const noexcept { return cfg_; }

    void set_t60(float t60_s) noexcept {
        cfg_.t60_s = t60_s;
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);
    }

    void set_gain(float gain) noexcept {
        cfg_.gain = gain;
        osc_.set_level(gain);
    }

    // ---- Uniform kit-voice control surface -------------------------------
    // Four normalized controls shared by every kit voice. Their defaults --
    // set_level(1), set_tune(1), set_decay(0.5), set_tone(0.5) -- reproduce the
    // calibrated reference sound exactly; each control only adds musical range
    // around that point. They map onto the existing CowbellConfig fields.

    /// Output gain in [0, 1]. 1.0 is the calibrated reference loudness; 0.5
    /// halves the peak. Maps to CowbellConfig::gain.
    void set_level(float level01) noexcept {
        level01_ = std::clamp(level01, 0.0f, 1.0f);
        set_gain(base_gain_ * level01_);
    }
    float level() const noexcept { return level01_; }

    /// Pitch multiplier; 1.0 is the calibrated two-tone. Scales both oscillator
    /// frequencies together so the hollow low/high ratio is preserved. The
    /// band-pass centre is fixed, so tuning up also lifts the spectrum. Clamped
    /// to a musical 0.25..4.0 octave range. Maps to CowbellConfig::low_hz and
    /// ::high_hz.
    void set_tune(float ratio) noexcept {
        tune_ = std::clamp(ratio, 0.25f, 4.0f);
        cfg_.low_hz = base_low_hz_ * tune_;
        cfg_.high_hz = base_high_hz_ * tune_;
        osc_.set_frequency(0, cfg_.low_hz);
        osc_.set_frequency(1, cfg_.high_hz);
    }
    float tune() const noexcept { return tune_; }

    /// Tail length in [0, 1]. 0.5 is the calibrated T60 (~0.981 s). Exponential
    /// around the default so the decay stays musical: 0 -> ~1/3x, 1 -> ~3x.
    /// Maps to CowbellConfig::t60_s.
    void set_decay(float decay01) noexcept {
        decay01_ = std::clamp(decay01, 0.0f, 1.0f);
        const float exponent = (decay01_ - 0.5f) * 2.0f;  // [-1, 1]
        set_t60(base_t60_s_ * std::pow(3.0f, exponent));
    }
    float decay() const noexcept { return decay01_; }

    /// Brightness in [0, 1]. 0.5 is the calibrated band-pass centre
    /// (~2640 Hz). Exponential around the default (0 -> 0.5x, 1 -> 2x): higher
    /// values push the centre up and raise the spectral centroid. Maps to
    /// CowbellConfig::bandpass_hz.
    void set_tone(float tone01) noexcept {
        tone01_ = std::clamp(tone01, 0.0f, 1.0f);
        const float exponent = (tone01_ - 0.5f) * 2.0f;  // [-1, 1]
        cfg_.bandpass_hz = base_bandpass_hz_ * std::pow(2.0f, exponent);
        bandpass_.set_coefficients(signal::Biquad::Type::bandpass, cfg_.bandpass_hz,
                                   cfg_.bandpass_q,
                                   static_cast<float>(sample_rate_));
        bandpass_headroom_ = std::max(1.0f, cfg_.bandpass_q);
    }
    float tone() const noexcept { return tone01_; }

    void reset() noexcept {
        osc_.reset();
        bandpass_.reset();
        lowpass_.reset();
        env_ = 0.0f;
    }

    /// Strike at @p velocity (0..1). The oscillators free-run; only the
    /// envelope restarts.
    void trigger(float velocity) noexcept {
        env_ = std::clamp(velocity, 0.0f, 1.0f);
    }

    float process() noexcept {
        const float tone = osc_.process();
        const float shaped = lowpass_.process(bandpass_.process(tone));
        const float out = env_ * shaped;
        env_ = signal::snap_to_zero(env_ * decay_coeff_);
        return out;
    }

    /// Upper bound on |process()|. The two weighted squares sum to at most
    /// `gain * (low_amp + high_amp)`; the RBJ band-pass here is the 0 dB-peak
    /// form (peak magnitude response 1), and the following low-pass is <= 1, so
    /// unity would bound the steady state. The `max(1, Q)` headroom factor
    /// keeps the bound conservative through the filters' IIR transients. A test
    /// asserts the render stays under it (with room to spare).
    float peak_bound() const noexcept {
        return cfg_.gain * (std::abs(cfg_.low_amp) + std::abs(cfg_.high_amp)) *
               bandpass_headroom_;
    }

private:
    float coeff_for_t60(float t60_s) const noexcept {
        if (t60_s <= 0.0f) return 0.0f;
        return static_cast<float>(std::pow(10.0, -3.0 / (t60_s * sample_rate_)));
    }

    void apply_config() {
        std::vector<signal::SquareOscBank::Partial> ps = {
            {cfg_.low_hz, cfg_.low_amp}, {cfg_.high_hz, cfg_.high_amp}};
        osc_.set_partials(ps);
        osc_.set_level(cfg_.gain);
        bandpass_.set_coefficients(signal::Biquad::Type::bandpass, cfg_.bandpass_hz,
                                   cfg_.bandpass_q, static_cast<float>(sample_rate_));
        lowpass_.set_coefficients(signal::Biquad::Type::lowpass, cfg_.lowpass_hz,
                                  cfg_.lowpass_q, static_cast<float>(sample_rate_));
        // The RBJ band-pass here (b0 = alpha, b2 = -alpha) is the 0 dB-peak
        // form, so its steady-state peak magnitude is 1. Carry a small
        // conservative headroom for the cascaded IIR transients so
        // peak_bound() is never exceeded.
        bandpass_headroom_ = std::max(1.0f, cfg_.bandpass_q);
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);

        // Capture the freshly installed config as the calibration point for the
        // uniform controls, and re-centre their knobs so the defaults reproduce
        // exactly this sound.
        base_gain_ = cfg_.gain;
        base_low_hz_ = cfg_.low_hz;
        base_high_hz_ = cfg_.high_hz;
        base_t60_s_ = cfg_.t60_s;
        base_bandpass_hz_ = cfg_.bandpass_hz;
        level01_ = 1.0f;
        tune_ = 1.0f;
        decay01_ = 0.5f;
        tone01_ = 0.5f;
    }

    signal::SquareOscBank osc_{};
    signal::Biquad bandpass_{};
    signal::Biquad lowpass_{};

    double sample_rate_ = 48000.0;
    CowbellConfig cfg_{};

    float env_ = 0.0f;
    float decay_coeff_ = 0.0f;
    float bandpass_headroom_ = 1.0f;

    // Calibration reference for the uniform controls, captured from the active
    // config; the knobs scale relative to these so their defaults are neutral.
    float base_gain_ = 0.185f;
    float base_low_hz_ = 545.0f;
    float base_high_hz_ = 814.0f;
    float base_t60_s_ = 0.981f;
    float base_bandpass_hz_ = 2640.0f;

    // Uniform-control knob positions (neutral defaults).
    float level01_ = 1.0f;
    float tune_ = 1.0f;
    float decay01_ = 0.5f;
    float tone01_ = 0.5f;
};

/// Reference cowbell preset (48 kHz, v120): two squares at 545/814 Hz into a
/// band-pass, centroid ~2424 Hz, T60 ~0.981 s, peak ~0.196.
inline CowbellConfig cowbell_config() { return CowbellConfig{}; }

}  // namespace pulp::examples
