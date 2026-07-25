#pragma once

/// @file rng.hpp
/// The house deterministic RNG, and the two stochastic modulation sources
/// built on it.
///
/// Series law 2 requires every "random" value in the catalog to be
/// reproducible: a render must be bit-identical for a given (params, input),
/// which means no `rand()`, no clock or address entropy, no denormal-dependent
/// branching, and no seed that a host can automate. This header is how that
/// law is kept — one generator, one seeding discipline, and two shaped noise
/// sources so that "analog drift" never becomes a excuse to reach for
/// `std::random_device`.
///
/// Two generators, for two different jobs:
///
///   - `Xorshift32` is the **sequential** generator (Marsaglia 2003). Call it
///     in order; state advances. This is what a noise source or a per-clock
///     coin flip wants. Three xor-shifts, no multiply.
///   - `mix64` is the **stateless keyed** hash (SplitMix64's finalizer). Given
///     `(seed, index, field)` it returns the same value forever, with no state
///     to advance and no ordering requirement. This is what a grain scheduler
///     or a per-partial detune wants: keying off a monotonic index rather than
///     a pool slot makes a draw independent of block size and of voice-steal,
///     which is the difference between "deterministic" and "deterministic as
///     long as you never change the buffer size".
///
/// Reach for `mix64` whenever a value must be reproducible independent of the
/// order it is asked for. Reach for `Xorshift32` when a stream of numbers is
/// genuinely a stream.
///
/// `OuWalkT` and `DriftT` are the two shaped sources. Both are bounded in
/// probability, both are seeded, both reset to a fixed state — so "the tape
/// wow wandered" is a repeatable statement rather than a different render
/// every time.
///
/// RT contract: nothing here allocates, locks, or performs I/O. All state is
/// POD and zero-init is a valid (if unseeded) state; `reset()` restores the
/// seed. `OuWalkT::prepare()` only recomputes coefficients — it does not
/// allocate either, so it is safe to call from a parameter change.

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// The house 32-bit deterministic generator: Marsaglia's xorshift with the
/// (13, 17, 5) triple. Period 2^32 − 1; state is never allowed to reach zero,
/// which is the generator's single absorbing state.
///
/// This is the same recurrence `NoiseSourceT` has always used, so a block that
/// migrates to this type produces a bit-identical stream.
class Xorshift32 {
public:
    /// Any non-zero constant works; this one is arbitrary and fixed so an
    /// unseeded generator is still deterministic rather than stuck at zero.
    static constexpr std::uint32_t kDefaultSeed = 0x9E3779B9u;

    constexpr Xorshift32() = default;
    constexpr explicit Xorshift32(std::uint32_t seed) { set_seed(seed); }

    /// Sets the seed and rewinds. Zero is remapped to `kDefaultSeed` because a
    /// zero state would emit zeros forever.
    constexpr void set_seed(std::uint32_t seed) {
        seed_ = seed == 0u ? kDefaultSeed : seed;
        state_ = seed_;
    }

    constexpr std::uint32_t seed() const { return seed_; }

    /// Rewinds to the seed. Per series law 2 this is called from every block's
    /// `reset()`, so a render always starts from the same point in the stream.
    constexpr void reset() { state_ = seed_; }

    /// Next raw 32-bit word.
    constexpr std::uint32_t next_uint() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    /// Next sample uniform on `[-1, 1)`. The divisor is 2^31 so the sign is
    /// symmetric.
    template <typename T = float>
    T next_bipolar() {
        return static_cast<T>(static_cast<double>(next_uint()) * (1.0 / 2147483648.0) - 1.0);
    }

    /// Next sample uniform on `[0, 1)`.
    template <typename T = float>
    T next_unit() {
        return static_cast<T>(static_cast<double>(next_uint()) * (1.0 / 4294967296.0));
    }

    /// Next Boolean, taken from the top bit (the xorshift's low bits are the
    /// weakest, so a `& 1` would be a worse coin than a `>> 31`).
    bool next_bool() { return (next_uint() >> 31) != 0u; }

private:
    std::uint32_t seed_ = kDefaultSeed;
    std::uint32_t state_ = kDefaultSeed;
};

/// The stateless keyed hash: SplitMix64's finalizer applied to a mixed key.
/// `mix64(seed, index, field)` is a pure function — same arguments, same
/// answer, forever, with no ordering requirement and no state to reset.
///
/// `field` distinguishes independent draws that share an index: a grain's
/// onset jitter and its pan spray both key off the same `grain_index`, and
/// without a field id they would be perfectly correlated.
inline constexpr std::uint64_t mix64(std::uint64_t seed,
                                     std::uint64_t index,
                                     std::uint64_t field = 0u) {
    // The odd multipliers spread the three inputs across all 64 bits before the
    // finalizer runs; the finalizer itself is SplitMix64's, which passes the
    // usual avalanche tests.
    std::uint64_t z = seed + index * 0x9E3779B97F4A7C15ull + field * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/// A `mix64` word → a value uniform on `[0, 1)`. Uses the top 53 bits, which
/// are the strongest and exactly the mantissa width of a `double`.
template <typename T = float>
inline constexpr T unit_from(std::uint64_t word) {
    return static_cast<T>(static_cast<double>(word >> 11) * (1.0 / 9007199254740992.0));
}

/// A `mix64` word → a value uniform on `[-1, 1)`.
template <typename T = float>
inline constexpr T bipolar_from(std::uint64_t word) {
    return static_cast<T>(unit_from<double>(word) * 2.0 - 1.0);
}

/// An Ornstein–Uhlenbeck walk: band-limited noise that wanders but is pulled
/// back toward zero, so it never drifts away the way an integrated random walk
/// does.
///
/// This is the honest model for a mechanical imperfection — capstan wow, motor
/// speed variation, a thermal offset. A plain random walk is unbounded (its
/// variance grows without limit), and plain filtered white noise has no
/// memory; the OU process is the one that has both a correlation time and a
/// stationary distribution, which is what "it wanders around the nominal
/// value" actually means.
///
/// Discretised as `x[n+1] = x[n] + θ·(0 − x[n]) + σ_step·ξ[n]`, with ξ uniform
/// bipolar from the house generator. The stationary standard deviation of that
/// recurrence is `σ_step / sqrt(θ·(2 − θ))·σ_ξ`, so `set_sigma()` states the
/// deviation the caller wants in real units and the step size is solved for it
/// — the caller never tunes a raw noise gain.
///
/// RT contract: `next()` is a handful of arithmetic ops and one generator
/// call. No allocation, no locks.
template <typename SampleType = float>
class OuWalkT {
public:
    /// Mean-reversion rate per sample: the fraction of the remaining distance
    /// to the mean the walk closes each step. Must be in (0, 2) for the
    /// recurrence to be stable; values near 0 wander slowly, values near 1
    /// look like white noise.
    /// [design parameter] default 0.05, range 0.01 .. 0.2.
    static constexpr double kDefaultTheta = 0.05;

    /// Sets the mean-reversion rate directly. Clamped into the stable open
    /// interval — a θ of 0 would be an unbounded random walk and a θ of 2 would
    /// oscillate rather than revert.
    void set_theta(double theta) {
        theta_ = std::fmin(std::fmax(theta, 1e-6), 1.0);
        update();
    }

    /// Sets the mean-reversion rate from a **correlation time in seconds** —
    /// the more musical spelling, since "the wow wanders on a 2-second
    /// timescale" is a statement anyone can check by ear. `θ = 1 − exp(−1/(τ·fs))`,
    /// the same time-constant convention as `units::ms_to_onepole_coef`.
    void set_correlation_time(double seconds, double sample_rate) {
        if (seconds > 0.0 && sample_rate > 0.0)
            set_theta(1.0 - std::exp(-1.0 / (seconds * sample_rate)));
    }

    /// Stationary standard deviation of the walk, in whatever unit the caller
    /// is modulating (cents, percent, Hz). The step size is solved from θ so
    /// changing θ does not silently change the depth.
    void set_sigma(double sigma) {
        sigma_ = sigma >= 0.0 ? sigma : 0.0;
        update();
    }

    double theta() const { return theta_; }
    double sigma() const { return sigma_; }

    /// Seeds the generator. Per series law 2 a seed is a construction/preset
    /// choice, never an automatable parameter.
    void set_seed(std::uint32_t seed) { rng_.set_seed(seed); }

    /// Returns to a fresh state: value zeroed, generator rewound to its seed.
    void reset() {
        value_ = 0.0;
        rng_.reset();
    }

    /// Current value without advancing.
    SampleType value() const { return static_cast<SampleType>(value_); }

    /// Advances one sample and returns the new value.
    SampleType next() {
        const double xi = rng_.next_bipolar<double>();
        value_ = snap_to_zero(value_ - theta_ * value_ + step_ * xi);
        return static_cast<SampleType>(value_);
    }

private:
    void update() {
        // Stationary variance of x[n+1] = (1−θ)x[n] + s·ξ with Var(ξ) = 1/3
        // (uniform on [−1,1)) is  s²·(1/3) / (1 − (1−θ)²) = s²/(3·θ·(2−θ)).
        // Solving for s at the requested σ:  s = σ·sqrt(3·θ·(2−θ)).
        step_ = sigma_ * std::sqrt(3.0 * theta_ * (2.0 - theta_));
    }

    double theta_ = kDefaultTheta;
    double sigma_ = 0.0;
    double step_ = 0.0;
    double value_ = 0.0;
    Xorshift32 rng_{0x1F123BB5u};
};

using OuWalk = OuWalkT<float>;
using OuWalk64 = OuWalkT<double>;

/// A slow multiplicative drift: an `OuWalkT` expressed as a **ratio around 1**
/// rather than an offset around 0.
///
/// The distinction matters because the things that drift in a machine are
/// mostly rates and gains — a capstan runs 0.3 % fast, an oscillator is 4 cents
/// sharp — and those compose multiplicatively. Handing a caller an offset and
/// letting it decide how to apply it is how a 0.3 % wow ends up added to a
/// delay time in samples in one block and multiplied into it in another.
///
/// `set_depth_percent(0.3)` means "one standard deviation is 0.3 %", so
/// `next()` returns a number whose typical excursion is 1 ± 0.003.
///
/// RT contract: as `OuWalkT`.
template <typename SampleType = float>
class DriftT {
public:
    /// One-sigma depth as a percentage of the nominal value.
    /// [design parameter] default 0.1 %, range 0 .. 5 %.
    static constexpr double kDefaultDepthPercent = 0.1;

    /// Correlation time in seconds — how slowly the drift wanders.
    /// [design parameter] default 2.0 s, range 0.05 .. 30 s.
    static constexpr double kDefaultCorrelationSeconds = 2.0;

    /// A default-constructed drift is already at its documented default depth
    /// and correlation time, so a caller that only calls `prepare()` gets the
    /// specified behaviour rather than a silently inert (σ = 0) walk.
    DriftT() {
        walk_.set_sigma(depth_percent_ * 0.01);
        walk_.set_correlation_time(correlation_s_, sample_rate_);
    }

    /// Recomputes coefficients for a sample rate. Allocates nothing; safe to
    /// call from a parameter change as well as from `prepare`.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        walk_.set_correlation_time(correlation_s_, sample_rate_);
    }

    void set_depth_percent(double percent) {
        depth_percent_ = percent >= 0.0 ? percent : 0.0;
        walk_.set_sigma(depth_percent_ * 0.01);
    }

    void set_correlation_time(double seconds) {
        correlation_s_ = seconds > 0.0 ? seconds : correlation_s_;
        walk_.set_correlation_time(correlation_s_, sample_rate_);
    }

    void set_seed(std::uint32_t seed) { walk_.set_seed(seed); }

    void reset() { walk_.reset(); }

    /// Advances one sample and returns the multiplier — a number near 1.
    /// Clamped to stay positive: a drift multiplier that reached zero would
    /// stop a delay line dead, and one that went negative is meaningless.
    SampleType next() {
        const double m = 1.0 + static_cast<double>(walk_.next());
        return static_cast<SampleType>(m > kMinMultiplier ? m : kMinMultiplier);
    }

private:
    /// Floor on the drift multiplier. Only reachable at absurd depths (a 1σ of
    /// 100 % would need a ~10σ excursion to get here), but a hard floor is
    /// cheaper than reasoning about the tail.
    static constexpr double kMinMultiplier = 1e-3;

    double sample_rate_ = 44100.0;
    double depth_percent_ = kDefaultDepthPercent;
    double correlation_s_ = kDefaultCorrelationSeconds;
    OuWalkT<double> walk_{};
};

using Drift = DriftT<float>;
using Drift64 = DriftT<double>;

}  // namespace pulp::signal
