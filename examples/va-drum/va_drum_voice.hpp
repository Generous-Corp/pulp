#pragma once

// The TR-808 bass drum voice: a struck resonator, not an oscillator.
//
// A 1 ms trigger pulse kicks a bridged-T bandpass and its ringing is the entire
// sound. Everything else here shapes the strike or the ring-down. Blocks are cut
// where high-impedance stages are driven by low-impedance ones, so each solves
// independently -- there is no global iterative solve anywhere in this circuit,
// and none of the nonlinearities need one either.
//
// Lineage: Werner, Abel & Smith, "A Physically-Informed, Circuit-Bendable,
// Digital Model of the Roland TR-808 Bass Drum Circuit", DAFx-14, and the
// published TR-808 Service Notes schematic its Fig. 1 is adapted from. Derived
// from those equations and that schematic only.

#include <pulp/signal/bridged_t_resonator.hpp>
#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::examples {

/// Component values outside the bridged-T proper, in SI units. Pot values are
/// the track maxima; knob positions scale them.
struct VaDrumComponents {
    double c39 = 0.033e-6;  ///< retrigger pulse high-pass
    double c40 = 0.015e-6;  ///< pulse shaper
    double c43 = 33e-6;     ///< feedback buffer
    double c45 = 0.1e-6;    ///< tone stage
    double c47 = 0.47e-6;   ///< level stage
    double c49 = 0.47e-6;   ///< output stage
    double r162 = 4.7e3;
    double r163 = 100e3;
    double r164 = 47e3;   ///< feedback buffer input; sets its DC gain with R169
    double r169 = 47e3;
    double r171 = 220.0;
    double r172 = 10e3;
    double r176 = 100e3;
    double r177 = 82e3;

    /// Level pot. The paper's Fig. 1 names VR4 here but prints no value; it
    /// only positions a sub-audible high-pass corner (3.4 Hz at this value),
    /// which the paper itself calls inaudible, so the choice is not load-bearing.
    double vr4 = 100e3;
    double vr5 = 10e3;   ///< tone
    double vr6 = 500e3;  ///< decay

    double rail_v = 15.0;  ///< +/-B2

    /// Height of Venv at Q42's collector, which drives the retrigger pulse.
    /// The paper gives the envelope generator no equations at all -- only the
    /// prose that Venv "swings quickly up" and settles ~5 ms after the trigger
    /// falls. A saturated switch pulling toward B2 is the reading taken here.
    /// The model is largely insensitive to it: the retrigger pulse's falling
    /// edge, which is the edge that does the work, is clamped by D52 to one
    /// diode drop regardless of this height.
    double env_high_v = 15.0;
};

/// One first-order section, H(s) = (b1*s + b0) / (a1*s + a0), bilinear-mapped
/// and run transposed direct form II.
///
/// Direct form is the right realization for every block here that holds its
/// coefficients still. The bridged-T is the sole exception and does not use
/// this type -- see BridgedTResonator on why per-sample modulation needs
/// physical state.
class OnePoleSection {
public:
    /// @p c is the bilinear constant 2/T. The paper uses it untuned, with no
    /// frequency warping compensation, because every feature of this circuit
    /// sits far below the sample rate.
    void set_analog(double b1, double b0, double a1, double a0, double c) noexcept {
        const double d = a0 + a1 * c;
        b0_ = (b0 + b1 * c) / d;
        b1_ = (b0 - b1 * c) / d;
        a1_ = (a0 - a1 * c) / d;
    }

    double process(double x) noexcept {
        const double y = b0_ * x + s1_;
        s1_ = b1_ * x - a1_ * y;
        return y;
    }

    void reset() noexcept { s1_ = 0.0; }
    void snap_denormals() noexcept { s1_ = signal::snap_to_zero(s1_); }

private:
    double b0_ = 1.0, b1_ = 0.0, a1_ = 0.0;
    double s1_ = 0.0;
};

/// The diode's memoryless curve (DAFx-14 Eq. 4). Positive voltages pass
/// untouched; negative ones floor at one diode drop.
///
/// This is the trick that keeps the circuit real-time. Nodal analysis of the
/// pulse shaper yields an implicit nonlinear ODE whose unknown sits inside an
/// exponential -- a Newton iteration per sample. Instead the diode is dropped
/// from the ODE (leaving a plain passive shelf with a closed-form transfer
/// function) and reinjected afterwards as this static curve. The implicit term
/// is removed structurally rather than solved numerically, so the per-sample
/// cost is bounded and data-independent.
inline double diode_shaper(double v) noexcept {
    if (v >= 0.0) return v;
    return 0.71 * (std::exp(std::max(v, -50.0)) - 1.0);
}

/// The complete bass drum voice: trigger logic, pulse shaper, envelope
/// generator, retrigger pulse, bridged-T resonator, feedback buffer, and the
/// tone/level/output stages.
///
/// Monophonic by construction, and that is a requirement rather than a
/// limitation. There is one resonator and it is always ringing; trigger()
/// injects into whatever state it finds. Allocating a voice per hit, or
/// clearing state on note-on, produces identical successive notes -- the
/// machine-gun effect this model exists to avoid.
///
/// RT contract: prepare() sets rates, all state is fixed-size, and neither
/// trigger() nor process() allocates or locks.
class BassDrumVoice {
public:
    void prepare(double sample_rate) noexcept {
        sample_rate_ = sample_rate;
        bilinear_c_ = 2.0 * sample_rate;
        resonator_.prepare(sample_rate);
        resonator_.set_rail_voltage(comps_.rail_v);
        apply_tune();
        update_pulse_shaper();
        update_feedback();
        update_retrigger();
        update_output();
        reset();
    }

    /// Silence the instrument. Called on host reset, never on a trigger.
    void reset() noexcept {
        resonator_.reset();
        pulse_shaper_.reset();
        feedback_.reset();
        retrigger_.reset();
        tone_.reset();
        level_stage_.reset();
        out_hp_.reset();
        v_fb_delayed_ = 0.0;
        pulse_remaining_ = 0;
        env_remaining_ = 0;
        accent_v_ = 0.0;
    }

    void set_components(const VaDrumComponents& c) noexcept {
        comps_ = c;
        resonator_.set_rail_voltage(c.rail_v);
        update_pulse_shaper();
        update_feedback();
        update_retrigger();
        update_output();
    }
    const VaDrumComponents& components() const noexcept { return comps_; }

    /// Substitute bridged-T components. These are the untuned values: the tune
    /// control scales the capacitive arms on top of whatever is set here, so
    /// bending a component and turning the knob compose rather than fight.
    void set_bridged_t_components(const signal::BridgedTComponents& c) noexcept {
        bt_nominal_ = c;
        apply_tune();
        update_retrigger();
    }
    const signal::BridgedTComponents& bridged_t_components() const noexcept {
        return bt_nominal_;
    }

    /// Retune by scaling both capacitive arms. With C41 == C42 the bridged-T's
    /// Q depends only on the resistances, so scaling the arms moves the centre
    /// frequency and leaves Q exactly where it was -- decay rate tracks pitch
    /// and the drum keeps its character across the range. Substituting R165 or
    /// R166 instead would retune it too, but would drag Q along.
    void set_tune(double ratio) noexcept {
        ratio = std::clamp(ratio, 0.25, 4.0);
        if (ratio == tune_) return;
        tune_ = ratio;
        apply_tune();
    }
    double tune() const noexcept { return tune_; }

    /// VR6, the decay pot: it reshapes the feedback buffer's high shelf, and
    /// less attenuation there means a longer ring.
    void set_decay(double k) noexcept {
        k = std::clamp(k, 0.0, 1.0);
        if (k == decay_k_) return;
        decay_k_ = k;
        update_feedback();
    }
    double decay() const noexcept { return decay_k_; }

    /// VR5, the tone pot.
    void set_tone(double l) noexcept {
        l = std::clamp(l, 0.0, 1.0);
        if (l == tone_l_) return;
        tone_l_ = l;
        update_output();
    }

    /// VR4, the level pot. The circuit's wiper fraction attenuates as it rises,
    /// so this maps 1.0 to full output.
    void set_level(double level) noexcept {
        level = std::clamp(level, 0.0, 1.0);
        if (level == level_) return;
        level_ = level;
        update_output();
    }

    /// Cut the R161 leakage path, the pitch sigh's only mechanism.
    void set_sigh_enabled(bool enabled) noexcept { resonator_.set_sigh_enabled(enabled); }

    /// Width of the trigger pulse. The paper lists this among its bends.
    void set_pulse_width_s(double seconds) noexcept {
        pulse_width_s_ = std::clamp(seconds, 1e-5, 0.1);
    }

    /// How long the envelope generator grounds Q43's collector, holding the
    /// centre frequency over an octave up.
    ///
    /// Set as a duration rather than derived from C38, which is what moves it
    /// in hardware, because the envelope generator is the one block the paper
    /// describes only in prose -- it publishes no transfer function for it, and
    /// inventing one would be guessing dressed as physics. The prose bound is
    /// that Venv settles roughly 5 ms after the trigger falls.
    void set_attack_gate_s(double seconds) noexcept {
        attack_gate_s_ = std::clamp(seconds, 0.0, 0.1);
    }

    /// Silence the retrigger pulse path (C39/R161/D52).
    void set_retrigger_enabled(bool enabled) noexcept { retrigger_enabled_ = enabled; }

    /// Strike the drum. @p accent_v is the trigger pulse amplitude, 4 V to 14 V
    /// across the accent range.
    ///
    /// Accent is the strike force at the input of a nonlinear system, not a
    /// gain at its output, and the difference is audible rather than academic:
    /// the pulse shaper's rising edge scales with this amplitude while its
    /// falling edge is pinned at one diode drop regardless. Accent therefore
    /// changes the *ratio* of the two edges that kick the resonator, so a hard
    /// hit is a different timbre and a different pitch trajectory -- not a loud
    /// soft hit.
    ///
    /// Deliberately does not touch the resonator: the new excitation superposes
    /// onto whatever ring is still in flight, interfering constructively or
    /// destructively by phase, so no two hits land the same. The envelope
    /// generator and retrigger pulse re-fire independently of that ring, which
    /// is what the counter resets below are.
    void trigger(double accent_v) noexcept {
        accent_v_ = accent_v;
        pulse_remaining_ = static_cast<int>(pulse_width_s_ * sample_rate_);
        env_remaining_ = static_cast<int>(attack_gate_s_ * sample_rate_);
    }

    /// Advance one sample, returning the voltage at the output buffer.
    double process() noexcept {
        const double v_trig = pulse_remaining_ > 0 ? accent_v_ : 0.0;
        const bool env_high = env_remaining_ > 0;
        if (pulse_remaining_ > 0) --pulse_remaining_;
        if (env_remaining_ > 0) --env_remaining_;

        // While Q42's collector is high, current into Q43's base grounds its
        // collector and shorts R165 out.
        resonator_.set_attack_shunt(env_high);

        const double v_plus = diode_shaper(pulse_shaper_.process(v_trig));

        // The retrigger pulse exists because energy at the normal centre
        // frequency has already been attenuated by the time the attack shift
        // ends; without it the note steps in amplitude when the frequency
        // drops back. Venv is treated as an ideal source and Vcomm as ground,
        // which leaves C39/R161/D52 as a high-pass into a diode clipper.
        const double v_env = env_high ? comps_.env_high_v : 0.0;
        const double v_rp = retrigger_enabled_
                                ? diode_shaper(retrigger_.process(v_env))
                                : 0.0;

        const auto bt = resonator_.process(v_plus, v_fb_delayed_, v_rp);

        // The bridged-T and feedback buffer form a delay-free loop. One unit
        // delay breaks it, which the paper allows because every feature of this
        // circuit is far below the sample rate.
        v_fb_delayed_ = std::clamp(feedback_.process(bt.vbt), -comps_.rail_v, comps_.rail_v);

        return out_hp_.process(level_stage_.process(tone_.process(bt.vbt)));
    }

    /// Snap settled state to zero at a block boundary so a silent voice cannot
    /// park denormals in the recursions.
    void snap_denormals() noexcept {
        resonator_.snap_denormals();
        pulse_shaper_.snap_denormals();
        feedback_.snap_denormals();
        retrigger_.snap_denormals();
        tone_.snap_denormals();
        level_stage_.snap_denormals();
        out_hp_.snap_denormals();
        v_fb_delayed_ = signal::snap_to_zero(v_fb_delayed_);
    }

    signal::BridgedTResonator& resonator() noexcept { return resonator_; }
    const signal::BridgedTResonator& resonator() const noexcept { return resonator_; }

private:
    void apply_tune() noexcept {
        auto bt = bt_nominal_;
        bt.c41 = bt_nominal_.c41 / tune_;
        bt.c42 = bt_nominal_.c42 / tune_;
        resonator_.set_components(bt);
    }

    void update_pulse_shaper() noexcept {
        // Passive low shelf: DC gain R162/(R162+R163) = 0.045, unity at HF --
        // it passes the edges and shaves the body, and it is the edges that
        // kick the resonator.
        const double rc = comps_.r162 * comps_.r163 * comps_.c40;
        pulse_shaper_.set_analog(rc, comps_.r162, rc, comps_.r162 + comps_.r163, bilinear_c_);
    }

    void update_feedback() noexcept {
        const double vr = comps_.vr6 * decay_k_;
        // Inverting stage: DC gain -R169/R164 = -1, HF gain -VR6*k/R164, so
        // the knob sets high-frequency attenuation and thus the ring's decay.
        feedback_.set_analog(-comps_.r169 * vr * comps_.c43,
                             -comps_.r169,
                             comps_.r164 * (comps_.r169 + vr) * comps_.c43,
                             comps_.r164,
                             bilinear_c_);
    }

    void update_retrigger() noexcept {
        const double rc = bt_nominal_.r161 * comps_.c39;
        retrigger_.set_analog(rc, 0.0, rc, 1.0, bilinear_c_);
    }

    void update_output() noexcept {
        const double vr5 = comps_.vr5 * tone_l_;
        const double req = comps_.r171 + (comps_.r172 * vr5) / (comps_.r172 + vr5);
        tone_.set_analog(0.0, 1.0, req * comps_.c45, 1.0, bilinear_c_);

        const double m = 1.0 - level_;
        level_stage_.set_analog(comps_.vr4 * (1.0 - m) * comps_.c47, 0.0,
                                comps_.vr4 * comps_.c47, 1.0, bilinear_c_);

        out_hp_.set_analog(comps_.r177 * comps_.c49, 0.0,
                           comps_.r176 * comps_.c49, 1.0, bilinear_c_);
    }

    VaDrumComponents comps_{};
    signal::BridgedTComponents bt_nominal_{};
    signal::BridgedTResonator resonator_{};

    OnePoleSection pulse_shaper_{};
    OnePoleSection feedback_{};
    OnePoleSection retrigger_{};
    OnePoleSection tone_{};
    OnePoleSection level_stage_{};
    OnePoleSection out_hp_{};

    double sample_rate_ = 48000.0;
    double bilinear_c_ = 96000.0;

    double tune_ = 1.0;
    double decay_k_ = 0.5;
    double tone_l_ = 0.5;
    double level_ = 1.0;
    double pulse_width_s_ = 1e-3;
    double attack_gate_s_ = 5.4e-3;
    bool retrigger_enabled_ = true;

    double v_fb_delayed_ = 0.0;
    double accent_v_ = 0.0;
    int pulse_remaining_ = 0;
    int env_remaining_ = 0;
};

// ---------------------------------------------------------------------------
// Reference calibration.
//
// The bridged-T core is circuit-faithful on its own terms: at nominal component
// values its centre frequency is the schematic's 49.44 Hz. These two constants
// do not touch that -- they place the *default knob positions* on the measured
// pitch and decay curve of the TR-808 hardware, so the stock sound lands on the
// reference rather than on the raw nominal. They are a calibration of the
// control surface, not of the physics: every sample is still produced by the
// resonator, and the values below are the measured mapping, not fitted audio.
//
// Reference targets (48 kHz, one hit): the Decay knob at 0/64/128/192/255 of
// 255 gives T60 = {0.296, 0.437, 0.907, 1.535, 2.286} s and a tail pitch near
// 48 Hz. These are measured constants from the reference instrument; no
// reference audio is stored.
// ---------------------------------------------------------------------------

/// Scales the Tune ratio so the default knob (1.0) lands on the reference's
/// tail pitch (~48 Hz) rather than the raw nominal 49.44 Hz.
inline constexpr double kReferenceTuneTrim = 0.966;

/// Maps the normalized Decay knob (0..1) to the resonator's internal feedback
/// coefficient so the five reference knob positions land on the measured T60
/// curve above. Piecewise-linear inverse of the voice's own T60(k) response,
/// sampled at the reference Tune trim; monotonic, so the knob stays smooth.
inline double reference_decay_taper(double knob01) noexcept {
    static constexpr double kn[5] = {0.0, 0.25, 0.5, 0.75, 1.0};
    static constexpr double kk[5] = {0.1532, 0.2498, 0.4826, 0.6827, 0.8368};
    knob01 = std::clamp(knob01, 0.0, 1.0);
    for (int i = 0; i < 4; ++i) {
        if (knob01 <= kn[i + 1]) {
            const double frac = (knob01 - kn[i]) / (kn[i + 1] - kn[i]);
            return kk[i] + (kk[i + 1] - kk[i]) * frac;
        }
    }
    return kk[4];
}

}  // namespace pulp::examples
