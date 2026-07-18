#pragma once

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// Component values of the TR-808 bass drum's bridged-T network, in SI units
/// (ohms, farads). The network is a Zobel bridged-T in the negative-feedback
/// path of an op-amp; its ringing is the entire bass drum sound.
///
/// Everything the resonator does is expressed in terms of these values rather
/// than abstract filter coefficients, so a component substitution ("what if
/// C41 were 22n?") is a legal, physically-meaningful edit rather than a
/// reverse-engineering exercise. That is the whole point of the topology.
///
/// Lineage: Werner, Abel & Smith, "A Physically-Informed, Circuit-Bendable,
/// Digital Model of the Roland TR-808 Bass Drum Circuit", DAFx-14, Fig. 1.
struct BridgedTComponents {
    double c41 = 15e-9;      ///< capacitive arm, op-amp inverting input side
    double c42 = 15e-9;      ///< capacitive arm, op-amp output side
    double r161 = 1.0e6;     ///< retrigger-pulse injection; also Q43's leakage path
    double r165 = 47.0e3;    ///< shorted out while the attack envelope grounds Q43
    double r166 = 6.8e3;     ///< Vcomm-to-collector leg of the branch
    double r167 = 1.0e6;     ///< the resistive bridge
    double r170 = 470.0e3;   ///< feedback-buffer injection
};

/// Softplus fit of Q43's collector current against Vcomm (DAFx-14 Eq. 8).
///
/// The paper obtains these by fitting SPICE-tabulated (Vcomm, iC) pairs, not
/// by closed-form device physics: the knee v0 lands ~one diode drop below
/// ground, which is where Q43's base starts to lift. The fit describes the
/// p-n junction in Q43 (a 2N3904), not D52.
struct Q43Leakage {
    double alpha = 14.3150;  ///< width of the transition region
    double v0 = -0.5560;     ///< voltage offset of the knee
    double m = 1.4765e-5;    ///< slope of the response below the knee
};

/// Q43's collector current for a given Vcomm (DAFx-14 Eq. 8): a negated
/// softplus, so iC is ~zero above the knee and turns linear below it. The
/// current is negative by this fit's sign convention -- it is drawn in.
///
/// Evaluated by the softplus's asymptotic branches rather than the literal
/// log1p(exp(e)). A hard strike drives Vcomm several volts negative, which puts
/// e well past the point where exp() overflows, and clamping e there would be
/// worse than the overflow it prevents: it freezes the leakage at whatever
/// current the clamp corresponds to, so R_e stops falling and turns back up
/// again -- the drum's pitch would bend the wrong way on its loudest hits. Both
/// branches below are exact to double precision at their thresholds, so the
/// function stays monotone over the whole rail-to-rail range.
inline double q43_collector_current(double vcomm, const Q43Leakage& leak) noexcept {
    const double e = -leak.alpha * (vcomm - leak.v0);
    const double scale = leak.m / leak.alpha;
    if (e > 30.0) return -e * scale;                 // log1p(exp(e)) == e here
    if (e < -30.0) return -std::exp(e) * scale;      // log1p(x) == x here
    return -std::log1p(std::exp(e)) * scale;
}

/// Resistance of the Vcomm-to-ground branch through R166, Q43's collector and
/// R165 (DAFx-14 Eq. 9, algebraically collapsed to one multiply-add and one
/// divide). With iC == 0 this is exactly R165 + R166; as Q43 leaks it falls,
/// which raises the bridged-T's centre frequency. That is the pitch sigh: it
/// emerges from the ring amplitude, and there is no pitch envelope anywhere.
///
/// Eq. 9's denominator vanishes at Vcomm ~ +17uV, and Vcomm crosses zero twice
/// per ring cycle, so samples land near that pole routinely rather than rarely.
/// The pole is an artifact of the fit, not of the circuit: Eq. 8's softplus
/// tail never quite reaches zero, so it reports ~0.36 nA of leakage at
/// Vcomm = 0 where a real Q43 is simply off. Eq. 9 then divides by that
/// fiction. Two defences, and both are needed:
///
///   * Above @p guard_v the branch is the series value outright. This is not an
///     approximation of Eq. 9 so much as a repair of it -- it discards a region
///     the fit was never valid in (leakage is a below-the-knee phenomenon, and
///     the knee is at -0.556 V). What it costs is the real correction near the
///     threshold: 0.195% of R_e at -10 mV, worth about 0.04 Hz of centre
///     frequency. What it buys is the removal of a pole that Eq. 9 approaches
///     through +/-infinity.
///   * The clamp is the second line of defence, against a re-fit of Eq. 8's
///     constants moving the pole. It cannot stand alone: raw values near the
///     pole go *negative*, and a negative value clamps to R166 -- which is the
///     attack tuning, so a clamp-only implementation emits spurious blips an
///     octave up rather than a glitch you would notice as a glitch.
inline double q43_branch_resistance(double vcomm,
                                    const BridgedTComponents& c,
                                    const Q43Leakage& leak,
                                    double guard_v = -0.01) noexcept {
    const double series = c.r165 + c.r166;
    if (vcomm > guard_v) return series;
    const double ic = q43_collector_current(vcomm, leak);
    const double den = vcomm + c.r165 * ic;
    if (den == 0.0) return series;
    return std::clamp(series * vcomm / den, c.r166, series);
}

/// Total Vcomm-to-ground resistance seen by the network: R161 || R_e || R170.
/// This single quantity sets both the centre frequency and the Q, which is why
/// it is the modulation target for the attack jump and the sigh alike.
inline double bridged_t_shunt_resistance(double r_e,
                                         const BridgedTComponents& c) noexcept {
    return 1.0 / (1.0 / c.r161 + 1.0 / c.r170 + 1.0 / r_e);
}

/// Centre frequency of the bridged-T for a given total shunt resistance.
inline double bridged_t_center_frequency(double r_eff,
                                         const BridgedTComponents& c) noexcept {
    constexpr double two_pi = 6.28318530717958647692;
    return 1.0 / (two_pi * std::sqrt(r_eff * c.r167 * c.c41 * c.c42));
}

/// Quality factor of the bridged-T network alone, with its three inputs
/// grounded. This is the bare network (~2.3 at nominal values), not the Q of
/// the ring you hear: the feedback buffer wraps this network in a regenerative
/// loop that raises the ringing Q by an order of magnitude, and the decay pot
/// is what sets it.
///
/// With C41 == C42 this collapses to sqrt(R167 / R_eff) / 2 -- independent of
/// the capacitor values. Scaling both arms therefore retunes the drum while
/// leaving Q exactly invariant, which is the signature a tune control has to
/// reproduce.
inline double bridged_t_q(double r_eff, const BridgedTComponents& c) noexcept {
    const double alpha2 = r_eff * c.r167 * c.c41 * c.c42;
    const double alpha1 = r_eff * (c.c41 + c.c42);
    return std::sqrt(alpha2) / alpha1;
}

/// The TR-808 bass drum's bridged-T network: one second-order system with
/// three inputs (pulse shaper, feedback buffer, retrigger pulse) and two
/// outputs (Vbt, the sound; Vcomm, the node that drives Q43's leakage).
///
/// Realized as a single trapezoidally-integrated two-state core whose states
/// are the physical capacitor voltages, NOT as the six transfer functions the
/// superposition analysis produces. The six share one characteristic
/// polynomial -- they are one system's output rows, not six filters -- and the
/// distinction is load-bearing rather than stylistic:
///
///   R_eff is recomputed every sample, at the ring frequency itself, from the
///   ring's own amplitude. Swapping the coefficients of a Direct-Form
///   realization mid-ring reinterprets state whose meaning depends on those
///   coefficients, which perturbs the stored energy; at deep decay settings
///   the network's dissipation is only ~3% per cycle, so that numerical
///   pumping wins and the drum latches into a limit cycle at the rails --
///   audibly, a note that never stops and sits at the wrong pitch. A capacitor
///   voltage is a continuous physical quantity, so a parameter change between
///   samples preserves it exactly and the loop decays at its linear rate under
///   the same modulation.
///
/// Direct-Form realizations remain correct for this circuit's fixed-coefficient
/// blocks (pulse shaper, feedback buffer, retrigger filter, output stage) --
/// the hazard is per-sample modulation, not the topology as such.
///
/// The caller drives the three inputs and reads both outputs; the shift logic
/// (Q43 leakage -> R_e) lives inside, since Vcomm never leaves the network in
/// the circuit either.
///
/// RT contract: prepare() clears state, all state is fixed-size, and process()
/// neither allocates nor locks.
class BridgedTResonator {
public:
    struct Output {
        double vbt = 0.0;    ///< op-amp output -- the bass drum signal
        double vcomm = 0.0;  ///< junction of R166, C41, C42 -- drives Q43
    };

    void set_components(const BridgedTComponents& c) noexcept { comps_ = c; }
    const BridgedTComponents& components() const noexcept { return comps_; }

    void set_leakage(const Q43Leakage& l) noexcept { leak_ = l; }
    const Q43Leakage& leakage() const noexcept { return leak_; }

    /// Cut the R161 leakage path, which is the pitch sigh's only mechanism.
    /// The paper lists disconnecting it as an architecture-level bend; it is
    /// also the control that proves the sigh is emergent rather than scripted.
    void set_sigh_enabled(bool enabled) noexcept { sigh_enabled_ = enabled; }
    bool sigh_enabled() const noexcept { return sigh_enabled_; }

    /// Supply rail, which the op-amp saturates against. Modelling the op-amp
    /// itself as linear is the paper's choice and is not a shortcut: the 808's
    /// character comes from its architecture, not from device nonlinearity.
    /// The clip is a bounded-output backstop, not a tone stage.
    void set_rail_voltage(double v) noexcept { rail_v_ = v; }

    void prepare(double sample_rate) noexcept {
        h_ = 1.0 / sample_rate;
        reset();
    }

    /// Clear the ring. Note the deliberate absence of any call to this from a
    /// trigger path: a trigger is an additive excitation into a resonator that
    /// is still ringing, and resetting here is exactly the machine-gun effect
    /// this model exists to avoid.
    void reset() noexcept {
        x1_ = 0.0;
        x2_ = 0.0;
        vcomm_delayed_ = 0.0;
        r_e_ = comps_.r165 + comps_.r166;
    }

    /// Ground Q43's collector, shorting out R165. The envelope generator holds
    /// this for a few ms after each trigger, which lifts the centre frequency
    /// by over an octave. This is an attack-timbre mechanism and is a separate
    /// circuit from the sigh -- the two are routinely conflated.
    void set_attack_shunt(bool active) noexcept { attack_shunt_ = active; }

    /// Advance one sample. Inputs are voltages at the three injection points.
    Output process(double v_plus, double v_fb, double v_rp) noexcept {
        // Shift logic reads a delayed Vcomm, which is what breaks the
        // Vcomm -> iC -> R_eff -> Vcomm loop into something computable.
        if (attack_shunt_) {
            r_e_ = comps_.r166;
        } else if (!sigh_enabled_) {
            r_e_ = comps_.r165 + comps_.r166;
        } else {
            r_e_ = q43_branch_resistance(vcomm_delayed_, comps_, leak_);
        }

        const double g = 1.0 / bridged_t_shunt_resistance(r_e_, comps_);

        // States are the capacitor voltages: x1 = v_C41 = V+ - Vcomm,
        // x2 = v_C42 = Vbt - Vcomm. Only a21 moves with R_eff.
        const double inv_r167_c41 = 1.0 / (comps_.r167 * comps_.c41);
        const double a11 = -inv_r167_c41;
        const double a12 = inv_r167_c41;
        const double a21 = (1.0 / comps_.r167 - g) / comps_.c42;
        const double a22 = -1.0 / (comps_.r167 * comps_.c42);
        const double u2 = (g * v_plus - v_rp / comps_.r161 - v_fb / comps_.r170) / comps_.c42;

        const double hh = 0.5 * h_;
        const double m11 = 1.0 - hh * a11;
        const double m12 = -hh * a12;
        const double m21 = -hh * a21;
        const double m22 = 1.0 - hh * a22;
        const double r1 = (1.0 + hh * a11) * x1_ + hh * a12 * x2_;
        const double r2 = hh * a21 * x1_ + (1.0 + hh * a22) * x2_ + h_ * u2;
        const double det = m11 * m22 - m12 * m21;

        x1_ = (m22 * r1 - m12 * r2) / det;
        x2_ = (m11 * r2 - m21 * r1) / det;

        Output out;
        out.vcomm = v_plus - x1_;
        out.vbt = std::clamp(out.vcomm + x2_, -rail_v_, rail_v_);
        vcomm_delayed_ = out.vcomm;
        return out;
    }

    /// Snap a settled ring to exact zero so a silent voice cannot park
    /// denormals in the recursion. Call at block boundaries, never per sample:
    /// the states carry the ring, and the threshold is far below any audible
    /// tail but is still a lie if applied often enough to matter.
    void snap_denormals() noexcept {
        x1_ = snap_to_zero(x1_);
        x2_ = snap_to_zero(x2_);
        vcomm_delayed_ = snap_to_zero(vcomm_delayed_);
    }

    /// Branch resistance in force on the most recent sample.
    double branch_resistance() const noexcept { return r_e_; }

    /// Centre frequency the network is tuned to right now. Tracks the ring
    /// amplitude while the sigh is active, which is the whole phenomenon.
    double center_frequency_hz() const noexcept {
        return bridged_t_center_frequency(bridged_t_shunt_resistance(r_e_, comps_), comps_);
    }

private:
    BridgedTComponents comps_{};
    Q43Leakage leak_{};
    double h_ = 1.0 / 48000.0;
    double rail_v_ = 15.0;
    bool sigh_enabled_ = true;
    bool attack_shunt_ = false;

    double x1_ = 0.0;
    double x2_ = 0.0;
    double vcomm_delayed_ = 0.0;
    double r_e_ = 47.0e3 + 6.8e3;
};

}  // namespace pulp::signal
