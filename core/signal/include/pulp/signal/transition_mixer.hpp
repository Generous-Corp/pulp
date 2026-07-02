#pragma once

/// @file transition_mixer.hpp
/// Shared click-free crossfade primitive (live-swap plan item 2.1).
///
/// The one place the live-swap machinery computes old→new blend gains, so the
/// DSP hot-swap slot, the convolver IR swapper, and the Phase-2 SwapUnit
/// transitions all fade identically and are tested by one fixture. Pure math +
/// a sample position; no allocation, no locks — safe to advance on the audio
/// thread.
///
/// Both laws are evaluated over a smoothstep ramp (zero slope at both ends), so
/// both are click-free at the swap instant AND the fade end; they differ only in
/// the mid-fade sum:
///   - `Smoothstep` — equal-GAIN (old + new == 1). No power bump for correlated
///     old/new (the usual hot-reload: same input, a small DSP tweak). Default.
///   - `EqualPower` — constant POWER (old² + new² == 1), cos/sin. Avoids the
///     mid-fade level dip when old/new are decorrelated (a big DSP change).
///     Mirrors `DryWetMixer`'s EqualPower law.

#include <cmath>
#include <cstddef>

namespace pulp::signal {

enum class TransitionCurve { Smoothstep, EqualPower };

class TransitionMixer {
public:
    TransitionMixer() = default;

    /// Configure the fade length (samples) + curve and rewind to the start.
    /// length 0 means "no fade" (done() is immediately true).
    void configure(std::size_t length_samples, TransitionCurve curve) {
        length_ = length_samples;
        curve_ = curve;
        pos_ = 0;
    }
    void set_curve(TransitionCurve curve) { curve_ = curve; }
    TransitionCurve curve() const { return curve_; }

    void reset() { pos_ = 0; }
    void advance(std::size_t frames) { pos_ += frames; }

    std::size_t length() const { return length_; }
    std::size_t position() const { return pos_; }
    bool done() const { return length_ == 0 || pos_ >= length_; }

    /// Old/new blend gains at an ABSOLUTE fade position (pure — no state change),
    /// so a caller can compute per-sample gains as `gains_at(position()+n, ...)`.
    /// Evaluated over the smoothstep ramp: click-free ends for both curves.
    void gains_at(std::size_t fade_pos, float& old_gain, float& new_gain) const {
        float t = (length_ == 0) ? 1.0f
                                 : static_cast<float>(fade_pos) / static_cast<float>(length_);
        if (t > 1.0f) t = 1.0f;
        const float ramp = t * t * (3.0f - 2.0f * t);   // smoothstep: 0 slope at 0 and 1
        if (curve_ == TransitionCurve::EqualPower) {
            constexpr float kHalfPi = 1.57079632679489661923f;
            const float theta = ramp * kHalfPi;
            old_gain = std::cos(theta);                 // old²+new² == 1
            new_gain = std::sin(theta);
        } else {
            old_gain = 1.0f - ramp;                     // old+new == 1
            new_gain = ramp;
        }
    }

private:
    std::size_t length_ = 0;
    std::size_t pos_ = 0;
    TransitionCurve curve_ = TransitionCurve::Smoothstep;
};

}  // namespace pulp::signal
