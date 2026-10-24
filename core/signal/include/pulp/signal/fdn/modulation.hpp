#pragma once

// Motion primitives for the FDN tank: a deterministic PRNG, a mean-reverting
// random walk, a phase oscillator, and the per-channel delay-modulation bank
// that layers the last two.
//
// Sixteen lines each doing something slightly different is what keeps a
// 16-channel tank from sounding like one metallic tube. Sine alone is periodic
// (it beats against itself); a walk alone wanders sameily. Layering them breaks
// both, and every source is seeded from a fixed constant so a render is
// bit-reproducible across runs and machines.

#include <pulp/signal/fdn/config.hpp>

#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::fdn {

// Deterministic 32-bit xorshift. Seeded, never from a clock or random_device:
// two renders of the same material must be bit-identical.
class XorShift32 {
public:
    XorShift32() = default;
    explicit XorShift32(std::uint32_t seed) { reset(seed); }

    void reset(std::uint32_t seed) { state_ = seed ? seed : 0x9E3779B9u; }

    std::uint32_t next() {
        std::uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    // Uniform in [0, 1).
    double next_unit() { return static_cast<double>(next() >> 8) * (1.0 / 16777216.0); }

    // Uniform in [-1, 1).
    double next_bipolar() { return next_unit() * 2.0 - 1.0; }

private:
    std::uint32_t state_ = 0x9E3779B9u;
};

// Mean-reverting (Ornstein-Uhlenbeck-style) random walk, stepped at the control
// rate. `reversion` pulls the value back toward zero every step, which is what
// keeps a long render from drifting off and parking against a limit — a plain
// random walk has no such bound and would eventually pin the delay modulation.
class MeanRevertingWalk {
public:
    void reset() { value_ = 0.0; }

    // One control-rate step. Returns the new value, bounded in [-1, 1].
    double step(XorShift32& rng, double reversion, double step_size) {
        value_ += step_size * rng.next_bipolar() - reversion * value_;
        if (value_ > 1.0) value_ = 1.0;
        if (value_ < -1.0) value_ = -1.0;
        return value_;
    }

    double value() const { return value_; }

private:
    double value_ = 0.0;
};

// Unit-amplitude sine from a wrapped phase accumulator. Phase math is double so
// a 60 s render at 96 kHz does not accumulate a perceptible frequency error.
class PhaseOsc {
public:
    void reset(double phase = 0.0) { phase_ = phase; }

    void set_rate(double hz, double sample_rate) {
        increment_ = (sample_rate > 0.0) ? hz / sample_rate : 0.0;
    }

    double tick() {
        phase_ += increment_;
        if (phase_ >= 1.0) phase_ -= std::floor(phase_);
        return std::sin(phase_ * 6.283185307179586);
    }

    double phase() const { return phase_; }

private:
    double phase_ = 0.0;
    double increment_ = 0.0;
};

// A bank of N unit sines advanced in lockstep by rotation: one complex
// multiply per oscillator per sample instead of a sin() call. The exact phase
// is carried separately and the rotation re-derived from it every
// kOscReanchorSamples steps, so rounding cannot accumulate; the re-anchor
// points depend only on how many samples have been rendered, never on how
// they were split into blocks, which keeps renders block-partition exact.
//
// Each oscillator matches PhaseOsc: a tick advances the phase, then reports
// sin (and cos) of the new phase.
template <int N> class QuadratureBank {
  public:
    // Restart the shared clock. Follow with reset(i, phase) for every
    // oscillator.
    void restart() {
        count_ = 0;
    }

    // Place oscillator i at `phase` (in cycles) without ticking it.
    void reset(int i, double phase) {
        const auto u = static_cast<std::size_t>(i);
        phase_[u] = wrap(phase - static_cast<double>(count_) * increment_[u]);
        derive(u, wrap(phase));
    }

    // Change oscillator i's rate, keeping its current phase.
    void set_rate(int i, double hz, double sample_rate) {
        const auto u = static_cast<std::size_t>(i);
        const double current = position(i);
        increment_[u] = (sample_rate > 0.0) ? hz / sample_rate : 0.0;
        phase_[u] = wrap(current - static_cast<double>(count_) * increment_[u]);
        derive(u, current);
    }

    // Advance every oscillator one sample.
    void tick() {
        if (++count_ >= kOscReanchorSamples) {
            for (std::size_t u = 0; u < static_cast<std::size_t>(N); ++u) {
                phase_[u] = wrap(phase_[u] + static_cast<double>(count_) * increment_[u]);
                derive(u, phase_[u]);
            }
            count_ = 0;
            return;
        }
        for (std::size_t u = 0; u < static_cast<std::size_t>(N); ++u) {
            const double s = sin_[u] * step_cos_[u] + cos_[u] * step_sin_[u];
            const double c = cos_[u] * step_cos_[u] - sin_[u] * step_sin_[u];
            sin_[u] = s;
            cos_[u] = c;
        }
    }

    double sin(int i) const {
        return sin_[static_cast<std::size_t>(i)];
    }
    double cos(int i) const {
        return cos_[static_cast<std::size_t>(i)];
    }
    const double* sin_data() const {
        return sin_.data();
    }

    // Current phase in cycles, [0, 1).
    double position(int i) const {
        const auto u = static_cast<std::size_t>(i);
        return wrap(phase_[u] + static_cast<double>(count_) * increment_[u]);
    }

  private:
    static constexpr double kTwoPi = 6.283185307179586;

    static double wrap(double phase) {
        return phase - std::floor(phase);
    }

    void derive(std::size_t u, double position) {
        sin_[u] = std::sin(position * kTwoPi);
        cos_[u] = std::cos(position * kTwoPi);
        step_sin_[u] = std::sin(increment_[u] * kTwoPi);
        step_cos_[u] = std::cos(increment_[u] * kTwoPi);
    }

    // Phase at the start of the shared clock, so phase + count * increment is
    // the current phase of every oscillator.
    std::array<double, N> phase_{};
    std::array<double, N> increment_{};
    std::array<double, N> sin_{};
    std::array<double, N> cos_{};
    std::array<double, N> step_sin_{};
    std::array<double, N> step_cos_{};
    int count_ = 0;
};

// How much the modulation depth shrinks as the decay lengthens. Constant-depth
// modulation on a very long tail accumulates audible pitch drift; scaling depth
// down with T60 keeps long tails still while short tails stay lush.
inline double mod_depth_shrink(double decay_seconds) {
    if (decay_seconds <= kModShrinkFullSeconds) return 1.0;
    if (decay_seconds >= kModShrinkMinSeconds) return kModShrinkFloor;
    const double t = (decay_seconds - kModShrinkFullSeconds) /
                     (kModShrinkMinSeconds - kModShrinkFullSeconds);
    return 1.0 + t * (kModShrinkFloor - 1.0);
}

// Per-channel delay modulation: sine + mean-reverting walk, mixed by the
// kModSineMix / kModWalkMix pair. One instance drives all kNumChannels lines in
// lockstep; the rates are spread across the channels so no two lines beat
// together.
class DelayModulationBank {
public:
    // Seeds are fixed per channel; `reset()` restores them, so a reset render
    // reproduces the previous one exactly.
    void reset() {
        osc_.restart();
        for (int i = 0; i < kNumChannels; ++i) {
            rng_[i].reset(kSeedBase + static_cast<std::uint32_t>(i) * kSeedStride);
            walk_[i].reset();
            // Spread the starting phases evenly so the lines never start in
            // lockstep (a 16-line unison sweep is exactly the artifact the
            // per-line rate spread exists to avoid).
            osc_.reset(i, static_cast<double>(i) / static_cast<double>(kNumChannels));
        }
    }

    void configure(double tank_rate) {
        for (int i = 0; i < kNumChannels; ++i) {
            const double frac = (kNumChannels > 1)
                                    ? static_cast<double>(i) /
                                          static_cast<double>(kNumChannels - 1)
                                    : 0.0;
            osc_.set_rate(i, kModRateMinHz + frac * (kModRateMaxHz - kModRateMinHz), tank_rate);
        }
    }

    // Control-rate update of the walk sources.
    void step_walks() {
        for (int i = 0; i < kNumChannels; ++i)
            walk_[i].step(rng_[i], kWalkReversion, kWalkStep);
    }

    // Advance every channel one sample and write each channel's modulation
    // signal, in [-1, 1], to out[0 .. kNumChannels).
    void tick(double* out) {
        osc_.tick();
        const double* s = osc_.sin_data();
        for (int i = 0; i < kNumChannels; ++i)
            out[i] = kModSineMix * s[i] + kModWalkMix * walk_[i].value();
    }

private:
    static constexpr std::uint32_t kSeedBase = 0x1234567u;
    static constexpr std::uint32_t kSeedStride = 0x9E3779B9u;

    std::array<XorShift32, kNumChannels> rng_{};
    std::array<MeanRevertingWalk, kNumChannels> walk_{};
    QuadratureBank<kNumChannels> osc_{};
};

}  // namespace pulp::signal::fdn
