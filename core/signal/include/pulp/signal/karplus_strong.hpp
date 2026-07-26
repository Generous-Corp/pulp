#pragma once

#include <pulp/signal/delay_line.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::signal {

/// A plucked string: a delay line fed back through a loss filter.
///
/// The idea is that a string is a delay. A disturbance travels to the end,
/// reflects, and comes back, and the round trip time is the period — so a
/// buffer of noise circulating through a delay of that length *is* a plucked
/// string, with no oscillator and no harmonic series written down anywhere.
/// The harmonics appear because only frequencies whose period divides the loop
/// survive going round it.
///
/// Three filters in the loop turn that from a curiosity into an instrument:
///
/// * **Loss.** A gain below one, set from the decay time, is what makes the
///   note stop. It is derived from the frequency rather than fixed, because
///   the loop runs `f0` times a second: the same per-trip loss gives a much
///   shorter note at a high pitch, which is exactly what a real string does.
/// * **Damping.** A lowpass in the loop makes high partials die faster than
///   low ones, which every real string does and an ideal delay does not.
/// * **Stiffness.** An allpass makes the loop's delay frequency-dependent, so
///   the partials stretch slightly sharp of a true harmonic series. That
///   inharmonicity is what separates a piano or a struck bar from a guitar,
///   and it is audible long before it is measurable as a tuning error.
///
/// The excitation matters as much as the loop. `pluck_position` subtracts a
/// delayed copy of the burst from itself, which cancels every partial with a
/// node at that point — the reason a string plucked near the bridge is thin
/// and bright and one plucked at the middle is round.
///
/// The fractional part of the loop length is taken by a first-order allpass
/// rather than by interpolating between samples. Both approaches are standard,
/// but they differ in a way that matters here: linear interpolation is itself a
/// lowpass, and it sits *inside* the loop next to the damping filter, so its
/// attenuation compounds on every round trip and shortens the note by an amount
/// that depends on pitch. An allpass has unity magnitude at every frequency, so
/// the decay stays the decay the caller asked for. The allpass delay is kept
/// away from zero, where its pole and zero cancel on the unit circle and the
/// delay becomes discontinuous.
///
/// Karplus & Strong, CMJ 7(2), 1983; the loss and stiffness extensions follow
/// Jaffe & Smith, CMJ 7(2), 1983.
///
/// RT contract: `prepare()` allocates the delay line. Everything else,
/// including `pluck()` and `process()`, allocates nothing and takes no locks.
template <typename SampleType = float>
class KarplusStrongT {
public:
    /// Sizes the delay line for the lowest note the string will play.
    void prepare(double sample_rate, double lowest_hz = 20.0) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        const int longest = static_cast<int>(sample_rate_ / std::max(lowest_hz, 1.0)) + 4;
        loop_.prepare(longest);
        excitation_.prepare(longest);
        damping_.prepare(static_cast<SampleType>(sample_rate_));
        dynamic_.prepare(static_cast<SampleType>(sample_rate_));
        set_dynamic_bandwidth_hz(dynamic_hz_);
        update();
        reset();
    }

    void set_frequency(double hz) {
        frequency_ = std::clamp(hz, 20.0, 0.25 * sample_rate_);
        update();
    }

    /// Time for the note to fall by 60 dB, in seconds.
    void set_decay_seconds(double seconds) {
        decay_seconds_ = std::max(seconds, 0.01);
        update();
    }

    /// How much faster high partials die than low ones, 0 (bright) to 1 (dark).
    void set_damping(double amount) {
        damping_amount_ = std::clamp(amount, 0.0, 1.0);
        update();
    }

    /// Inharmonicity, 0 (a true harmonic series) to 1 (bar-like stretch).
    void set_stiffness(double amount) { stiffness_ = std::clamp(amount, 0.0, 1.0); }

    /// Where along the string it is excited, 0 (at the end) to 0.5 (the middle).
    void set_pluck_position(double position) {
        pluck_position_ = std::clamp(position, 0.0, 0.5);
    }

    /// Bandwidth of the excitation, in Hz — how hard the string is played.
    ///
    /// A harder attack is brighter, not merely louder, so this is where playing
    /// dynamics belong. Setting it from velocity is what stops a soft note being
    /// the same note turned down.
    void set_dynamic_bandwidth_hz(double hz) {
        dynamic_hz_ = std::clamp(hz, 100.0, 20000.0);
        dynamic_.set_cutoff(static_cast<SampleType>(
            std::min(dynamic_hz_, 0.49 * sample_rate_)));
    }

    /// Direction of the pick, 0 to 1. A one-pole on the excitation only; at 0 it
    /// is bypassed, and raising it darkens the attack the way striking with the
    /// flat of a pick does compared with its point.
    void set_pick_direction(double amount) {
        pick_direction_ = std::clamp(amount, 0.0, 0.99);
    }

    void reset() {
        loop_.reset();
        excitation_.reset();
        damping_.reset();
        dynamic_.reset();
        pick_state_ = 0;
        tuning_state_ = 0;
        stiffness_state_ = 0;
        burst_remaining_ = 0;
        settling_ = 0;
        active_ = false;
        level_ = 0.0;
    }

    /// Excites the string. The caller supplies the burst sample by sample
    /// through `process`; this only opens the window and clears the loop's
    /// contribution from any previous note if `restart` is set.
    ///
    /// Leaving `restart` false is the physical behaviour — a string plucked
    /// again while ringing keeps what is already travelling on it — and is
    /// what stops a fast repeated figure sounding identical each time.
    void pluck(bool restart = false) {
        if (restart) loop_.reset();
        burst_remaining_ = std::max(1, static_cast<int>(loop_length_));
        // Nothing comes back out of the loop until a full round trip has
        // elapsed, so the string is silent for its first period by
        // construction. Without this window the "has it finished" check below
        // sees that silence, concludes the note is over, and switches the voice
        // off one sample before it would have sounded.
        settling_ = burst_remaining_ + 2;
        active_ = true;
    }

    bool is_active() const { return active_; }
    double level() const { return level_; }

    /// Advances one sample. `excitation` is the burst being injected; feed it
    /// noise for a pluck, silence once the burst is over.
    SampleType process(SampleType excitation) {
        if (!active_) return SampleType{0};

        // Position comb: the burst minus a delayed copy of itself cancels every
        // partial with a node at the pluck point.
        SampleType shaped = excitation;
        if (burst_remaining_ > 0) {
            excitation_.push(excitation);
            if (pluck_position_ > 0.0) {
                const auto offset =
                    static_cast<SampleType>(pluck_position_ * loop_length_);
                shaped = excitation - excitation_.read(offset);
            }
            --burst_remaining_;
        } else {
            shaped = SampleType{0};
        }

        // Excitation shaping, all outside the loop: pick direction darkens the
        // attack, and the dynamic-level filter is where playing hard becomes
        // playing bright.
        if (pick_direction_ > 0.0) {
            const auto a = static_cast<SampleType>(pick_direction_);
            pick_state_ = (SampleType{1} - a) * shaped + a * pick_state_;
            shaped = pick_state_;
        }
        shaped = dynamic_.process_lowpass(shaped);

        SampleType y = loop_.read(static_cast<SampleType>(integer_delay_));
        y = damping_.process_lowpass(y);

        // First-order allpass carrying the fractional part of the loop length.
        // Unity magnitude, so unlike an interpolator it costs the decay nothing.
        const SampleType out = static_cast<SampleType>(tuning_coefficient_) * y +
                               tuning_state_;
        tuning_state_ = y - static_cast<SampleType>(tuning_coefficient_) * out;
        y = out;

        // First-order allpass: delays low frequencies more than high ones, so
        // the partials stretch sharp.
        if (stiffness_ > 0.0) {
            const auto a = static_cast<SampleType>(-0.9 * stiffness_);
            const SampleType out = a * y + stiffness_state_;
            stiffness_state_ = y - a * out;
            y = out;
        }

        y = static_cast<SampleType>(loop_gain_) * y;
        loop_.push(snap_to_zero(shaped + y));

        level_ = std::max(std::fabs(static_cast<double>(y)), level_ * 0.9999);
        if (settling_ > 0) --settling_;
        if (settling_ == 0 && burst_remaining_ <= 0 && level_ < 1e-6) active_ = false;
        return y;
    }

private:
    void update() {
        loop_length_ = sample_rate_ / frequency_;

        // Split the loop into an integer delay plus an allpass whose delay sits
        // in [0.1, 1.1) samples. Keeping it off zero is deliberate: at zero the
        // allpass's pole and zero cancel on the unit circle and its delay is no
        // longer continuous in the coefficient.
        constexpr double kMinFractional = 0.1;
        double fractional = loop_length_ - std::floor(loop_length_);
        integer_delay_ = std::floor(loop_length_);
        if (fractional < kMinFractional) {
            fractional += 1.0;
            integer_delay_ -= 1.0;
        }
        integer_delay_ = std::max(integer_delay_, 1.0);
        // Delay d of a first-order allpass with coefficient c is (1-c)/(1+c).
        tuning_coefficient_ = (1.0 - fractional) / (1.0 + fractional);
        // The loop runs f0 times a second, so reaching -60 dB after
        // `decay_seconds` means losing 3/(f0 * decay) decades per trip. Deriving
        // it this way is what makes a high note decay faster than a low one at
        // the same setting, as a real string does.
        loop_gain_ = std::pow(10.0, -3.0 / (frequency_ * decay_seconds_));

        // Damping sweeps the loop's corner geometrically from wide open down to
        // a few multiples of the fundamental, so the control is useful across
        // its whole travel rather than only near one end.
        const double open = std::min(0.45 * sample_rate_, 18000.0);
        // The closed end sits several multiples above the fundamental rather
        // than just above it. Any lower and the loop filter's own loss at f0
        // exceeds the loss the decay time asks for, at which point the decay
        // control stops being able to hold its setting.
        const double closed = std::min(frequency_ * 6.0, open);
        const double corner = open * std::pow(closed / open, damping_amount_);
        damping_.set_cutoff(static_cast<SampleType>(corner));

        // The damping filter attenuates the fundamental as well as the upper
        // partials -- only slightly, but the signal goes round the loop
        // hundreds of times, so a fraction of a decibel per trip compounds into
        // tens of decibels. Left uncompensated the damping control is mostly a
        // volume control: a fully damped string measured two hundred times
        // quieter at its own fundamental. Dividing the loop gain by the
        // filter's response at f0 leaves the fundamental decaying at the
        // requested rate and confines damping to what it is named for.
        //
        // The clamp is where physics wins: if the filter's own per-trip loss
        // is already larger than the decay time asks for, no loop gain below
        // unity can make the note last that long, and it dies faster than
        // requested. That is what a heavily damped string does.
        const double ratio = frequency_ / std::max(corner, 1e-9);
        loop_gain_ = std::min(loop_gain_ * std::sqrt(1.0 + ratio * ratio), 0.9999);
    }

    DelayLineT<SampleType> loop_;
    DelayLineT<SampleType> excitation_;
    TptFilterT<SampleType> damping_;
    TptFilterT<SampleType> dynamic_;

    double sample_rate_ = 44100.0;
    double frequency_ = 220.0;
    double decay_seconds_ = 2.0;
    double damping_amount_ = 0.3;
    double stiffness_ = 0.0;
    double pluck_position_ = 0.25;

    double loop_length_ = 200.0;
    double integer_delay_ = 200.0;
    double tuning_coefficient_ = 0.0;
    double loop_gain_ = 0.99;
    double dynamic_hz_ = 12000.0;
    double pick_direction_ = 0.0;
    SampleType pick_state_ = 0;
    SampleType tuning_state_ = 0;
    SampleType stiffness_state_ = 0;
    int burst_remaining_ = 0;
    int settling_ = 0;
    bool active_ = false;
    double level_ = 0.0;
};

using KarplusStrong = KarplusStrongT<float>;
using KarplusStrong64 = KarplusStrongT<double>;

}  // namespace pulp::signal
