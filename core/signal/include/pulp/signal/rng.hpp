#pragma once

/// @file rng.hpp
/// Deterministic randomness for modulation: a counter-free PRNG, a Gaussian
/// draw, a stateless purpose-keyed hash, and the Ornstein-Uhlenbeck walk that
/// every "analog drift" effect in the catalog is built from.
///
/// RT contract: every type here is POD scalar state with no owned memory.
/// `prepare()` recomputes coefficients from a sample rate and is a control-side
/// call; `seed()`, `reset()`, `next*()`, and the accessors allocate nothing and
/// are audio-thread safe.
///
/// USE: modulation that must sound alive but replay identically. Randomness in
/// a plugin is only usable when it is reproducible — an A/B comparison, a
/// golden-file test, and an offline bounce all have to agree with the live
/// render. Every generator here is seeded explicitly at `reset()` and never
/// reads a clock, a device id, or `std::random_device`. Two instances with the
/// same seed produce bit-identical streams forever.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pulp::signal {

/// Xorshift32 (Marsaglia, "Xorshift RNGs", J. Stat. Soft. 8(14), 2003) with the
/// canonical 13/17/5 triple.
///
/// Zero is the generator's fixed point — a zero-initialized instance would emit
/// zeros forever — so the state is defaulted to a nonzero constant and a zero
/// state is repaired lazily on use. That keeps "zero-init = fresh" true for
/// aggregates that embed this without a constructor call.
class Xorshift32 {
public:
    /// Any nonzero constant works; this is the 32-bit golden-ratio odd constant,
    /// which is also what `seed(0)` normalizes to.
    static constexpr std::uint32_t kDefaultSeed = 0x9E3779B9u;

    constexpr Xorshift32() = default;
    constexpr explicit Xorshift32(std::uint32_t s)
        : state_(s == 0u ? kDefaultSeed : s), seed_(state_) {}

    void seed(std::uint32_t s) {
        seed_ = (s == 0u ? kDefaultSeed : s);
        state_ = seed_;
        // The Box-Muller cache holds a deviate from the OLD stream. Leaving it
        // would make the first gaussian() after a re-seed come from the
        // sequence the caller just replaced.
        cached_ = 0.0f;
        has_cached_ = false;
    }

    /// Rewind to the seeded stream position. `reset()` restores the seed the
    /// caller set, not the default one — a reset that silently discarded the
    /// seed would make every seeded object share one stream, which is the
    /// opposite of what seeding is for.
    void reset() {
        state_ = seed_ == 0u ? kDefaultSeed : seed_;
        cached_ = 0.0f;
        has_cached_ = false;
    }

    std::uint32_t next_u32() {
        std::uint32_t x = state_;
        if (x == 0u) x = kDefaultSeed;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    /// Uniform in [0, 1). Built from the top 24 bits so every result is exactly
    /// representable in `float` and the spacing is uniform.
    float next_unipolar() {
        return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f);
    }

    /// Uniform in [-1, 1).
    float next_bipolar() { return next_unipolar() * 2.0f - 1.0f; }

    /// Standard normal via the polar-free Box-Muller transform (Box & Muller,
    /// Ann. Math. Statist. 29(2), 1958). The transform produces two independent
    /// deviates per pair of uniforms; the second is cached so the amortized cost
    /// is one `log`/`sqrt`/`sincos` pair every other call.
    ///
    /// `u1` is drawn on (0, 1] rather than [0, 1) — `log(0)` is the one input
    /// that breaks the transform.
    float gaussian() {
        if (has_cached_) {
            has_cached_ = false;
            return cached_;
        }
        float u1 = next_unipolar();
        if (u1 <= 0.0f) u1 = 1.0f / 16777216.0f;
        const float u2 = next_unipolar();
        const float radius = std::sqrt(-2.0f * std::log(u1));
        const float angle = 6.2831853071795864769f * u2;
        cached_ = radius * std::sin(angle);
        has_cached_ = true;
        return radius * std::cos(angle);
    }

    std::uint32_t state() const { return state_ == 0u ? kDefaultSeed : state_; }

    std::uint32_t seed() const { return seed_ == 0u ? kDefaultSeed : seed_; }

private:
    std::uint32_t state_ = kDefaultSeed;
    std::uint32_t seed_ = kDefaultSeed;
    float cached_ = 0.0f;
    bool has_cached_ = false;
};

/// splitmix64 finalizer (Steele, Lea & Flood, OOPSLA 2014). A bijective
/// avalanche mix, not a generator: it has no state, so it turns any key into a
/// well-distributed value.
constexpr std::uint64_t mix64(std::uint64_t z) {
    z ^= z >> 30;
    z *= 0xBF58476D1CE4E5B9ull;
    z ^= z >> 27;
    z *= 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z;
}

/// Stateless purpose-keyed draw in [0, 1).
///
/// USE: per-voice / per-partial / per-tap detune and offsets. Deriving them
/// from `unit_from(rng_key(purpose, index))` instead of a shared stream makes
/// each one independent of *when* it was requested, so adding a voice or
/// reordering initialisation cannot shuffle every other value.
inline float unit_from(std::uint64_t key) {
    return static_cast<float>(mix64(key) >> 40) * (1.0f / 16777216.0f);
}

/// The 64-bit golden-ratio odd constant that splitmix64 steps its state by.
inline constexpr std::uint64_t kGoldenGamma = 0x9E3779B97F4A7C15ull;

/// Combine a purpose tag and an index into a key for `unit_from()`.
///
/// The golden constant is added, not just multiplied by: `mix64` is a bijection
/// and therefore maps 0 to 0, so without it the natural first key — purpose 0,
/// index 0 — would land exactly on the finalizer's fixed point and draw 0.
constexpr std::uint64_t rng_key(std::uint64_t purpose, std::uint64_t index) {
    return mix64(purpose * kGoldenGamma + index + kGoldenGamma);
}

/// Ornstein-Uhlenbeck walk (Uhlenbeck & Ornstein, Phys. Rev. 36, 1930):
///
///     y += (1 - e^(-theta * dt)) * (mu - y) + sigma * sqrt(dt) * g
///
/// The drift term uses the exact per-step decay factor rather than the naive
/// Euler-Maruyama `theta * dt`, which matches it for small products but goes
/// unstable once `theta * dt` exceeds 1 — see the note in `next()`.
///
/// A random walk with a restoring force. Unlike filtered white noise it has a
/// defined mean it returns to, and unlike an LFO it never repeats. The
/// stationary standard deviation is `sigma / sqrt(2 * theta)`.
///
/// RT contract: `prepare()` recomputes `dt` and the noise scale; `next()`,
/// `reset()`, and accessors allocate nothing.
///
/// USE: the one primitive behind tape wow, filter-cutoff drift, and
/// feedback-network delay-length walks. Anywhere the answer to "why does this
/// sound static?" is "because nothing is moving that the listener did not ask
/// for". `theta` is how hard it is pulled home (small = long slow excursions),
/// `sigma` is how far it gets. Output is clamped to +/-`kClamp` so a large
/// `sigma` cannot hand a downstream pitch or delay control an absurd value.
template <typename SampleType = float>
class OuWalkT {
public:
    /// A Gaussian walk has unbounded tails; downstream consumers (pitch ratios,
    /// delay lengths) do not. 1.5 sits far enough out that clipping is rare at
    /// sane settings but the bound still exists.
    static constexpr SampleType kClamp = SampleType{1.5};

    /// @param update_rate_hz Rate at which `next()` will be called — the audio
    ///        sample rate for per-sample use, or the control rate for a
    ///        decimated one. The walk's statistics are rate-independent.
    void prepare(double update_rate_hz) {
        rate_ = update_rate_hz > 0.0 ? update_rate_hz : 1.0;
        dt_ = 1.0 / rate_;
        sqrt_dt_ = std::sqrt(dt_);
        update_drift_coeff_();
    }

    void set_theta(double theta_per_second) {
        theta_ = std::max(0.0, theta_per_second);
        update_drift_coeff_();
    }
    void set_sigma(double sigma) { sigma_ = std::max(0.0, sigma); }
    void set_mu(double mu) { mu_ = mu; }
    void seed(std::uint32_t s) { rng_.seed(s); }

    void reset() {
        value_ = mu_;
        rng_.reset();
    }

    /// Reset to the mean without disturbing the stream position.
    void reset_value() { value_ = mu_; }

    SampleType next() {
        // The drift uses the exact per-step decay factor `1 - e^(-theta*dt)`
        // rather than the naive Euler `theta * dt`. The two agree when
        // `theta * dt` is small, but Euler overshoots the mean once the
        // product passes 1 and turns the walk into a clamp-to-clamp
        // oscillator past 2 — reachable with a hard pull at a low update
        // rate. The exact factor is a fraction in (0, 1] for every theta, so
        // the pull is unconditionally stable.
        const double drift = drift_coeff_ * (mu_ - value_);
        const double diffusion = sigma_ * sqrt_dt_ * static_cast<double>(rng_.gaussian());
        value_ = std::clamp(value_ + drift + diffusion,
                            -static_cast<double>(kClamp),
                            static_cast<double>(kClamp));
        return static_cast<SampleType>(value_);
    }

    SampleType current() const { return static_cast<SampleType>(value_); }

    /// Closed-form stationary standard deviation, for tests and for callers
    /// sizing a modulation depth from a target excursion.
    double stationary_std() const {
        return theta_ > 0.0 ? sigma_ / std::sqrt(2.0 * theta_) : 0.0;
    }

private:
    void update_drift_coeff_() { drift_coeff_ = -std::expm1(-theta_ * dt_); }

    Xorshift32 rng_{};
    double value_ = 0.0;
    double mu_ = 0.0;
    double theta_ = 1.0;
    double sigma_ = 0.1;
    double rate_ = 48000.0;
    double dt_ = 1.0 / 48000.0;
    double sqrt_dt_ = 1.0 / 219.089023002066;
    // Matches the default theta and dt; `prepare()` recomputes it exactly.
    double drift_coeff_ = 1.0 / 48000.0;
};

using OuWalk = OuWalkT<float>;
using OuWalk64 = OuWalkT<double>;

/// Two decorrelated OU walks presented as the two things drift is actually
/// wired to: a pitch ratio and a unit fraction.
///
/// The walks are stepped once every `kDecimation` samples and interpolated
/// between control points. An OU walk's spectrum is already low-pass — running
/// it per sample costs a `log`, a `sqrt`, and a `sincos` for detail far above
/// where drift lives — so the decimation is free in character and 32x in cost.
/// The interpolation matters: without it the stepped control is a staircase,
/// and a staircase on a delay length or an oscillator frequency is audible as
/// zipper noise, not as drift.
///
/// RT contract: `prepare()` is control-side. `next()`, `reset()`, and the
/// accessors allocate nothing.
///
/// USE: `pitch_factor()` multiplies an oscillator frequency or a resampling
/// ratio; `fraction()` is a signed unit control for anything else (filter
/// cutoff in cents, a delay-time percentage, a stereo width). Set
/// `set_cents(3)` for "this is a good tape machine", 15 for "this one needs
/// service", 40 for seasick.
template <typename SampleType = float>
class DriftT {
public:
    /// Control-rate decimation. 32 samples is 0.67 ms at 48 kHz — two orders of
    /// magnitude faster than the fastest drift anyone wants.
    static constexpr int kDecimation = 32;

    void prepare(double sample_rate) {
        const double sr = sample_rate > 0.0 ? sample_rate : 1.0;
        const double control_rate = sr / static_cast<double>(kDecimation);
        pitch_walk_.prepare(control_rate);
        fraction_walk_.prepare(control_rate);
        reset();
    }

    /// Peak-ish pitch excursion in cents, scaled by the walk's unit output.
    void set_cents(double cents) { cents_ = cents; }

    void set_theta(double theta_per_second) {
        pitch_walk_.set_theta(theta_per_second);
        fraction_walk_.set_theta(theta_per_second);
    }

    void set_sigma(double sigma) {
        pitch_walk_.set_sigma(sigma);
        fraction_walk_.set_sigma(sigma);
    }

    /// The two walks are seeded from one user-facing seed through `mix64` so
    /// they are decorrelated by construction; seeding them `s` and `s + 1`
    /// would leave them visibly related for the first few draws.
    void seed(std::uint32_t s) {
        pitch_walk_.seed(static_cast<std::uint32_t>(mix64(s) & 0xFFFFFFFFu));
        fraction_walk_.seed(static_cast<std::uint32_t>(mix64(s ^ 0xA5A5A5A5u) >> 32));
    }

    void reset() {
        pitch_walk_.reset();
        fraction_walk_.reset();
        counter_ = 0;
        pitch_prev_ = pitch_next_ = pitch_walk_.current();
        fraction_prev_ = fraction_next_ = fraction_walk_.current();
        pitch_now_ = pitch_prev_;
        fraction_now_ = fraction_prev_;
    }

    /// Advance one audio sample. Call once per sample; read the outputs after.
    void next() {
        if (counter_ == 0) {
            pitch_prev_ = pitch_next_;
            fraction_prev_ = fraction_next_;
            pitch_next_ = pitch_walk_.next();
            fraction_next_ = fraction_walk_.next();
        }
        const SampleType t = static_cast<SampleType>(counter_)
                             / static_cast<SampleType>(kDecimation);
        pitch_now_ = pitch_prev_ + (pitch_next_ - pitch_prev_) * t;
        fraction_now_ = fraction_prev_ + (fraction_next_ - fraction_prev_) * t;
        if (++counter_ >= kDecimation) counter_ = 0;
    }

    /// Multiplicative pitch ratio: `2 ^ (cents * x / 1200)`.
    SampleType pitch_factor() const {
        return static_cast<SampleType>(
            std::exp2(cents_ * static_cast<double>(pitch_now_) / 1200.0));
    }

    /// Signed unit drift, independent of the pitch walk. Clamped to [-1, 1]:
    /// the underlying walk is allowed out to `OuWalkT::kClamp` for pitch
    /// headroom, but the unit contract is what downstream consumers — delay
    /// percentages, cutoff offsets — size against.
    SampleType fraction() const {
        return std::clamp(fraction_now_, SampleType{-1}, SampleType{1});
    }

private:
    OuWalkT<SampleType> pitch_walk_{};
    OuWalkT<SampleType> fraction_walk_{};
    double cents_ = 5.0;
    int counter_ = 0;
    SampleType pitch_prev_{};
    SampleType pitch_next_{};
    SampleType fraction_prev_{};
    SampleType fraction_next_{};
    SampleType pitch_now_{};
    SampleType fraction_now_{};
};

using Drift = DriftT<float>;
using Drift64 = DriftT<double>;

} // namespace pulp::signal
