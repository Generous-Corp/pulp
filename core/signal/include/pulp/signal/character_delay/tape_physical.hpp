#pragma once

// Physical tape tier — the standard tier's saturation and loss stages replaced
// by the actual physics, plus the wear artifacts a used machine has.
//
//   * Saturation → Jiles-Atherton magnetization (hysteresis.hpp).
//   * Loss stand-in → Wallace's playback-loss physics (Wallace 1951; physical
//     ranges from Bertram 1994), split across a fitted IIR cascade and a
//     minimum-phase FIR (tape_loss.hpp).
//   * Age → a wear axis: spacing grows, hiss rises, dropouts ("chew") appear,
//     and continuous degradation closes the bandwidth down.
//
// The loss stage is a cascade of two realizations — a fitted IIR for the smooth
// spacing/thickness tilt and a minimum-phase FIR for the gap null. tape_loss.hpp
// carries the reasoning and the normative crossfade rule; this file just wires
// them into the channel.
//
// Why nothing here is designed on the audio thread: the age macro is a
// per-sample parameter, and fitting a filter — a simplex search over 24
// frequency points, or an FFT and a cepstral fold — is not something an audio
// callback may do. So the age axis is fitted at kLossAgeKnots when the sample
// rate is set, and the audio thread only interpolates four physical parameters
// between neighbouring knots.

#include <pulp/signal/character_delay/hysteresis.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/character_delay/tape_loss.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal::chardelay {

/// Intermittent dropout. Two states with randomly drawn durations and
/// raised-cosine transitions, so the artifact arrives and leaves the way a
/// crease in the tape passes the head rather than switching on a sample edge.
class TapeChew {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        transition_samples_ = std::max(1, static_cast<int>(kChewTransitionS * fs));
        lowpass_.set_cutoff(kChewLpHz, fs);
        reset();
    }

    void reset() noexcept {
        rng_.reset();
        lowpass_.reset();
        degraded_ = false;
        envelope_ = 0.0;
        remaining_ = draw_duration(kChewCleanScaleS);
        state_index_ = 0;
    }

    void set_seed(std::uint32_t seed) noexcept { rng_.reseed(seed); }

    /// State-transition count since reset — the acceptance suite's determinism
    /// check compares this sequence across renders.
    std::size_t state_index() const noexcept { return state_index_; }

    double process(double x, double depth) noexcept {
        if (--remaining_ <= 0) {
            degraded_ = !degraded_;
            ++state_index_;
            remaining_ = draw_duration(degraded_ ? kChewDegradedScaleS : kChewCleanScaleS);
        }

        const double target = degraded_ ? 1.0 : 0.0;
        const double increment = 1.0 / static_cast<double>(transition_samples_);
        envelope_ = std::clamp(envelope_ + (target > envelope_ ? increment : -increment), 0.0, 1.0);
        if (depth <= 0.0 || envelope_ <= 0.0) {
            lowpass_.lowpass(x);  // keep the filter's state warm so re-entry is smooth
            return x;
        }

        const double window = 0.5 * (1.0 - std::cos(kPi * envelope_));
        const double magnitude = std::pow(std::abs(x), kChewPower);
        const double shaped = lowpass_.lowpass((x < 0.0 ? -magnitude : magnitude));
        return x + depth * window * (shaped - x);
    }

private:
    int draw_duration(double scale_seconds) noexcept {
        const double r = std::max(rng_.uniform(), 1e-6);
        const double seconds = scale_seconds * std::pow(r, kChewVariance);
        return std::max(1, static_cast<int>(seconds * sample_rate_));
    }

    Xorshift32 rng_{kPrngSeed};
    OnePole lowpass_;
    double sample_rate_ = 48000.0;
    double envelope_ = 0.0;
    int transition_samples_ = 1;
    int remaining_ = 1;
    bool degraded_ = false;
    std::size_t state_index_ = 0;
};

/// The physical tier for one channel.
class TapePhysicalChannel {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        solver_rate_ = fs * 4.0;  // the oversampler's factor

        const double emphasis_gain = std::pow(10.0, kTapeEmphasisDb / 20.0);
        record_eq_.set(kTapeEmphasisHz, emphasis_gain, fs);
        playback_eq_.set_inverse(kTapeEmphasisHz, emphasis_gain, fs);
        dc_blocker_.set_cutoff(kTapeDcBlockHz, fs);
        hiss_filter_.set_cutoff(kHissLpHz, fs);

        oversampler_.prepare();
        hysteresis_.prepare(solver_rate_);
        chew_.prepare(fs);

        design_.prepare(fs, speed_ips_);
        previous_design_ = design_;
        const std::size_t taps = design_.gap_taps().size();
        active_.prepare(taps);
        previous_.prepare(taps);

        crossfade_samples_ = std::max(1, static_cast<int>(kLossBankCrossfadeS * fs));
        degrade_filter_.set_cutoff(20000.0, fs);

        build_makeup_table();
        update(0.0);
        reset();
    }

    void reset() noexcept {
        record_eq_.reset();
        playback_eq_.reset();
        dc_blocker_.reset();
        hiss_filter_.reset();
        degrade_filter_.reset();
        head_bump_.reset();
        oversampler_.reset();
        hysteresis_.reset();
        chew_.reset();
        active_.reset();
        previous_.reset();
        hiss_rng_.reset();
        degrade_rng_.reset();
        crossfade_remaining_ = 0;
    }

    void set_seeds(std::uint32_t base) noexcept {
        hiss_rng_.reseed(base ^ 0x9E3779B9u);
        degrade_rng_.reseed(base ^ 0x85EBCA6Bu);
        chew_.set_seed(base ^ 0xC2B2AE35u);
    }

    /// Control-thread only: redesigns both filters and starts the crossfade.
    ///
    /// The outgoing design keeps running in its OWN complete filter instance
    /// and the two outputs are crossfaded — never a blend of coefficients, which
    /// for the recursive half would apply mixed coefficients to state belonging
    /// to neither design. The incoming stage inherits the outgoing one's FIR
    /// history (delayed input samples, meaningful under any coefficients) and
    /// starts its recursive half from zero, which the crossfade covers because
    /// the incoming stage's weight is still near zero while it warms.
    void set_speed_ips(double ips) {
        const double clamped = std::clamp(ips, kTapeSpeedsIps.front(), kTapeSpeedsIps.back());
        if (std::abs(clamped - speed_ips_) < 1e-9) return;
        speed_ips_ = clamped;
        // Callers legitimately configure the speed before the sample rate is
        // known (the catalog node does exactly that). Until prepare() has run
        // there is nothing designed to swap, so record the speed and let
        // prepare() design against it.
        if (design_.empty()) return;

        previous_ = active_;
        previous_design_ = design_;
        design_.set_speed(speed_ips_);
        active_.copy_history_from(previous_);
        active_.reset_recursive();
        active_.set_parameters(design_.parameters_at(age_), sample_rate_);
        crossfade_remaining_ = crossfade_samples_;
    }

    double speed_ips() const noexcept { return speed_ips_; }
    const std::vector<double>& gap_coefficients() const noexcept { return design_.gap_taps(); }
    TapeLossIirParams loss_parameters() const noexcept { return active_.parameters(); }

    /// Control-rate update. In this tier `character_amount` IS the age axis.
    void update(double age) noexcept {
        age_ = std::clamp(age, 0.0, 1.0);

        const double drive = interpolate_knots(kTapeAxis, kTapeDrive, age_);
        const double bias = interpolate_knots(kTapeAxis, kTapeBias, age_);
        hysteresis_.set_character(drive, bias);
        makeup_ = makeup_for(age_);

        hiss_gain_ = std::pow(10.0, interpolate_knots(kAgeAxis, kAgeHissDbfs, age_) / 20.0);
        chew_depth_ = interpolate_knots(kAgeAxis, kAgeChewDepth, age_);
        degrade_ = interpolate_knots(kAgeAxis, kAgeDegrade, age_);

        const double bump_db = interpolate_knots(kTapeAxis, kTapeBumpDb, age_);
        head_bump_.set_bell(interpolate_knots(kHeadBumpIps, kHeadBumpHz, speed_ips_), kTapeBumpQ,
                            bump_db, sample_rate_);

        // Recomputing the IIR from continuously interpolated PHYSICAL
        // parameters is ordinary filter modulation, not a coefficient blend
        // between two designs — see tape_loss.hpp. During a speed crossfade the
        // outgoing stage is deliberately frozen at the parameters it had.
        active_.set_parameters(design_.parameters_at(age_), sample_rate_);

        // Continuous wear: bandwidth falls exponentially toward the floor, and
        // the level dips slightly — a tired machine is darker AND quieter.
        const double nyquist_guard = 0.45 * sample_rate_;
        const double top = std::min(20000.0, nyquist_guard);
        const double degrade_hz = top * std::pow(kDegradeMinLpHz / top, degrade_);
        degrade_filter_.set_cutoff(degrade_hz, sample_rate_);
        degrade_gain_ = std::pow(10.0, kDegradeGainDipDb * degrade_ / 20.0);
    }

    /// Record EQ → oversampled hysteresis → makeup → playback EQ.
    double pre_process(double x) noexcept {
        const double emphasized = record_eq_.process(x);
        const double magnetized = oversampler_.process(
            emphasized, [this](double sample) { return hysteresis_.process(sample); });
        return playback_eq_.process(magnetized * makeup_);
    }

    /// Wallace loss → head bump → hiss → chew → degrade → DC block.
    double post_process(double x) noexcept {
        active_.push(x);
        double lossy = active_.process(design_);
        if (crossfade_remaining_ > 0) {
            previous_.push(x);
            const double outgoing = previous_.process(previous_design_);
            const double t = 1.0 - static_cast<double>(crossfade_remaining_) /
                                       static_cast<double>(crossfade_samples_);
            const double weight = 0.5 * (1.0 - std::cos(kPi * t));
            lossy = outgoing + weight * (lossy - outgoing);
            --crossfade_remaining_;
        }

        double y = head_bump_.process(lossy);
        y += hiss_filter_.lowpass(hiss_rng_.bipolar()) * hiss_gain_;
        y = chew_.process(y, chew_depth_);
        if (degrade_ > 0.0) {
            y = degrade_filter_.lowpass(y) * degrade_gain_;
            y += degrade_rng_.bipolar() * 1e-4 * degrade_;
        }
        return dc_blocker_.highpass(y);
    }

    /// Group delay the oversampling wrap adds inside the loop, in host samples.
    static int oversampler_latency_samples() noexcept {
        return HalfBandOversampler4x::latency_samples();
    }

    /// Total in-loop delay this tier contributes ahead of the delay line's
    /// read: the oversampler's group delay plus both halves of the loss
    /// cascade. The engine subtracts this from the requested line delay so the
    /// echo still lands on the requested time and latency_samples() can stay 0.
    double in_loop_delay_samples() const noexcept {
        return static_cast<double>(oversampler_latency_samples()) +
               design_.gap_group_delay_samples() +
               active_.dc_group_delay_seconds() * sample_rate_;
    }

    const JilesAthertonHysteresis& hysteresis() const noexcept { return hysteresis_; }
    JilesAthertonHysteresis& hysteresis() noexcept { return hysteresis_; }
    std::size_t chew_state_index() const noexcept { return chew_.state_index(); }

private:
    static constexpr std::size_t kMakeupKnots = 11;

    /// Makeup gain is MEASURED, not derived. The Jiles-Atherton small-signal
    /// slope has contributions from both the reversible and irreversible terms
    /// and no closed form worth trusting, so the curve is sampled once at
    /// prepare() by running a low-amplitude tone through a scratch solver at
    /// each knot. That keeps a character sweep from also being a level sweep,
    /// which inside a feedback loop would read as the feedback amount changing.
    void build_makeup_table() {
        for (std::size_t knot = 0; knot < kMakeupKnots; ++knot) {
            const double age =
                static_cast<double>(knot) / static_cast<double>(kMakeupKnots - 1u);
            const double drive = interpolate_knots(kTapeAxis, kTapeDrive, age);
            const double bias = interpolate_knots(kTapeAxis, kTapeBias, age);

            JilesAthertonHysteresis probe;
            probe.prepare(solver_rate_);
            probe.set_character(drive, bias);

            constexpr double kProbeAmplitude = 0.02;
            constexpr double kProbeHz = 1000.0;
            const auto period_samples =
                static_cast<int>(std::llround(solver_rate_ / kProbeHz));
            double input_energy = 0.0;
            double output_energy = 0.0;
            for (int n = 0; n < period_samples * 8; ++n) {
                const double phase = 2.0 * kPi * static_cast<double>(n) / static_cast<double>(period_samples);
                const double in = kProbeAmplitude * std::sin(phase);
                const double out = probe.process(in);
                if (n >= period_samples * 4) {
                    input_energy += in * in;
                    output_energy += out * out;
                }
            }
            const double ratio = (output_energy > 1e-30)
                                     ? std::sqrt(input_energy / output_energy)
                                     : 1.0;
            makeup_table_[knot] = std::clamp(ratio, 0.05, 50.0);
        }
    }

    double makeup_for(double age) const noexcept {
        const double position =
            std::clamp(age, 0.0, 1.0) * static_cast<double>(kMakeupKnots - 1u);
        auto lower = static_cast<std::size_t>(position);
        if (lower >= kMakeupKnots - 1u) lower = kMakeupKnots - 2u;
        const double t = position - static_cast<double>(lower);
        return makeup_table_[lower] + t * (makeup_table_[lower + 1u] - makeup_table_[lower]);
    }

    FirstOrderShelf record_eq_;
    FirstOrderShelf playback_eq_;
    OnePole dc_blocker_;
    OnePole hiss_filter_;
    OnePole degrade_filter_;
    Svf2 head_bump_;
    HalfBandOversampler4x oversampler_;
    JilesAthertonHysteresis hysteresis_;
    TapeChew chew_;
    TapeLossDesign design_;
    TapeLossDesign previous_design_;
    TapeLossStage active_;
    TapeLossStage previous_;
    Xorshift32 hiss_rng_{kPrngSeed};
    Xorshift32 degrade_rng_{kPrngSeed};

    std::array<double, kMakeupKnots> makeup_table_{};

    double sample_rate_ = 48000.0;
    double solver_rate_ = 192000.0;
    double speed_ips_ = 7.5;
    double age_ = 0.0;
    double makeup_ = 1.0;
    double hiss_gain_ = 0.0;
    double chew_depth_ = 0.0;
    double degrade_ = 0.0;
    double degrade_gain_ = 1.0;
    int crossfade_samples_ = 1;
    int crossfade_remaining_ = 0;
};

}  // namespace pulp::signal::chardelay
