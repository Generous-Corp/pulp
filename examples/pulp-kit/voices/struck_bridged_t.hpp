#pragma once

// A struck bridged-T resonator with a scalar regenerative feedback tap: the
// shared tone generator for every membrane/shell voice in the kit whose pitch
// is a single ringing mode (the snare's two drum bodies, the rimshot and clave
// clicks). It wraps pulp::signal::BridgedTResonator -- the same bridged-T core
// that is the bass drum's sound -- and adds the one element a shorter, brighter
// voice needs that the bass drum gets from its own feedback buffer: a way to
// set the ring's Q, and therefore its decay, independently of the pitch.
//
// Why the pitch and decay separate here, when the bare network cannot separate
// them: the bridged-T alone has a fixed Q (~2.3 at nominal values) that scaling
// its capacitive arms leaves untouched -- so cap-scaling retunes it but every
// tuning rings for the same handful of milliseconds. The bass drum's long ring
// comes from wrapping that network in the op-amp's regenerative feedback loop;
// the decay pot sets how much of the ring the loop returns. This helper is the
// minimal form of exactly that loop: one delayed, inverted feedback sample into
// the network's R170 injection point. The bridged-T sits in the op-amp's
// inverting path, so the returned copy must be inverted to regenerate rather
// than damp; a larger gain narrows the closed-loop bandwidth, raising the
// effective Q and lengthening the ring. Past a critical gain the loop
// self-oscillates, so the gain is clamped strictly below that threshold and
// this stays a decaying resonator.
//
// The sigh (the bass drum's amplitude-dependent pitch bend) is switched off:
// these voices want a clean, fixed-pitch ring, not the kick's downward glide.
// With the sigh off the network's branch resistance is constant and the whole
// thing is a linear time-invariant two-pole -- struck, it rings down at one
// rate.
//
// RT contract: prepare() and the setters recompute scalars only; strike() and
// process() allocate nothing and never lock. A strike is additive into the
// live ring (it never resets state), so repeated hits superpose and interfere
// rather than machine-gunning -- the same property the bass drum voice relies
// on.

#include <pulp/signal/bridged_t_resonator.hpp>
#include <pulp/signal/denormal.hpp>

#include <algorithm>

namespace pulp::examples {

class StruckBridgedT {
public:
    /// Centre frequency of the bridged-T at nominal component values with the
    /// sigh disabled (R161 || R170 || (R165+R166) = 46.05 kΩ). set_frequency()
    /// scales the capacitive arms relative to this. It is the schematic value
    /// the bass drum resonator's topology test also pins.
    static constexpr double kNominalFcHz = 49.44;

    /// Largest regeneration gain the helper will apply. The loop self-oscillates
    /// just below ~0.94 across the kit's frequency range; clamping here keeps the
    /// output a strictly decaying ring rather than a latched tone at the rail.
    static constexpr double kMaxFeedback = 0.93;

    void prepare(double sample_rate) noexcept {
        resonator_.prepare(sample_rate);
        resonator_.set_sigh_enabled(false);
        apply_frequency();
        reset();
    }

    /// Retune by scaling both capacitive arms. Q is invariant under this scaling
    /// (it depends only on the resistances when C41 == C42), so the ring keeps
    /// its character and only its pitch moves.
    void set_frequency(double hz) noexcept {
        hz = std::clamp(hz, 20.0, 12000.0);
        if (hz == freq_hz_) return;
        freq_hz_ = hz;
        apply_frequency();
    }
    double frequency() const noexcept { return freq_hz_; }

    /// Regeneration gain in [0, kMaxFeedback]. 0 is the bare network's fast
    /// ring; larger values lengthen the decay toward (but never reaching) the
    /// oscillation threshold. The map from this gain to a T60 is frequency
    /// dependent and steep near the top of the range, so voices calibrate it
    /// against measured decay rather than assuming a formula.
    void set_feedback(double g) noexcept {
        feedback_gain_ = std::clamp(g, 0.0, kMaxFeedback);
    }
    double feedback() const noexcept { return feedback_gain_; }

    /// Additively excite the ring. The amplitude is injected at the pulse-shaper
    /// input (V+) on the next sample and superposes onto whatever ring is still
    /// in flight -- deliberately no state reset, so retriggers interfere.
    void strike(double amplitude) noexcept { pending_ += amplitude; }

    /// Advance one sample and return the network output (the ring).
    double process() noexcept {
        const double v_plus = pending_;
        pending_ = 0.0;
        const auto out = resonator_.process(v_plus, feedback_state_, 0.0);
        feedback_state_ = -feedback_gain_ * out.vbt;
        return out.vbt;
    }

    /// Silence the ring. Called on prepare / host reset, never on a strike.
    void reset() noexcept {
        resonator_.reset();
        feedback_state_ = 0.0;
        pending_ = 0.0;
    }

    void snap_denormals() noexcept {
        resonator_.snap_denormals();
        feedback_state_ = signal::snap_to_zero(feedback_state_);
    }

    signal::BridgedTResonator& resonator() noexcept { return resonator_; }

private:
    void apply_frequency() noexcept {
        signal::BridgedTComponents c{};
        const double tune = freq_hz_ / kNominalFcHz;
        c.c41 /= tune;
        c.c42 /= tune;
        resonator_.set_components(c);
    }

    signal::BridgedTResonator resonator_{};
    double freq_hz_ = kNominalFcHz;
    double feedback_gain_ = 0.0;
    double feedback_state_ = 0.0;
    double pending_ = 0.0;
};

}  // namespace pulp::examples
