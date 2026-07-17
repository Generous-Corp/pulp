#pragma once

// BowedString -- a McIntyre-Schumacher-Woodhouse (MSW, 1983) digital-waveguide
// bowed string. The string is split at the bow into a bridge-side segment and a
// nut/finger-side segment, each a delay line carrying a transverse *velocity*
// wave. They meet at the bow through a single-valued friction scattering
// junction; the bridge termination is a lossy reflection (the loop's only
// deliberate energy sink) and the nut is near-rigid.
//
// The bow IS the excitation. Raising the bow velocity from rest self-starts the
// travelling Helmholtz corner (stick-slip); the fixed loop losses cap its
// amplitude, so the result is a bounded limit cycle -- a sustaining, in-tune
// string, not a decaying pluck. There is no separate excitation generator and
// no per-sample iterative solve.
//
// Why this is real-time-safe and cannot run away:
//   * The friction junction is single-valued (a reflection coefficient in
//     [0,1]); it can never amplify the differential velocity, so the bow is
//     passive-bounded by construction.
//   * Every other loop element has magnitude <= 1 (fractional delay: passive;
//     nut: near-unity; bridge: strictly < 1). The loop is therefore net-lossy
//     whenever the bow is not injecting -- lift the bow and it decays to
//     silence -- and bounded while it is.
//   * Belt-and-suspenders, mirroring examples/va-drum: a hard clamp on the wave
//     pushed back into the delays (a physical velocity ceiling), denormal
//     snapping at block boundaries, and a NaN/Inf guard that resets the voice.
//
// Fractional delay is mandatory: a tuned string needs a loop delay of fs/f0
// samples and the fractional remainder is worth cents of pitch error (a
// quarter-tone in the top octave if truncated). DelayLineT interpolates only
// linearly -- its pitch-dependent magnitude droop would move the loss budget
// with the note -- so the delays here read with the tested stateless 4-point
// Lagrange helper (pulp::signal::Interpolator::lagrange). Stateless matters:
// vibrato / glissando / pitch-bend retune sample-to-sample with no transient,
// where a stateful allpass whose coefficient moves every sample would chirp.
//
// Friction curve: rho(dv) = clamp((|slope*dv| + c)^(-p), 0, 1), the hyperbolic
// stick->slip characteristic published as the friction *equation* (Smith, PASP
// "The Bowed String"; Serafin's thesis; a fit of the MSW/Schelleng
// characteristic). Bow force widens the stick region by lowering the slope.
// Derived from that equation and the MSW junction algebra only.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/interpolator.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::examples {

/// A one-way delay line read with 4-point Lagrange interpolation. The whole
/// sample part is an integer ring-buffer read; the sub-sample remainder goes
/// through the stateless Lagrange helper so a continuously-moving pitch retunes
/// with no interpolator transient. Owns nothing DelayLineT could give it -- it
/// exists to pair the ring buffer with Lagrange rather than DelayLineT's linear
/// tap, which is the tuning-critical difference for a waveguide.
///
/// RT contract: prepare() allocates; push()/read() are allocation- and
/// lock-free and data-independent.
class WaveguideDelay {
public:
    void prepare(int max_delay_samples) {
        // Power-of-two length + mask for branch-free wrap, with headroom for the
        // 4-tap Lagrange stencil (reads one sample past the delay).
        int need = std::max(max_delay_samples + 4, 8);
        int size = 1;
        while (size < need) size <<= 1;
        buffer_.assign(static_cast<std::size_t>(size), 0.0);
        mask_ = size - 1;
        write_ = 0;
    }

    void push(double sample) noexcept {
        buffer_[static_cast<std::size_t>(write_)] = sample;
        write_ = (write_ + 1) & mask_;
    }

    /// Read `delay` samples in the past (>= 1). The whole part indexes the ring;
    /// the fraction feeds Lagrange over the four straddling taps.
    double read(double delay) const noexcept {
        // Position of the requested (possibly fractional) sample, measured back
        // from the most-recently written slot -- same convention as DelayLineT.
        double base = static_cast<double>(write_) - delay - 1.0;
        double fl = std::floor(base);
        double frac = base - fl;
        int i0 = static_cast<int>(fl);
        double ym1 = tap(i0 - 1);
        double y0 = tap(i0);
        double y1 = tap(i0 + 1);
        double y2 = tap(i0 + 2);
        return signal::Interpolator::lagrange(frac, ym1, y0, y1, y2);
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0);
        write_ = 0;
    }

    void snap_denormals() noexcept {
        for (auto& s : buffer_) s = signal::snap_to_zero(s);
    }

private:
    double tap(int idx) const noexcept {
        return buffer_[static_cast<std::size_t>(idx & mask_)];
    }

    std::vector<double> buffer_;
    int mask_ = 0;
    int write_ = 0;
};

/// A one-pole lowpass reflection filter for the bridge termination. Models the
/// frequency-dependent loss at the bridge: y = (1-d)*x + d*y_prev. Damping `d`
/// (Tone) rolls off the highs; a separate loss gain `g` (Decay) sets how much
/// energy survives each round trip (and hence how hard the bow must work to
/// sustain, and how fast it dies when the bow lifts).
class BridgeFilter {
public:
    void set_damping(double d) noexcept { d_ = std::clamp(d, 0.0, 0.999); }
    void set_loss(double g) noexcept { g_ = std::clamp(g, 0.0, 0.9999); }

    /// Reflect an incoming wave: lowpass then apply the loss gain. The sign
    /// inversion of a velocity-wave reflection at a termination is applied by
    /// the caller so both terminations share one sign convention.
    double process(double x) noexcept {
        y_ = (1.0 - d_) * x + d_ * y_;
        return g_ * y_;
    }

    void reset() noexcept { y_ = 0.0; }
    void snap_denormals() noexcept { y_ = signal::snap_to_zero(y_); }

    double damping() const noexcept { return d_; }
    double loss() const noexcept { return g_; }

private:
    double d_ = 0.4;
    double g_ = 0.97;
    double y_ = 0.0;
};

/// One bowed string. Monophonic by construction: it is a single resonant loop
/// that is either being bowed or ringing down. prepare() sizes the delays for
/// the lowest supported pitch; set_frequency() splits the loop delay across the
/// two segments at the bow position; process() advances one sample.
class BowedString {
public:
    /// Lowest fundamental the delays are sized for. Below this the loop delay
    /// would exceed the buffer; set_frequency() clamps up to it.
    static constexpr double kMinFrequency = 20.0;

    void prepare(double sample_rate) {
        sample_rate_ = sample_rate;
        int max_total = static_cast<int>(sample_rate_ / kMinFrequency) + 8;
        bridge_delay_.prepare(max_total);
        nut_delay_.prepare(max_total);
        refresh_ramp();
        set_frequency(freq_hz_);
        reset();
    }

    void reset() noexcept {
        bridge_delay_.reset();
        nut_delay_.reset();
        bridge_filter_.reset();
        nut_lp_ = 0.0;
        v_bow_ = 0.0;
        v_bow_target_ = 0.0;
        engage_ = 0.0;
        engage_target_ = 0.0;
        bowing_ = false;
    }

    // ── Controls ────────────────────────────────────────────────────────────

    /// Set the fundamental. Total loop delay = fs/f0, less the loop's filter
    /// phase delay, split between the two segments by the bow position. The
    /// remainder rides the Lagrange fraction, which is why this stays in tune.
    void set_frequency(double f0) noexcept {
        freq_hz_ = std::clamp(f0, kMinFrequency, sample_rate_ * 0.45);
        update_delays();
        update_loss();
    }
    double frequency() const noexcept { return freq_hz_; }

    /// Bow position beta = distance from the bridge / string length, in (0,1).
    /// Small beta = near the bridge (brighter, sul ponticello); beta = 1/n
    /// places the bow on the node of harmonic n and suppresses it.
    void set_bow_position(double beta) noexcept {
        beta_ = std::clamp(beta, 0.02, 0.5);
        update_delays();
    }
    double bow_position() const noexcept { return beta_; }

    /// Bow force in [0,1]. More force widens the stick region (a lower friction
    /// slope): 0 = a thin, whistling, easily-slipping contact; 1 = a broad,
    /// raucous, near-locked grip. The measured playable band sits between.
    void set_bow_force(double force) noexcept {
        force_ = std::clamp(force, 0.0, 1.0);
        // Map to the BowTable slope: more force -> smaller slope -> the curve
        // holds near 1 (stick) over a wider differential velocity.
        slope_ = kSlopeMax - (kSlopeMax - kSlopeMin) * force_;
    }
    double bow_force() const noexcept { return force_; }

    /// Steady bow speed the note ramps toward while bowing (sets amplitude and,
    /// with force, onset speed). Normalized string-velocity units.
    void set_bow_velocity(double v) noexcept { bow_speed_ = std::max(0.0, v); }
    double bow_velocity() const noexcept { return bow_speed_; }

    /// Decay knob in [0,1] -> ring-down time (unbowed T60). The per-round-trip
    /// bridge loss is then derived from the note's own loop period, so the loss
    /// is frequency-compensated: a low (long) string takes more loss per trip
    /// than a high one and both ring for the same wall-clock time. Without this
    /// the fixed per-trip loss lets a low string over-accumulate energy and
    /// tip into a period-doubled (raucous) regime, while high strings die
    /// instantly -- the T60 mapping keeps the playable band open across the
    /// compass. Higher knob = longer ring and less bow force needed to sustain.
    void set_decay(double knob) noexcept {
        knob = std::clamp(knob, 0.0, 1.0);
        // Musical ring-down range, exponential so the knob feels even.
        t60_ = kMinT60 * std::pow(kMaxT60 / kMinT60, knob);
        update_loss();
    }
    double ring_time() const noexcept { return t60_; }
    /// Tone knob -> bridge damping. Higher = darker (more HF loss at the bridge).
    /// Retunes the loop because the one-pole's group delay moves with damping.
    void set_tone(double d) noexcept {
        bridge_filter_.set_damping(d);
        update_delays();
    }

    // ── Bowing ──────────────────────────────────────────────────────────────

    /// Start (or continue) bowing: ramp the bow up to bow_speed(). A bow is
    /// sustained, not a one-shot -- this does not reset the loop, so re-bowing a
    /// ringing string adds to whatever is in flight.
    void bow_on() noexcept {
        bowing_ = true;
        v_bow_target_ = bow_speed_;
        engage_target_ = 1.0;
    }

    /// Lift the bow: ramp the bow velocity to zero. The bridge loss then decays
    /// the Helmholtz motion to silence -- the natural release.
    void bow_off() noexcept {
        bowing_ = false;
        v_bow_target_ = 0.0;
        // Lift the bow off the string: the friction coupling ramps to zero so the
        // string becomes free (both terminations, no bow node) and rings down
        // through the bridge loss at the Decay time. Without this the bow stays
        // "stuck" at zero velocity -- a near-lossless node that traps energy in a
        // sub-segment and makes a low string ring on long after release.
        engage_target_ = 0.0;
    }

    bool is_bowing() const noexcept { return bowing_; }

    /// True once the loop energy has fallen to near silence AND the bow is off,
    /// so a voice pool can reclaim it.
    bool is_silent() const noexcept {
        return !bowing_ && engage_ < 1e-4 && std::abs(last_string_vel_) < 1e-6;
    }

    /// Advance one sample; returns the string velocity at the bow (the pickup).
    double process() noexcept {
        // Smooth the bow velocity so onsets/releases never step (a stepped bow
        // velocity injects a click and can jolt the loop out of its limit cycle).
        v_bow_ += (v_bow_target_ - v_bow_) * bow_ramp_;
        engage_ += (engage_target_ - engage_) * bow_ramp_;

        // Incoming velocity waves arriving at the bow, each already reflected at
        // its termination (with the -1 velocity-wave sign).
        double v_br_plus = -bridge_filter_.process(bridge_delay_.read(bridge_len_));
        // The nut/finger reflection carries a light lowpass too: a real string is
        // stopped by soft flesh rather than a knife edge, so the finger end loses
        // a little of the highest partials each pass. Small enough not to dull the
        // tone; its group delay is folded into the tuning correction below.
        double nut_raw = nut_delay_.read(nut_len_);
        nut_lp_ = (1.0 - nut_damping_) * nut_raw + nut_damping_ * nut_lp_;
        double v_nu_plus = -g_nut_ * nut_lp_;

        // Free string velocity at the bow, and the bow-string differential.
        double v_string = v_br_plus + v_nu_plus;
        double dv = v_bow_ - v_string;

        // Single-valued friction: rho in [0,1]. delta = rho*dv is the velocity
        // the bow drives the string toward v_bow. |delta| <= |dv| always.
        // Engagement gates the coupling: 1 while bowing, ramping to 0 once lifted
        // so a released string is free and decays cleanly.
        double rho = engage_ * bow_reflection(dv);
        double delta = rho * dv;

        // Scatter: the outgoing wave into each segment is the wave that arrived
        // from the *other* segment plus the bow's contribution.
        double to_nut = clamp_state(v_br_plus + delta);
        double to_bridge = clamp_state(v_nu_plus + delta);
        nut_delay_.push(to_nut);
        bridge_delay_.push(to_bridge);

        last_string_vel_ = v_string;
        if (!std::isfinite(v_string)) {
            reset();
            return 0.0;
        }
        return v_string;
    }

    /// Denormal housekeeping at block boundaries (call once per block, never
    /// per sample).
    void snap_denormals() noexcept {
        bridge_delay_.snap_denormals();
        nut_delay_.snap_denormals();
        bridge_filter_.snap_denormals();
        nut_lp_ = signal::snap_to_zero(nut_lp_);
        v_bow_ = signal::snap_to_zero(v_bow_);
        last_string_vel_ = signal::snap_to_zero(last_string_vel_);
    }

private:
    // The published BowTable friction characteristic (a fit of the MSW/Schelleng
    // stick-slip curve). Single-valued, so no per-sample solve and no hysteresis
    // that could latch the loop.
    double bow_reflection(double dv) const noexcept {
        double s = std::abs(slope_ * dv) + kFrictionKnee;
        double rho = 1.0 / (s * s * s * s);  // s^(-4), no pow() on the audio path
        return std::min(rho, 1.0);
    }

    static double clamp_state(double x) noexcept {
        // A travelling velocity wave has a physical ceiling. This never fires in
        // the bounded limit cycle; it is the hard backstop that guarantees the
        // feedback loop can never blow up into a hazard.
        return std::clamp(x, -kStateCeiling, kStateCeiling);
    }

    void update_loss() noexcept {
        if (sample_rate_ <= 0.0) return;
        // g per round trip so an unbowed string decays 60 dB in t60_ seconds.
        // The loop period is ~1/freq_hz_, so low notes (fewer trips per second)
        // get a smaller g (more loss per trip) and both ring for t60_ wall-clock.
        double g = std::exp(-kLn1000 / (freq_hz_ * t60_));
        bridge_filter_.set_loss(std::clamp(g, 0.5, 0.99995));
    }

    void update_delays() noexcept {
        if (sample_rate_ <= 0.0) return;
        // Total loop delay for the fundamental, less the loop's phase delay so
        // measured f0 lands in tune. The correction is the bridge one-pole's
        // group delay d/(1-d) (which moves with Tone) plus a fixed offset for
        // the Lagrange stencils and the read convention. The remainder rides the
        // Lagrange fraction, which is why this stays in tune across the compass.
        double bridge_gd = bridge_filter_.damping() / (1.0 - bridge_filter_.damping());
        double nut_gd = nut_damping_ / (1.0 - nut_damping_);
        double comp = kDelayComp + bridge_gd + nut_gd;
        double total = sample_rate_ / freq_hz_ - comp;
        total = std::max(total, 4.0);
        bridge_len_ = std::max(beta_ * total, 1.5);
        nut_len_ = std::max(total - bridge_len_, 1.5);
    }

    // Friction shape constants: the published BowTable knee (0.75) and exponent
    // (4). Measured across the compass, this is the richest and most even
    // setting -- more harmonics and a faster, flatter sustain than a softer knee.
    static constexpr double kFrictionKnee = 0.75;
    static constexpr double kSlopeMin = 1.0;   // most force: widest stick
    static constexpr double kSlopeMax = 5.0;   // least force: narrowest stick
    static constexpr double kStateCeiling = 8.0;
    // Fixed part of the loop-delay correction (Lagrange stencils + read
    // convention), measured so f0 lands in tune; the Tone-dependent bridge group
    // delay is added on top in update_delays().
    static constexpr double kDelayComp = 1.8;
    static constexpr double kBowRampSeconds = 0.02;
    static constexpr double kLn1000 = 6.907755278982137;  // ln(1000) == 60 dB
    static constexpr double kMinT60 = 0.15;                 // Decay knob = 0
    static constexpr double kMaxT60 = 6.0;                  // Decay knob = 1

    WaveguideDelay bridge_delay_;
    WaveguideDelay nut_delay_;
    BridgeFilter bridge_filter_;

    double sample_rate_ = 48000.0;
    double freq_hz_ = 220.0;
    double beta_ = 0.13;
    double bridge_len_ = 60.0;
    double nut_len_ = 100.0;

    double force_ = 0.5;
    double slope_ = 3.0;
    double bow_speed_ = 0.12;
    double v_bow_ = 0.0;
    double v_bow_target_ = 0.0;
    double engage_ = 0.0;
    double engage_target_ = 0.0;
    double bow_ramp_ = 1.0 / (kBowRampSeconds * 48000.0);
    double g_nut_ = 0.995;
    double nut_damping_ = 0.15;
    double nut_lp_ = 0.0;
    double t60_ = 1.5;
    double last_string_vel_ = 0.0;
    bool bowing_ = false;

    void refresh_ramp() noexcept {
        bow_ramp_ = 1.0 / std::max(kBowRampSeconds * sample_rate_, 1.0);
    }
};

}  // namespace pulp::examples
