#pragma once

#include <algorithm>
#include <cmath>

namespace pulp::signal::drum {

/// How hard a hit changes the sound, beyond changing how loud it is.
///
/// Velocity that only scales amplitude is the single most common reason a
/// synthesised drum sounds synthetic. A real drum struck harder is not the
/// same sound turned up: the head deflects further so the pitch bends further
/// before settling, the stick excites more high partials, and the balance
/// between the body and the noise the strike makes shifts toward the noise.
/// Turning one knob cannot reproduce that, which is why this struct exists and
/// why every voice in this namespace consumes it rather than reading velocity
/// directly.
///
/// All four responses are zero-cost when left at their defaults except
/// `level_db`, so a voice that genuinely wants level-only velocity says so by
/// leaving the others at zero rather than by omitting the wiring.
///
/// **There is deliberately no decay term.** Velocity must not shorten a note:
/// a soft hit is a shallower swoop of the same length, not a shorter one. A
/// voice that genuinely wants velocity-dependent decay has to expose that as
/// its own explicit parameter, so it is a stated design choice rather than
/// something that arrives as a side effect of this struct growing a field.
///
/// Bend depths are meant to convey tension, not portamento: a fraction of an
/// octave is the house default, and the one voice that goes to 1.5 octaves
/// (the Simmons-style tom) is being extreme on purpose, because the laser
/// sweep *is* that instrument.
struct VelocityResponse {
    /// Attenuation at velocity 0, in decibels below a full-velocity hit.
    float level_db = 18.0f;

    /// Extra pitch-envelope depth at full velocity, in octaves. A harder
    /// strike deflects the head further, so the pitch starts higher and falls
    /// through a wider range.
    float bend_octaves = 0.0f;

    /// Extra exciter brightness at full velocity, in octaves of filter corner.
    float brightness_octaves = 0.0f;

    /// Shift toward the noise layer at full velocity, 0 to 1. A harder strike
    /// puts proportionally more energy into the transient than into the body.
    float noise_balance = 0.0f;

    /// Amplitude multiplier for `velocity` in [0, 1].
    float gain(float velocity) const {
        const float v = std::clamp(velocity, 0.0f, 1.0f);
        return std::pow(10.0f, (level_db * (v - 1.0f)) / 20.0f);
    }

    /// Pitch-envelope depth to add, in octaves.
    float bend(float velocity) const {
        return bend_octaves * std::clamp(velocity, 0.0f, 1.0f);
    }

    /// Multiplier to apply to an exciter or filter frequency.
    float brightness_scale(float velocity) const {
        return std::exp2(brightness_octaves * std::clamp(velocity, 0.0f, 1.0f));
    }

    /// Amount to add to a body-versus-noise balance in [0, 1].
    float noise_shift(float velocity) const {
        return noise_balance * std::clamp(velocity, 0.0f, 1.0f);
    }
};

}  // namespace pulp::signal::drum
