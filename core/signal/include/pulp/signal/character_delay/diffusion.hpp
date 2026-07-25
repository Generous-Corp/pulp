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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace pulp::signal::chardelay {

/// One modulated Schroeder allpass: y[n] = −g·x[n] + v[n−d], v[n] = x[n] + g·y[n].
class ModulatedAllpass {
public:
    void prepare(std::size_t capacity_samples) { line_.prepare(capacity_samples); }
    void reset() { line_.reset(); }

    double process(double x, double delay_samples, double gain) noexcept {
        const double delayed = line_.read(delay_samples);
        const double y = -gain * x + delayed;
        line_.push(x + gain * y);
        return y;
    }

    double max_delay() const noexcept { return line_.max_delay(); }

private:
    FractionalDelayLine line_;
};

/// The four-stage chain for one channel.
class DiffusionChain {
public:
    void prepare(double fs) {
        sample_rate_ = fs;
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
    }

    /// `decorrelation` scales the modulation depth (1.0 left, kStereoDecorr right).
    double process(double x, double decorrelation) noexcept {
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

    /// Advance the two anti-metallic LFOs by one sample.
    void tick_modulation() noexcept {
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

    std::array<ModulatedAllpass, kStageCount> stages_{};
    std::array<double, kStageCount> delays_ = {1.0, 1.0, 1.0, 1.0};
    std::array<double, kStageCount> gains_ = {0.0, 0.0, 0.0, 0.0};
    std::array<double, 2> mod_phase_ = {0.0, 0.0};
    double sample_rate_ = 48000.0;
};

}  // namespace pulp::signal::chardelay
