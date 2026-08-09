#pragma once

// Diffusion character: a Schroeder allpass chain INSIDE the feedback loop.
//
// The placement is the whole point. A diffuser in series with the output would
// smear the first repeat and every later one identically; in the loop, repeat k
// has been through the chain k times, so the echo train melts progressively
// from a discrete slap into a soft cloud that approaches reverb texture as it
// recirculates.
//
// The base network is Dattorro's published input diffuser (Effect Design Part
// 1, Table 1). Two things keep it from ringing metallically: each stage's
// delay is rounded to an ODD sample count, so no two stages share a factor and
// their comb nulls never align; and two of the four stages are slowly
// modulated (Dattorro's own recommendation) through the same Lagrange
// fractional read the main delay line uses.

#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/detail/schroeder_allpass.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pulp::signal::chardelay {

/// One modulated Schroeder allpass: y[n] = -g*x[n] + v[n-d],
/// v[n] = x[n] + g*y[n]. The output-state form preserves live history when
/// control-rate updates change the coefficient.
class ModulatedAllpass {
public:
    void prepare(std::size_t capacity_samples) { line_.prepare(capacity_samples); }
    void reset() { line_.reset(); }

    double process(double x, double delay_samples, double gain) noexcept {
        const double delayed = line_.read(delay_samples);
        const auto step = detail::schroeder_allpass_output_state_step(x, delayed, gain);
        line_.push(step.write);
        return step.output;
    }

    double max_delay() const noexcept { return line_.max_delay(); }

private:
    FractionalDelayLine line_;
};

/// Dattorro's figure-of-eight reverb tank, one per channel.
///
/// The input diffuser above cannot make a reverb on its own: it has no
/// recirculation, so it smears a transient and then falls silent until the
/// delay's next repeat. This is the part that holds energy. Two branches, each
/// a modulated allpass, a delay, a damping lowpass, a second allpass and a
/// second delay, cross-coupled so each branch feeds the other through `decay`.
///
/// Two departures from the textbook tank, both in service of a GRANULAR
/// texture rather than a smooth hall:
///   * the first allpass in each branch is swept far harder than an
///     anti-metallic wobble (tens of samples, ~1 Hz), which scatters the
///     recirculating energy in time so the tail shimmers instead of sustaining;
///   * each branch delay is read at two extra offset taps as well as its end,
///     which multiplies echo density per pass without another allpass in the
///     loop.
class ReverbTank {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        delay_slew_coefficient_ =
            std::exp(-1.0 / (kDiffusionTankDelaySlewMs * 0.001 * sample_rate_));
        const double longest = std::max({kDiffusionTankDelayAMs[1], kDiffusionTankDelayBMs[1],
                                         kDiffusionTankApAMs[1], kDiffusionTankApBMs[1]});
        const auto capacity = static_cast<std::size_t>(
            std::ceil(longest * kDiffusionTankSizeScale.back() * 0.001 * fs +
                      kDiffusionTankModDepth.back()) + 8.0);
        for (auto& line : delays_) line.prepare(capacity);
        for (auto& ap : allpasses_) ap.prepare(capacity);
        for (auto& g : low_guard_) g.set_cutoff(kDiffusionTankLowGuardHz, fs);
    }

    void reset() {
        for (auto& line : delays_) line.reset();
        for (auto& ap : allpasses_) ap.reset();
        for (auto& d : damping_) d.reset();
        for (auto& g : low_guard_) g.reset();
        feedback_ = {0.0, 0.0};
        mod_phase_ = {0.0, 0.0};
        snap_delays_on_update_ = true;
    }

    void update(double character_amount) noexcept {
        const double size =
            interpolate_knots(kDiffusionTankAxis, kDiffusionTankSizeScale, character_amount);
        decay_ = interpolate_knots(kDiffusionTankAxis, kDiffusionTankDecay, character_amount);
        mix_ = interpolate_knots(kDiffusionTankAxis, kDiffusionTankMix, character_amount);
        mod_depth_ =
            interpolate_knots(kDiffusionTankAxis, kDiffusionTankModDepth, character_amount);
        const double damp_hz =
            interpolate_knots(kDiffusionTankAxis, kDiffusionTankDampHz, character_amount);
        for (auto& d : damping_) d.set_cutoff(damp_hz, sample_rate_);

        // Normalize the tank to roughly unity ENERGY before it is heard.
        //
        // Stability does not need this: the figure of eight recirculates
        // through feedback_ unscaled, so decay_ < 1 already guarantees the
        // tank converges, and output_scale_ affects level only — the tank is
        // never in the delay's feedback path. Dividing by the settled DC gain
        // (1 - decay) would assume steady state, but a repeat is a transient
        // that never settles, and that estimate over-attenuated by the whole
        // decay factor (a measured 17 dB loss at full character). A
        // recirculating network with per-pass gain g carries impulse energy
        // ~1/(1 - g^2), so the unity-energy scale is sqrt(1 - g^2); the tap
        // sum adds its own energy on top, divided back out the same way.
        output_scale_ = kTankHeadroom *
                        std::sqrt(std::max(1.0 - decay_ * decay_, 1e-4)) /
                        std::sqrt(1.0 + kTapGain * kTapGain *
                                            static_cast<double>(kTapCount));

        const double to_samples = 0.001 * sample_rate_ * size;
        delay_len_target_[0] = kDiffusionTankDelayAMs[0] * to_samples;
        delay_len_target_[1] = kDiffusionTankDelayAMs[1] * to_samples;
        delay_len_target_[2] = kDiffusionTankDelayBMs[0] * to_samples;
        delay_len_target_[3] = kDiffusionTankDelayBMs[1] * to_samples;
        ap_len_target_[0] = kDiffusionTankApAMs[0] * to_samples;
        ap_len_target_[1] = kDiffusionTankApAMs[1] * to_samples;
        ap_len_target_[2] = kDiffusionTankApBMs[0] * to_samples;
        ap_len_target_[3] = kDiffusionTankApBMs[1] * to_samples;
        if (snap_delays_on_update_) {
            delay_len_ = delay_len_target_;
            ap_len_ = ap_len_target_;
            snap_delays_on_update_ = false;
        }
    }

    /// Zero mix means the tank contributes nothing at all — the character is
    /// then the bare allpass diffuser, which is the magnitude-flat baseline the
    /// diffusion character is specified against.
    bool active() const noexcept { return mix_ > 0.0; }
    double mix() const noexcept { return mix_; }

    double process(double x, double decorrelation) noexcept {
        advance_delay_slew();
        // Both branches read the OTHER branch's previous output, which is what
        // closes the figure of eight without a zero-delay loop.
        const double in_a = x + feedback_[1] * decay_;
        const double in_b = x + feedback_[0] * decay_;

        double taps = 0.0;
        const double a = run_branch(0, in_a, decorrelation, taps);
        const double b = run_branch(1, in_b, decorrelation, taps);
        feedback_[0] = a;
        feedback_[1] = b;

        // The branch ends carry the tail; the scattered taps carry the density.
        return output_scale_ * (0.5 * (a + b) + kTapGain * taps);
    }

    void tick_modulation() noexcept {
        for (std::size_t m = 0; m < mod_phase_.size(); ++m) {
            mod_phase_[m] += kDiffusionTankModRatesHz[m] / sample_rate_;
            if (mod_phase_[m] >= 1.0) mod_phase_[m] -= 1.0;
        }
    }

private:
    static constexpr double kTapGain = 0.30;
    static constexpr std::size_t kTapCount = 4;   // two per branch
    static constexpr double kTankHeadroom = 1.0;

    void advance_delay_slew() noexcept {
        for (std::size_t i = 0; i < delay_len_.size(); ++i) {
            delay_len_[i] = delay_len_target_[i] +
                            delay_slew_coefficient_ * (delay_len_[i] - delay_len_target_[i]);
            ap_len_[i] =
                ap_len_target_[i] + delay_slew_coefficient_ * (ap_len_[i] - ap_len_target_[i]);
        }
    }

    /// One branch: allpass -> delay -> damp -> allpass -> delay, accumulating
    /// two offset taps from the first delay on the way through.
    double run_branch(std::size_t branch, double x, double decorrelation,
                      double& taps) noexcept {
        const std::size_t i0 = branch * 2u;
        const std::size_t i1 = i0 + 1u;

        const double sweep = mod_depth_ * decorrelation *
                             std::sin(2.0 * kPi * mod_phase_[branch]);
        double y = allpasses_[i0].process(
            x, clamp_delay(ap_len_[i0] + sweep, allpasses_[i0].max_delay()),
            kDiffusionTankApGain1);

        delays_[i0].push(y);
        const double len0 = clamp_delay(delay_len_[i0], delays_[i0].max_delay());
        for (std::size_t t = 0; t < kDiffusionTankTapFraction.size(); ++t)
            taps += delays_[i0].read(clamp_delay(len0 * kDiffusionTankTapFraction[t],
                                                 delays_[i0].max_delay()));
        y = delays_[i0].read(len0);

        y = damping_[branch].lowpass(y);
        // The other half of the damping: every pass sheds low end too, so no
        // sub-100 Hz mode can recirculate at unity while the rest decays.
        y = low_guard_[branch].highpass(y);
        y = allpasses_[i1].process(y, clamp_delay(ap_len_[i1], allpasses_[i1].max_delay()),
                                   kDiffusionTankApGain2);
        delays_[i1].push(y);
        return delays_[i1].read(clamp_delay(delay_len_[i1], delays_[i1].max_delay()));
    }

    static double clamp_delay(double d, double max_delay) noexcept {
        return std::clamp(d, 1.0, max_delay);
    }

    std::array<FractionalDelayLine, 4> delays_{};
    std::array<ModulatedAllpass, 4> allpasses_{};
    std::array<OnePole, 2> damping_{};
    std::array<OnePole, 2> low_guard_{};
    std::array<double, 4> delay_len_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> ap_len_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> delay_len_target_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, 4> ap_len_target_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, 2> feedback_ = {0.0, 0.0};
    std::array<double, 2> mod_phase_ = {0.0, 0.0};
    double sample_rate_ = 48000.0;
    double decay_ = 0.0;
    double mix_ = 0.0;
    double output_scale_ = 0.0;
    double mod_depth_ = 0.0;
    double delay_slew_coefficient_ = 0.0;
    bool snap_delays_on_update_ = true;
};

/// The four-stage chain for one channel.
class DiffusionChain {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
        tank_.prepare(fs);
        for (std::size_t i = 0; i < kStageCount; ++i) {
            // Sized for the largest sizeScale knot plus the modulation swing
            // and the Lagrange kernel's reach.
            const double max_ms = kDiffusionStageMs[i] * kDiffusionSizeScale.back();
            const auto capacity = static_cast<std::size_t>(
                std::ceil(max_ms * 0.001 * fs + kDiffusionModDepthSamples) + 8.0);
            stages_[i].prepare(capacity);
        }
    }

    void reset() {
        for (auto& s : stages_) s.reset();
        tank_.reset();
        mod_phase_ = {0.0, 0.0};
    }

    /// Recompute stage delays and gains for a character amount. Cheap enough
    /// to run at control rate; the odd-sample rounding means the delays step
    /// rather than glide, which is inaudible under the allpass.
    void update(double character_amount) noexcept {
        const double size_scale =
            interpolate_knots(kDiffusionAxis, kDiffusionSizeScale, character_amount);
        const double gain_scale =
            interpolate_knots(kDiffusionAxis, kDiffusionGainScale, character_amount);
        for (std::size_t i = 0; i < kStageCount; ++i) {
            const double samples = kDiffusionStageMs[i] * 0.001 * sample_rate_ * size_scale;
            delays_[i] = round_to_odd(samples);
            gains_[i] = std::min(kDiffusionStageGain[i] * gain_scale, kDiffusionGainMax);
        }
        tank_.update(character_amount);
    }

    /// The bare allpass diffuser — the only part safe to RECIRCULATE. An
    /// allpass is unity-gain by construction, so the delay's feedback loop can
    /// pass repeats through it forever and the always-decays contract survives.
    /// `decorrelation` scales the modulation depth (1.0 left, kStereoDecorr right).
    double process_diffuser(double x, double decorrelation) noexcept {
        for (std::size_t i = 0; i < kStageCount; ++i) {
            double delay = delays_[i];
            if (i == 0 || i == 2) {
                const std::size_t m = (i == 0) ? 0u : 1u;
                delay += kDiffusionModDepthSamples * decorrelation *
                         std::sin(2.0 * kPi * mod_phase_[m]);
            }
            delay = std::clamp(delay, 1.0, stages_[i].max_delay());
            x = stages_[i].process(x, delay, gains_[i]);
        }
        return x;
    }

    /// The reverb tank, crossfaded in — OUTPUT ONLY, never in the loop.
    ///
    /// The tank rings longer than a typical loop period, so it must not feed
    /// back through the delay line and compose two resonators into net growth.
    /// On the output it is excited afresh by every repeat (the cloud still
    /// blooms per repeat), but its energy never re-enters the line. Tank decay
    /// < 1 and loop feedback < 1 therefore guarantee their own halves of the
    /// stability contract independently.
    double process_cloud(double x, double decorrelation) noexcept {
        if (!tank_.active()) return x;
        const double cloud = tank_.process(x, decorrelation);
        return x + tank_.mix() * (cloud - x);
    }

    /// One-pass smear for the cross-character output blend: diffuser + cloud.
    double process(double x, double decorrelation) noexcept {
        return process_cloud(process_diffuser(x, decorrelation), decorrelation);
    }

    /// Advance the two anti-metallic LFOs by one sample.
    void tick_modulation() noexcept {
        tank_.tick_modulation();
        for (std::size_t m = 0; m < mod_phase_.size(); ++m) {
            mod_phase_[m] += kDiffusionModRatesHz[m] / sample_rate_;
            if (mod_phase_[m] >= 1.0) mod_phase_[m] -= 1.0;
        }
    }

private:
    static constexpr std::size_t kStageCount = 4;

    static double round_to_odd(double samples) noexcept {
        auto n = static_cast<long long>(std::llround(samples));
        if (n < 3) n = 3;
        if ((n & 1LL) == 0LL) n += 1;
        return static_cast<double>(n);
    }

    ReverbTank tank_{};
    std::array<ModulatedAllpass, kStageCount> stages_{};
    std::array<double, kStageCount> delays_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, kStageCount> gains_ = {0.0, 0.0, 0.0, 0.0};
    std::array<double, 2> mod_phase_ = {0.0, 0.0};
    double sample_rate_ = 48000.0;
};

}  // namespace pulp::signal::chardelay
