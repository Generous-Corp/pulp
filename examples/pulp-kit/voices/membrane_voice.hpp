#pragma once

// The tom / conga voice: the bass-drum's struck bridged-T circuit, tuned up.
//
// A tom is not a new circuit. It is the same Zobel bridged-T resonator that
// makes the bass drum -- struck by a pulse, its ringing wrapped in the same
// regenerative feedback buffer that lifts the bare network's Q into a long
// ring -- with its capacitive arms scaled down so the centre frequency lands
// in the low-mid ("membrane") band instead of the sub-bass. Everything that
// makes the bass-drum voice faithful (states are capacitor voltages, so a
// per-sample parameter change preserves stored energy; a trigger superposes
// onto whatever ring is in flight rather than restarting it) is inherited by
// reusing BassDrumVoice directly.
//
// What differs from the bass drum, and why:
//   * The tuning is set by substituting the bridged-T capacitors rather than by
//     the Tune pot, because the pot only spans +/-2 octaves around the nominal
//     49 Hz and a tom sits ~2.8 octaves above it. Scaling C41 == C42 moves the
//     centre frequency and leaves Q exactly invariant (see bridged_t_q), so the
//     drum keeps its character at the new pitch -- the same property the Tune
//     pot relies on, used over a wider range.
//   * The pitch sigh (Q43 leakage) is disabled. The bass drum's sigh is a
//     sub-bass-specific behaviour driven by the ring amplitude at the op-amp
//     rails; a struck tom reads as a pure tone (measured spectral flatness of
//     the reference tom pads is ~0.001, i.e. a clean body), so the leakage path
//     is cut and the body rings at a fixed pitch.
//   * The retrigger pulse and attack-frequency shunt are left off: both exist
//     to repair the sub-bass note's amplitude/pitch step and have no analogue
//     in the tom's short, clean strike.
//
// Lineage: the bridged-T resonator and BassDrumVoice this reuses derive from
// Werner, Abel & Smith, "A Physically-Informed, Circuit-Bendable, Digital Model
// of the Roland TR-808 Bass Drum Circuit", DAFx-14, and the published Service
// Notes schematic. The tom/conga family shares that bridged-T generator; the
// tuning band and per-preset decay below are calibrated to the measured output
// of the reference instrument (numbers only -- no reference audio is stored or
// replayed; every sample here is produced by the circuit model).

#include <va_drum_voice.hpp>

#include <pulp/signal/bridged_t_resonator.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples {

/// Nominal centre frequency of the bass-drum bridged-T at stock components,
/// from the schematic (49.44 Hz). Used only to derive the capacitor scaling
/// that retunes the network into the membrane band -- it is the pivot the tune
/// ratio multiplies, not a fitted value.
inline constexpr double kBassDrumNominalHz = 49.44;

/// Scale the bridged-T capacitive arms so the resonator's nominal centre
/// frequency becomes @p target_hz. Both arms scale together, which moves the
/// centre frequency as 1/scale and leaves Q untouched (Q depends only on the
/// resistances when C41 == C42). @p base are the arms to scale from -- pass the
/// stock bass-drum values to retune the reference network.
inline signal::BridgedTComponents membrane_components_for_hz(
    double target_hz, signal::BridgedTComponents base = {}) noexcept {
    const double scale = kBassDrumNominalHz / std::max(target_hz, 1.0);
    base.c41 *= scale;
    base.c42 *= scale;
    return base;
}

/// Decay-pot resistance for the membrane voice, substituted for the bass
/// drum's stock 500 k VR6.
///
/// This is the one component the tom needs that the bass drum does not, and it
/// is a physically-meaningful edit rather than a fudge. The feedback buffer
/// regenerates the ring with a high-frequency loop gain of VR6*k / (R164 +
/// VR6*k): with the stock 500 k pot this saturates near 0.9, which at ~49 Hz
/// gives a multi-second ring but at ~347 Hz -- seven times as many cycles per
/// second losing that same fraction each cycle -- caps the ring at T60 ~0.57 s,
/// about half the reference tom's ~1 s. A tom voice therefore runs a larger
/// decay pot so the loop can sit close enough to the oscillation threshold to
/// ring for a full second at the membrane pitch. 650 k puts the target decay
/// range (T60 ~0.8..1.4 s at 347 Hz) squarely on the usable side of the knob.
inline constexpr double kMembraneDecayPotOhms = 650e3;

/// Corner (Hz) and maximum gain of the output tone tilt driven by set_tone.
///
/// The circuit's own tone pot (VR5) is pinned to full body at prepare() and
/// only darkens as it opens, so it cannot brighten past the calibrated default.
/// The uniform Tone control instead runs a first-order presence tilt on the
/// voice output: out = x + g * (x - lowpass(x)), a fixed one-pole splitting the
/// signal at ~400 Hz -- just above the ~347 Hz body -- with the tilt gain g
/// lifting (bright) or cutting (dark) the band above it. That moves the spectral
/// centroid without touching the pitch. At the neutral setting (tone01 == 0.5)
/// g is exactly zero, so process() returns the bare resonator sample and the
/// calibrated default sound is reproduced bit-for-bit; the control only adds
/// brightness range around it.
inline constexpr double kMembraneToneTiltHz = 400.0;
inline constexpr double kMembraneToneTiltMaxGain = 0.9;

/// A struck bridged-T membrane voice (tom / conga). Wraps a BassDrumVoice tuned
/// into the membrane band, with the sub-bass-only mechanisms (pitch sigh,
/// retrigger pulse, attack shunt) disabled and the decay pot enlarged so the
/// ring reaches the reference tom's ~1 s length at the higher pitch.
///
/// RT contract: prepare() sets rates and allocates nothing beyond the fixed
/// members; trigger() and process() neither allocate nor lock. reset() is a
/// host-reset / prepare concern and is never called from a trigger -- a strike
/// superposes onto the ring in flight.
class MembraneVoice {
public:
    void prepare(double sample_rate) noexcept {
        core_.prepare(sample_rate);
        core_.set_sigh_enabled(false);       // clean tonal body, no pitch drop
        core_.set_retrigger_enabled(false);  // sub-bass amplitude repair only
        core_.set_attack_gate_s(0.0);        // no attack-frequency jump
        core_.set_tone(1.0);                 // full body through the tone stage
        VaDrumComponents comps = core_.components();
        comps.vr6 = kMembraneDecayPotOhms;   // enlarge the decay pot for the ring
        core_.set_components(comps);
        // One-pole tone-tilt splitter, corner just above the body. Rate-dependent
        // coefficient; the tilt gain itself comes from tone01_ via update_tone().
        constexpr double kPi = 3.14159265358979323846;
        tone_lp_a_ = 1.0 - std::exp(-2.0 * kPi * kMembraneToneTiltHz / sample_rate);
        tone_lp_z_ = 0.0;
        update_tone();
        apply_tuning();
        core_.set_decay(decay_);
        core_.set_level(level_);
    }

    void reset() noexcept {
        core_.reset();
        tone_lp_z_ = 0.0;
    }

    /// Set the body pitch in Hz. Retunes by substituting the bridged-T caps,
    /// then leaves the Tune pot at unity so fine tune (set_tune) still composes
    /// on top. Prepare-time / preset use.
    void set_frequency(double hz) noexcept {
        frequency_hz_ = std::clamp(hz, 40.0, 1200.0);
        apply_tuning();
    }
    double frequency() const noexcept { return frequency_hz_; }

    /// Uniform Tune: pitch multiplier around the body pitch (0.5..2.0). 1.0 is
    /// the calibrated pitch. Composes with set_frequency -- the resonator ends at
    /// frequency_hz_ * ratio -- and stays inside the Tune pot's +/-2 octaves.
    void set_tune(float ratio) noexcept {
        fine_tune_ = std::clamp(static_cast<double>(ratio), 0.5, 2.0);
        apply_tuning();
    }
    double tune() const noexcept { return fine_tune_; }

    /// Uniform Decay: ring length knob (0..1). 0.5 is the calibrated tail; higher
    /// is longer. Maps onto the feedback buffer's high-shelf attenuation --
    /// exactly the bass drum's Decay pot.
    void set_decay(float decay01) noexcept {
        decay_ = std::clamp(static_cast<double>(decay01), 0.0, 1.0);
        core_.set_decay(decay_);
    }
    double decay() const noexcept { return decay_; }

    /// Uniform Level: output gain (0..1). 1.0 is the calibrated loudness; the
    /// Level pot is linear in peak, so 0.5 halves the output.
    void set_level(float level01) noexcept {
        level_ = std::clamp(static_cast<double>(level01), 0.0, 1.0);
        core_.set_level(level_);
    }
    double level() const noexcept { return level_; }

    /// Uniform Tone: body brightness (0..1). 0.5 is the calibrated tone and
    /// renders the bare resonator (bit-for-bit default); above 0.5 lifts the band
    /// above the body to raise the spectral centroid, below 0.5 cuts it to darken.
    /// A first-order output presence tilt, so it moves brightness, not pitch.
    void set_tone(float tone01) noexcept {
        tone01_ = std::clamp(tone01, 0.0f, 1.0f);
        update_tone();
    }
    float tone() const noexcept { return tone01_; }

    /// Strike the membrane. @p accent_v is the trigger-pulse amplitude in volts
    /// (4..14 V over the accent range), the same excitation the bass drum takes.
    void trigger(double accent_v) noexcept { core_.trigger(accent_v); }

    double process() noexcept {
        const double x = core_.process();
        // Advance the tone-tilt splitter and apply the presence tilt. At the
        // neutral tone (tone01_ == 0.5) tone_tilt_g_ is exactly 0.0, so this
        // returns x unchanged -- the calibrated default renders bit-for-bit.
        tone_lp_z_ += tone_lp_a_ * (x - tone_lp_z_);
        return x + tone_tilt_g_ * (x - tone_lp_z_);
    }
    void snap_denormals() noexcept {
        core_.snap_denormals();
        tone_lp_z_ = signal::snap_to_zero(tone_lp_z_);
    }

    BassDrumVoice& core() noexcept { return core_; }
    const BassDrumVoice& core() const noexcept { return core_; }

private:
    void apply_tuning() noexcept {
        // Substitute the caps for the coarse body pitch, then let the Tune pot
        // carry the fine ratio. The pot clamps to +/-2 octaves, which the fine
        // ratio (0.5..2.0) stays inside, so the two compose cleanly.
        core_.set_bridged_t_components(
            membrane_components_for_hz(frequency_hz_, signal::BridgedTComponents{}));
        core_.set_tune(fine_tune_);
    }

    void update_tone() noexcept {
        // Symmetric tilt gain: 0 at the neutral 0.5, +/-max at the extremes.
        tone_tilt_g_ =
            (static_cast<double>(tone01_) - 0.5) * 2.0 * kMembraneToneTiltMaxGain;
    }

    BassDrumVoice core_{};
    double frequency_hz_ = 347.0;
    double fine_tune_ = 1.0;
    double decay_ = 0.5;
    double level_ = 1.0;

    // Output tone-tilt splitter (set_tone). tone_lp_a_ is the rate-dependent
    // one-pole coefficient; tone_lp_z_ its state; tone_tilt_g_ the presence gain.
    float tone01_ = 0.5f;
    double tone_lp_a_ = 0.0;
    double tone_lp_z_ = 0.0;
    double tone_tilt_g_ = 0.0;
};

// ---------------------------------------------------------------------------
// Reference calibration.
//
// The reference instrument exposes its tom/conga family as THREE pads that
// share one body spectrum (dominant ~347 Hz) and differ by decay and level, not
// by transposition -- a MIDI note selects a pad, it does not pitch the body.
// (Measured offline from the reference AU renders, byte-identical-render
// confirmed; only the numbers below are kept, never the audio.) These three
// presets place the voice on those measured pads.
//
// Measured targets (velocity-120 render, 48 kHz):
//   pad          dominant body   T60(s)   centroid(Hz)   rel. peak
//   A (lo/lo-mid)   347           1.043       418          0.017
//   B (mid)         347           1.182       415          0.020   (longest)
//   C (hi)          347 (+450)    0.981       508          0.028   (brightest, shortest)
//
// The single struck-bridged-T body reproduces the dominant ~347 Hz ring and its
// decay; the reference's weaker 78/185/450 Hz partials are a multi-mode
// membrane detail a one-resonator model does not chase. Pad C is voiced a touch
// higher (363 Hz, +4.6% -- inside the 7% body tolerance) to carry its measured
// brightness/pitch tilt (its ring sits above 347 Hz in the reference).
//
// The decay knobs below are the inverse of the voice's own T60(k) response at
// the 650 k decay pot, bisection-fit so each pad's rendered T60 lands within
// ~0.1% of its measured target; the levels are the reference peaks normalized
// to the loudest pad. Rendered at the reference definitions (48 kHz, one hit,
// T60 = last sample above peak/1000): TomA T60 1.044 s / f0 345 Hz, TomB 1.181 s
// / 345 Hz, TomC 0.981 s / 361 Hz.
// ---------------------------------------------------------------------------

enum class MembranePreset { TomA, TomB, TomC };

/// Per-pad calibration: body pitch (Hz), decay knob, and relative level.
struct MembraneCalibration {
    double frequency_hz;
    double decay;
    double level;
};

inline MembraneCalibration membrane_calibration(MembranePreset preset) noexcept {
    switch (preset) {
        case MembranePreset::TomA: return {347.0, 0.9035, 0.61};
        case MembranePreset::TomB: return {347.0, 0.9269, 0.71};
        case MembranePreset::TomC: return {363.0, 0.8999, 1.00};
    }
    return {347.0, 0.9035, 0.61};
}

/// Configure a prepared MembraneVoice onto one reference pad.
inline void apply_membrane_preset(MembraneVoice& v, MembranePreset preset) noexcept {
    const auto c = membrane_calibration(preset);
    v.set_frequency(c.frequency_hz);
    v.set_decay(c.decay);
    v.set_level(c.level);
}

}  // namespace pulp::examples
