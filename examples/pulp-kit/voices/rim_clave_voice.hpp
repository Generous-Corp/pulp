#pragma once

// The kit's rimshot / clave voice: one short bridged-T ring, excited by a brief
// noise burst rather than a clean impulse. Rimshot and clave are the same pad on
// the reference machine (two MIDI notes map to one voice), and they share a
// circuit: a resonator pinged by the trigger and left to ring for a few tens of
// milliseconds. What separates a rimshot from a clave is only the resonator's
// pitch and how metallic-versus-woody the ring reads -- the tune control moves
// between them.
//
// Why noise excitation, not an impulse: a clean impulse into a two-pole
// resonator produces a pure decaying sinusoid, and the reference pad is not pure
// -- it is a bright, clicky, slightly noisy tick (measured spectral flatness
// ~0.3, well above a sinusoid's ~0). The trigger pulse in the real circuit has
// fast, noisy edges, and driving the bridged-T with a short burst of noise
// reproduces that: the resonator colours the noise into a tonal ring at its
// centre frequency (so the crossing rate still reads the pitch) while the burst
// spreads energy up the spectrum (so the onset is bright and broadband). The
// sound is still entirely the resonator's -- the noise is the strike, not the
// output.
//
// Calibration: the ring frequency (~850 Hz), its feedback (a very short ring),
// the excitation burst length and the output level are fit to the reference
// rim/clave pad's measured decay (~35 ms), crossing rate (~900 Hz), onset
// centroid (~3.1 kHz) and spectral flatness (~0.3). The tune knob then shifts
// the ring for a higher, woodier clave.
//
// RT contract: prepare() clears state; trigger() and process() allocate nothing
// and never lock. A trigger adds a fresh noise burst into the live ring without
// resetting it, so rapid re-hits superpose and draw decorrelated noise rather
// than machine-gunning.

#include "struck_bridged_t.hpp"

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/svf.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::examples {

namespace rim_clave_detail {

/// Deterministic white-noise source (xorshift32) in [-1, 1). Free-running from
/// prepare, so successive triggers draw decorrelated excitation.
struct WhiteNoise {
    std::uint32_t state = 0x9e3779b9u;
    void reset() noexcept { state = 0x9e3779b9u; }
    float next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(static_cast<std::int32_t>(state)) * (1.0f / 2147483648.0f);
    }
};

}  // namespace rim_clave_detail

class RimClaveVoice {
public:
    static constexpr double kRingHz = 850.0;         ///< reference rim/clave pad pitch.
    static constexpr double kExcitationLowpassHz = 10000.0;  ///< trims the burst's extreme highs.
    static constexpr double kExcitationT60 = 0.013;  ///< fixed noise-burst length, ~13 ms.
    static constexpr double kOutputGain = 0.103;     ///< lands a full-velocity hit at the reference peak.
    /// Common tune ratio that lifts the ring toward a bright ~2.5 kHz clave.
    static constexpr double kClaveTune = 2.9;

    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate;
        ring_.prepare(sample_rate);
        ring_.set_frequency(kRingHz * tune_);
        exc_lowpass_.set_sample_rate(static_cast<float>(sample_rate));
        exc_lowpass_.set_mode(signal::Svf::Mode::lowpass);
        exc_lowpass_.set_resonance(0.7f);
        apply_tone();
        const double tau = kExcitationT60 / 6.9077552789821;
        exc_decay_coeff_ = std::exp(-1.0 / (tau * sample_rate_));
        apply_decay();
        reset();
    }

    void reset() noexcept {
        ring_.reset();
        exc_lowpass_.reset();
        noise_.reset();
        exc_env_ = 0.0;
    }

    /// Strike the voice. @p velocity in [0, 1] scales the excitation burst, so
    /// peak level tracks velocity linearly.
    void trigger(double velocity) noexcept {
        exc_env_ = std::clamp(velocity, 0.0, 1.0);
    }

    /// Advance one sample.
    float process() noexcept {
        // The strike is a decaying burst of lowpassed noise fed into the
        // resonator; the resonator's ring is the sound.
        const double excitation = exc_lowpass_.process(noise_.next()) * exc_env_;
        exc_env_ = signal::snap_to_zero(exc_env_ * exc_decay_coeff_);
        ring_.strike(excitation);
        return static_cast<float>(signal::snap_to_zero(kOutputGain * level_ * ring_.process()));
    }

    /// Uniform kit control surface. The four setters below share one signature
    /// across every kit voice; the defaults set_level(1)/set_tune(1)/
    /// set_decay(0.5)/set_tone(0.5) reproduce the calibrated reference sound
    /// exactly, and each control only opens musical range around it.

    /// Output gain in [0, 1]. 1.0 is the calibrated reference loudness; the map
    /// is linear, so peak level scales directly with the knob (0.5 == half).
    void set_level(float level01) noexcept {
        level_ = std::clamp(static_cast<double>(level01), 0.0, 1.0);
    }
    double level() const noexcept { return level_; }

    /// Excitation brightness in [0, 1]. Sweeps the noise-burst lowpass +/- one
    /// octave around its calibrated 10 kHz (tone 0.5): a brighter setting lets
    /// more of the burst's highs into the ring and lifts the onset centroid, a
    /// darker one rolls them off. Maps to exc_lowpass_'s cutoff.
    void set_tone(float tone01) noexcept {
        const double t = std::clamp(static_cast<double>(tone01), 0.0, 1.0);
        if (t == tone_knob_) return;
        tone_knob_ = t;
        apply_tone();
    }
    double tone() const noexcept { return tone_knob_; }

    /// Decay knob in [0, 1], mapped to the ring's feedback and so to how long
    /// the tick rings out (~27..60 ms). The excitation burst length is fixed;
    /// this moves the resonator's own ring-down, which is what dominates the
    /// tail. Default (0.5) is the reference's ~35 ms.
    void set_decay(float decay01) noexcept {
        const double knob01 = std::clamp(static_cast<double>(decay01), 0.0, 1.0);
        if (knob01 == decay_knob_) return;
        decay_knob_ = knob01;
        apply_decay();
    }
    double decay() const noexcept { return decay_knob_; }

    /// Retune the ring. 1.0 is the reference rimshot; kClaveTune shifts it up to
    /// a woodier clave.
    void set_tune(float ratio) noexcept {
        const double r = std::clamp(static_cast<double>(ratio), 0.25, 4.0);
        if (r == tune_) return;
        tune_ = r;
        ring_.set_frequency(kRingHz * tune_);
    }
    double tune() const noexcept { return tune_; }

    void snap_denormals() noexcept {
        ring_.snap_denormals();
        exc_env_ = signal::snap_to_zero(exc_env_);
    }

private:
    void apply_decay() noexcept {
        // Ring feedback 0.64 .. 0.88, with the 0.5 default at 0.76 -- the
        // calibrated reference ring. Higher feedback holds the ring longer.
        ring_.set_feedback(0.64 + 0.24 * decay_knob_);
    }

    void apply_tone() noexcept {
        // tone 0.5 lands exactly on the calibrated 10 kHz excitation lowpass;
        // pow(2, 0) == 1, so the default is bit-exact. The knob then sweeps the
        // cutoff +/- one octave (5 kHz .. 20 kHz), clamped below Nyquist so the
        // SVF's tan(pi*f/sr) never blows up at high sample rates.
        const double octaves = (tone_knob_ - 0.5) * 2.0;
        double cutoff = kExcitationLowpassHz * std::pow(2.0, octaves);
        cutoff = std::min(cutoff, 0.45 * sample_rate_);
        exc_lowpass_.set_frequency(static_cast<float>(cutoff));
    }

    StruckBridgedT ring_{};
    signal::Svf exc_lowpass_{};
    rim_clave_detail::WhiteNoise noise_{};

    double sample_rate_ = 48000.0;
    double decay_knob_ = 0.5;
    double tune_ = 1.0;
    double tone_knob_ = 0.5;
    double level_ = 1.0;
    double exc_env_ = 0.0;
    double exc_decay_coeff_ = 0.0;
};

}  // namespace pulp::examples
