#pragma once

// Tape character, standard tier — the CPU-friendly default.
//
// Mechanisms follow the published tape-modeling literature (Chowdhury 2019 for
// the model family; Wallace 1951 for why high frequencies die with speed and
// spacing; Uhlenbeck & Ornstein 1930 for the drift process), scoped to what an
// ECHO unit needs rather than to a full machine emulation. The physical tier
// (tape_physical.hpp) replaces this tier's saturation and loss stages with the
// actual physics; this one keeps the same audible outcomes at a fraction of
// the cost, and is what ships as the default realization.
//
// Two things carry the character:
//
//   * The EQ-bracketed saturator. Record EQ boosts the highs INTO the
//     nonlinearity and playback EQ cuts them after, so saturation works the
//     high frequencies harder than the lows — the published record/playback-EQ
//     rationale, and the reason tape distortion sounds like tape rather than
//     like a clipper. The two shelves are exact inverses (see FirstOrderShelf),
//     so with drive at zero the pair is transparent and adds no residual tilt
//     for the loop to compound.
//   * Wow and flutter. Both are pitch instability, but at different rates and
//     from different causes, so they are modeled separately: wow (published
//     band 0.5–6 Hz) as a periodic component plus an Ornstein-Uhlenbeck drift,
//     because a pure sine reads as an effect and pure noise reads as broken;
//     flutter (published band 5–100 Hz) as a small harmonic stack, because the
//     rotational components that cause it are harmonically related.
//
// Modulation is strictly zero-mean and the delay time is added on top, so the
// MEAN read position equals the requested delay: instability changes the pitch,
// never the tempo.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::chardelay {

/// Wow + flutter generator for one channel. Shared by both tape tiers.
class TapeInstability {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        control_rate_ = fs / static_cast<double>(kControlInterval);
        innovation_filter_.set_cutoff(kWowOuNoiseLpHz, control_rate_);
        reset();
    }

    /// `flutter_phase_offset` is in cycles; the right channel starts at a fixed
    /// nonzero fraction so the two channels wobble independently but
    /// deterministically.
    void configure(std::uint32_t seed, double flutter_phase_offset) noexcept {
        rng_.reseed(seed);
        flutter_phase_offset_ = flutter_phase_offset;
    }

    void reset() noexcept {
        rng_.reset();
        innovation_filter_.reset();
        wow_phase_ = 0.0;
        flutter_phase_ = 0.0;
        drift_ = 0.0;
        drift_previous_ = 0.0;
        control_counter_ = 0;
    }

    /// Advance one sample. Call exactly once per sample per channel, before
    /// reading offset_ms().
    void tick() noexcept {
        wow_phase_ += kWowRateHz / sample_rate_;
        if (wow_phase_ >= 1.0) wow_phase_ -= 1.0;
        flutter_phase_ += kFlutterBaseHz / sample_rate_;
        if (flutter_phase_ >= 1.0) flutter_phase_ -= 1.0;

        if (--control_counter_ <= 0) {
            control_counter_ = kControlInterval;
            step_drift();
        }
        drift_interpolation_ =
            1.0 - static_cast<double>(control_counter_) / static_cast<double>(kControlInterval);
    }

    /// Zero-mean read-position offset, in milliseconds.
    double offset_ms(double wow_depth_ms, double flutter_depth_ms) const noexcept {
        const double drift =
            drift_previous_ + drift_interpolation_ * (drift_ - drift_previous_);
        const double wow = wow_depth_ms * (std::cos(2.0 * kPi * wow_phase_) + drift);

        double flutter = 0.0;
        for (std::size_t k = 0; k < kFlutterAmplitudes.size(); ++k) {
            const double harmonic = static_cast<double>(k + 1);
            flutter += kFlutterAmplitudes[k] *
                       std::sin(2.0 * kPi * (harmonic * flutter_phase_ + kFlutterPhases[k] +
                                             flutter_phase_offset_));
        }
        return wow + flutter_depth_ms * flutter;
    }

private:
    /// Ornstein-Uhlenbeck mean-reverting walk, stepped at control rate and
    /// linearly interpolated between steps. Pre-filtering the Gaussian
    /// innovation keeps the drift organic rather than jittery — an unfiltered
    /// innovation at control rate reads as noise on the pitch, not as a
    /// transport wandering.
    void step_drift() noexcept {
        const double dt = static_cast<double>(kControlInterval) / sample_rate_;
        const double innovation = innovation_filter_.lowpass(rng_.gaussian());
        drift_previous_ = drift_;
        drift_ = flush_denormal(drift_ + kWowOuTheta * (0.0 - drift_) * dt +
                                kWowOuSigma * std::sqrt(dt) * innovation);
    }

    Xorshift32 rng_{kPrngSeed};
    OnePole innovation_filter_;
    double sample_rate_ = 48000.0;
    double control_rate_ = 1500.0;
    double wow_phase_ = 0.0;
    double flutter_phase_ = 0.0;
    double flutter_phase_offset_ = 0.0;
    double drift_ = 0.0;
    double drift_previous_ = 0.0;
    double drift_interpolation_ = 0.0;
    int control_counter_ = 0;
};

/// Standard-tier tape coloration for one channel: the pre-line record EQ +
/// saturation + playback EQ, and the post-line loss stand-in + DC blocker.
class TapeStandardChannel {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        const double emphasis_gain = std::pow(10.0, kTapeEmphasisDb / 20.0);
        record_eq_.set(kTapeEmphasisHz, emphasis_gain, fs);
        playback_eq_.set_inverse(kTapeEmphasisHz, emphasis_gain, fs);
        dc_blocker_.set_cutoff(kTapeDcBlockHz, fs);
        update(0.0);
        reset();
    }

    void reset() noexcept {
        record_eq_.reset();
        playback_eq_.reset();
        loss_filter_.reset();
        head_bump_.reset();
        dc_blocker_.reset();
    }

    void update(double character_amount) noexcept {
        drive_ = interpolate_knots(kTapeAxis, kTapeDrive, character_amount);
        bias_ = interpolate_knots(kTapeAxis, kTapeBias, character_amount);
        const double loss_hz = interpolate_knots(kTapeAxis, kTapeLossLpHz, character_amount);
        const double bump_db = interpolate_knots(kTapeAxis, kTapeBumpDb, character_amount);
        loss_filter_.set_cutoff(loss_hz, sample_rate_);
        head_bump_.set_bell(kTapeBumpHz, kTapeBumpQ, bump_db, sample_rate_);

        // Pre-computed denominator of the DC-compensated biased tanh.
        const double tanh_bias = std::tanh(bias_);
        compensation_ = drive_ * (1.0 - tanh_bias * tanh_bias);
        tanh_bias_ = tanh_bias;
    }

    /// Record EQ → saturation → playback EQ. Runs before the delay line.
    double pre_process(double x) noexcept {
        return playback_eq_.process(saturate(record_eq_.process(x)));
    }

    /// Loss stand-in + head bump + DC block. Runs after the delay line.
    double post_process(double x) noexcept {
        const double lossy = head_bump_.process(loss_filter_.lowpass(x));
        return dc_blocker_.highpass(lossy);
    }

private:
    /// tanh soft clipper with an operating-point offset for even harmonics,
    /// output-compensated to unity small-signal gain so raising the character
    /// macro changes the COLOR without also changing the loop's level (which
    /// would otherwise read as a feedback change).
    double saturate(double x) const noexcept {
        if (compensation_ <= 1e-12) return x;
        return (std::tanh(drive_ * x + bias_) - tanh_bias_) / compensation_;
    }

    FirstOrderShelf record_eq_;
    FirstOrderShelf playback_eq_;
    OnePole loss_filter_;
    Svf2 head_bump_;
    OnePole dc_blocker_;

    double sample_rate_ = 48000.0;
    double drive_ = 1.0;
    double bias_ = 0.0;
    double tanh_bias_ = 0.0;
    double compensation_ = 1.0;
};

}  // namespace pulp::signal::chardelay
