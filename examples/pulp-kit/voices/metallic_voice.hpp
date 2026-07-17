#pragma once

// The metallic 808 voices: closed hat, open hat, and cymbal.
//
// All three are the SAME circuit -- a cluster of free-running square-wave
// oscillators at fixed, inharmonic frequencies, summed, high-passed, and then
// gated by an amplitude envelope. The closed and open hats are identical but
// for the length of that envelope (the closed hat is a tick, the open hat
// rings). The cymbal shares the design and adds two lower partials plus a
// lower high-pass corner to darken its longer ring. All three are one class
// parameterised by a small config rather than three near-duplicate voices.
//
// The oscillators free-run; a trigger does NOT reset their phase. Only the
// envelope restarts. This matches the hardware -- the six Schmitt-trigger
// oscillators never stop -- and it means the metallic cluster is already in
// steady state when the envelope opens, so there is no per-hit filter
// transient, just a clean gated decay.
//
// The closed hat "chokes" the open hat: on a real machine both share the
// output VCA, so striking closed cuts an open hat still ringing. That is a
// relationship between two voices, not a property of one, so it is modelled as
// choke() -- a fast forced release the owning group calls on the open hat when
// the closed hat fires. See test_kit_metallic.cpp for the measured choke.
//
// Circuit lineage: the TR-808 cymbal/hi-hat section -- six square oscillators
// into a high-pass and an envelope VCA. Frequencies, high-pass corner, decay
// times and the small noise admixture here are calibrated to the *measured*
// output of the reference instrument (cluster peaks, spectral centroid and
// T60 per voice); the sound is still generated sample by sample by the
// oscillator cluster and filter below, never replayed from a sample.

#include "noise_source.hpp"

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/square_osc_bank.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace pulp::examples {

/// Everything that distinguishes one metallic voice from another.
///
/// The partials are the oscillator cluster (Hz); `highpass_hz`/`highpass_q`
/// set the high-pass that removes the squares' low body and leaves the
/// metallic top; `noise_level` is a small white admixture that fills the gaps
/// between the discrete partials (a pure osc cluster sounds too "pitched");
/// `t60_s` is the metallic cluster's 60 dB decay time; `gain` scales the
/// summed cluster so the voice's peak matches the reference level.
///
/// The noise "chiff" is transient -- it carries its own, much shorter
/// `noise_t60_s`. A struck hat opens with a broadband tick and then rings as a
/// pure metallic cluster, so the brightest energy is gone in the first tens of
/// milliseconds. That single mechanism is what makes a short note (closed hat)
/// read brighter than a long one (cymbal) even though the tone source is
/// identical: the long note spends most of its life as the darker cluster
/// alone, pulling its average spectral centroid down. A uniform VCA over both
/// noise and tone could not reproduce that measured trend.
struct MetallicConfig {
    /// Oscillator cluster as {frequency Hz, linear weight}. The cymbal weights
    /// its added low partials above the high cluster to darken its centroid.
    std::vector<signal::SquareOscBank::Partial> partials;
    float highpass_hz = 8000.0f;
    float highpass_q = 0.707f;
    float noise_level = 0.0f;
    float noise_t60_s = 0.05f;
    float t60_s = 0.3f;
    float gain = 0.12f;
};

/// One metallic voice. prepare() sizes everything; trigger()/choke()/process()
/// are audio-thread safe and allocate nothing.
class MetallicVoice {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        osc_.prepare(static_cast<float>(sample_rate_));
        apply_config();
        reset();
    }

    /// Install a voice preset. Allocates (resizes the oscillator bank); a
    /// prepare-time call, not an audio-thread call.
    void set_config(const MetallicConfig& cfg) {
        cfg_ = cfg;
        apply_config();
    }
    const MetallicConfig& config() const noexcept { return cfg_; }

    /// Retune the envelope's decay without touching the tone. Audio-thread safe.
    void set_t60(float t60_s) noexcept {
        cfg_.t60_s = t60_s;
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);
    }

    /// Master output gain applied to the whole voice (cluster and noise
    /// together), after the internal cluster normalization. Audio-thread safe.
    void set_gain(float gain) noexcept { cfg_.gain = gain; }

    // -- Uniform kit control surface -----------------------------------------
    //
    // The four normalized setters every kit voice exposes, mapped onto this
    // voice's existing config. Each moves exactly one audible axis and is
    // centered so the calibrated defaults -- set_level(1), set_tune(1),
    // set_decay(0.5), set_tone(0.5) -- reproduce the stock sound bit-for-bit
    // (each reduces to a multiply by exactly 1.0). All are audio-thread safe:
    // they recompute scalars and filter coefficients in place, never allocate
    // or lock. The base_* values are the calibration captured by apply_config,
    // so 1.0/0.5 always refers to the current preset, not a fixed constant.

    /// Output level. 0..1; 1.0 is the calibrated loudness, 0.0 silences.
    /// Scales the master gain, so peak scales linearly with `level01`.
    void set_level(float level01) noexcept {
        cfg_.gain = base_gain_ * std::clamp(level01, 0.0f, 1.0f);
    }

    /// Pitch. `ratio` multiplies every oscillator frequency; 1.0 is the
    /// calibrated pitch. Clamped to a musical [0.5, 1.5] so the top partial
    /// never crosses Nyquist. The high-pass corner is left fixed, so the move
    /// reads as pitch rather than tone.
    void set_tune(float ratio) noexcept {
        const float r = std::clamp(std::isfinite(ratio) ? ratio : 1.0f, 0.5f, 1.5f);
        const std::size_t n = osc_.size();
        for (std::size_t i = 0; i < n && i < cfg_.partials.size(); ++i)
            osc_.set_frequency(i, cfg_.partials[i].frequency * r);
    }

    /// Decay. 0..1; 0.5 is the calibrated tail. Scales t60 geometrically over
    /// a 4x range each way (base/4 .. base*4), so both extremes stay musical.
    void set_decay(float decay01) noexcept {
        const float d = std::clamp(decay01, 0.0f, 1.0f);
        set_t60(base_t60_ * std::pow(kDecayRange, (d - 0.5f) * 2.0f));
    }

    /// Tone/brightness. 0..1; 0.5 is the calibrated high-pass corner. Sweeps
    /// the corner geometrically over a 2.5x range each way: raising it strips
    /// the lower partials and lifts the spectral centroid, lowering it passes
    /// more low body and darkens. Audio-thread safe (recomputes coefficients
    /// in place, filter state is preserved).
    void set_tone(float tone01) noexcept {
        const float t = std::clamp(tone01, 0.0f, 1.0f);
        const float hp = base_highpass_hz_ * std::pow(kToneRange, (t - 0.5f) * 2.0f);
        highpass_.set_coefficients(signal::Biquad::Type::highpass, hp, cfg_.highpass_q,
                                   static_cast<float>(sample_rate_));
        cfg_.highpass_hz = hp;
    }

    /// Silence the voice. Host reset only, never a trigger.
    void reset() noexcept {
        osc_.reset();
        highpass_.reset();
        env_ = 0.0f;
        noise_env_ = 0.0f;
        active_coeff_ = decay_coeff_;
        noise_.reset();
    }

    /// Strike the voice at @p velocity (0..1). Restarts both the tone and the
    /// noise envelopes; the oscillator cluster keeps free-running, so
    /// successive hits interfere by phase rather than repeating identically.
    void trigger(float velocity) noexcept {
        const float v = std::clamp(velocity, 0.0f, 1.0f);
        env_ = v;
        noise_env_ = v;
        active_coeff_ = decay_coeff_;
    }

    /// Force a fast release -- the closed hat calls this on the open hat. The
    /// tone drops over `kChokeT60` instead of the voice's own decay, which
    /// mutes a ringing open hat in a few milliseconds.
    void choke() noexcept {
        active_coeff_ = choke_coeff_;
        noise_env_ = 0.0f;
    }

    /// Advance one sample. The metallic cluster rides the tone envelope; the
    /// broadband noise rides its own, shorter envelope, so the timbre darkens
    /// as the note rings out.
    float process() noexcept {
        // Gate each source by its own envelope, then share one high-pass, then
        // the master gain. The cluster rides the slow tone envelope; the noise
        // rides the fast one. The cluster is internally normalized to unit peak
        // so `gain` and `noise_level` set an absolute level and a mix.
        const float pre = env_ * osc_.process() +
                          noise_env_ * cfg_.noise_level * noise_.process();
        const float out = cfg_.gain * highpass_.process(pre);
        env_ = signal::snap_to_zero(env_ * active_coeff_);
        noise_env_ = signal::snap_to_zero(noise_env_ * noise_coeff_);
        return out;
    }

    /// Largest magnitude process() can return: master gain times the
    /// unit-normalized cluster's peak bound plus the noise admixture, with a
    /// unity envelope and the Butterworth high-pass's <= 1 pass-band gain. A
    /// test asserts the render never exceeds it.
    float peak_bound() const noexcept {
        return std::abs(cfg_.gain) * (osc_.peak_bound() + std::abs(cfg_.noise_level));
    }

    std::size_t partial_count() const noexcept { return osc_.size(); }
    float t60() const noexcept { return cfg_.t60_s; }

private:
    /// Per-sample multiplier that decays the envelope 60 dB over `t60_s`.
    float coeff_for_t60(float t60_s) const noexcept {
        if (t60_s <= 0.0f) return 0.0f;
        return static_cast<float>(std::pow(10.0, -3.0 / (t60_s * sample_rate_)));
    }

    void apply_config() {
        osc_.set_partials(cfg_.partials);
        // Normalize the cluster to unit peak bound so the master gain sets an
        // absolute level independent of the partial count and weights.
        float sum = 0.0f;
        for (const auto& p : cfg_.partials) sum += std::abs(p.amplitude);
        osc_.set_level(sum > 0.0f ? 1.0f / sum : 1.0f);
        highpass_.set_coefficients(signal::Biquad::Type::highpass, cfg_.highpass_hz,
                                   cfg_.highpass_q, static_cast<float>(sample_rate_));
        decay_coeff_ = coeff_for_t60(cfg_.t60_s);
        noise_coeff_ = coeff_for_t60(cfg_.noise_t60_s);
        choke_coeff_ = coeff_for_t60(kChokeT60);
        active_coeff_ = decay_coeff_;
        // Snapshot the calibrated defaults the uniform setters center on, so
        // set_level(1)/set_decay(0.5)/set_tone(0.5) always mean "this preset".
        base_gain_ = cfg_.gain;
        base_t60_ = cfg_.t60_s;
        base_highpass_hz_ = cfg_.highpass_hz;
    }

    static constexpr float kChokeT60 = 0.004f;  ///< choke release, ~4 ms
    static constexpr float kDecayRange = 4.0f;  ///< set_decay 0..1 spans base/4..base*4
    static constexpr float kToneRange = 2.5f;   ///< set_tone 0..1 spans base/2.5..base*2.5

    signal::SquareOscBank osc_{};
    signal::Biquad highpass_{};

    double sample_rate_ = 48000.0;
    MetallicConfig cfg_{};

    float env_ = 0.0f;
    float noise_env_ = 0.0f;
    float decay_coeff_ = 0.0f;
    float noise_coeff_ = 0.0f;
    float choke_coeff_ = 0.0f;
    float active_coeff_ = 0.0f;
    // Calibrated defaults the uniform control surface centers on (set by
    // apply_config from the installed preset).
    float base_gain_ = 0.12f;
    float base_t60_ = 0.3f;
    float base_highpass_hz_ = 8000.0f;
    WhiteNoise noise_{};
};

// ---------------------------------------------------------------------------
// Reference presets.
//
// The oscillator cluster is shared by all three voices; the closed and open
// hats use the same six partials and differ only in decay, while the cymbal
// adds two lower partials that pull its spectral centroid down. Frequencies,
// high-pass corner, noise level and decay times are the measured targets of
// the reference instrument (48 kHz, v120):
//
//   voice        T60 s   centroid Hz   cluster / added partials
//   closed hat   0.136   ~11700        6227 7266 8304 9750 11205 12456
//   open hat     0.433    ~9750        (same six)
//   cymbal       2.606    ~7750        (same six) + 3114 5227
//
// These are calibration constants for the control surface, not fitted audio.
// ---------------------------------------------------------------------------

/// The six-oscillator cluster (equal weights) shared by every metallic voice.
inline std::vector<signal::SquareOscBank::Partial> metallic_cluster() {
    return {{6227.0f, 1.0f}, {7266.0f, 1.0f}, {8304.0f, 1.0f},
            {9750.0f, 1.0f}, {11205.0f, 1.0f}, {12456.0f, 1.0f}};
}

inline MetallicConfig closed_hat_config() {
    MetallicConfig c;
    c.partials = metallic_cluster();
    c.highpass_hz = 8500.0f;
    c.highpass_q = 0.707f;
    c.noise_level = 0.35f;
    c.noise_t60_s = 0.045f;
    c.t60_s = 0.130f;
    c.gain = 0.178f;
    return c;
}

inline MetallicConfig open_hat_config() {
    MetallicConfig c = closed_hat_config();
    c.t60_s = 0.433f;
    return c;
}

inline MetallicConfig cymbal_config() {
    MetallicConfig c;
    // The cymbal's spectral centroid sits far below the hats' because its low
    // body partials (~3.1 kHz measured dominant) carry more energy than the
    // shared high cluster; weight them up rather than adding a filter.
    c.partials = {{3114.0f, 3.4f}, {5227.0f, 2.0f}, {6227.0f, 1.0f},
                  {7266.0f, 1.0f}, {8304.0f, 1.0f}, {9750.0f, 1.0f},
                  {11205.0f, 1.0f}, {12456.0f, 1.0f}};
    c.highpass_hz = 2500.0f;
    c.highpass_q = 0.707f;
    c.noise_level = 0.16f;
    c.noise_t60_s = 0.08f;
    c.t60_s = 2.606f;
    c.gain = 0.132f;
    return c;
}

}  // namespace pulp::examples
