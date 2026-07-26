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

    /// Compatibility spelling used by the Round 2 DSP blocks.
    void set_seed(std::uint32_t s) { seed(s); }

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

    /// Compatibility spelling used by the Round 2 DSP blocks.
    std::uint32_t next_uint() { return next_u32(); }

    /// Uniform in [0, 1). Built from the top 24 bits so every result is exactly
    /// representable in `float` and the spacing is uniform.
    float next_unipolar() {
        return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f);
    }

    /// Uniform in [-1, 1).
    float next_bipolar() { return next_unipolar() * 2.0f - 1.0f; }

    template <typename T>
    T next_bipolar() {
        return static_cast<T>(static_cast<double>(next_u32()) *
                                  (1.0 / 2147483648.0) -
                              1.0);
    }

    /// Uniform in [0, 1), with the caller-selected result precision.
    float next_unit() { return next_unipolar(); }

    template <typename T>
    T next_unit() {
        return static_cast<T>(static_cast<double>(next_u32()) *
                              (1.0 / 4294967296.0));
    }

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

/// Purpose-keyed overload used when seed, index, and field are already
/// separate values. It remains a pure stateless draw.
constexpr std::uint64_t mix64(std::uint64_t seed,
                              std::uint64_t index,
                              std::uint64_t field = 0u) {
    return mix64(seed + index * 0x9E3779B97F4A7C15ull +
                 field * 0xBF58476D1CE4E5B9ull);
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

template <typename T>
inline T unit_from(std::uint64_t key) {
    return static_cast<T>(static_cast<double>(mix64(key) >> 11) *
                          (1.0 / 9007199254740992.0));
}

template <typename T = float>
inline T bipolar_from(std::uint64_t key) {
    return static_cast<T>(unit_from<double>(key) * 2.0 - 1.0);
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
///     y[n+1] = mu + e^(-theta*dt) * (y[n] - mu)
///              + sigma * sqrt(1 - e^(-2*theta*dt)) * g
///
/// This is the exact discrete transition, not Euler-Maruyama: both the decay
/// and innovation scale are solved for the configured update interval.
///
/// A random walk with a restoring force. Unlike filtered white noise it has a
/// defined mean it returns to, and unlike an LFO it never repeats. The
/// stationary standard deviation is the caller-facing `sigma`, independent of
/// update rate and theta.
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
    static constexpr double kMinTheta = 1e-6;
    static constexpr double kMaxTheta = 1.0;
    static constexpr double kDefaultTheta = 0.05;

    /// @param update_rate_hz Rate at which `next()` will be called — the audio
    ///        sample rate for per-sample use, or the control rate for a
    ///        decimated one. The walk's statistics are rate-independent.
    void prepare(double update_rate_hz) {
        rate_ = update_rate_hz > 0.0 ? update_rate_hz : 1.0;
        dt_ = 1.0 / rate_;
        update_coefficients_();
    }

    void set_theta(double theta_per_second) {
        if (!std::isfinite(theta_per_second)) return;
        theta_ = std::clamp(theta_per_second, kMinTheta, kMaxTheta);
        update_coefficients_();
    }
    void set_sigma(double sigma) {
        if (!std::isfinite(sigma)) return;
        sigma_ = std::max(0.0, sigma);
        update_coefficients_();
    }
    void set_mu(double mu) {
        if (std::isfinite(mu)) mu_ = mu;
    }
    void seed(std::uint32_t s) { rng_.seed(s); }
    void set_seed(std::uint32_t s) { seed(s); }

    void set_correlation_time(double seconds, double sample_rate) {
        if (!(std::isfinite(seconds) && seconds > 0.0 &&
              std::isfinite(sample_rate) && sample_rate > 0.0))
            return;
        prepare(sample_rate);
        set_theta(1.0 / seconds);
    }

    void reset() {
        value_ = mu_;
        rng_.reset();
    }

    /// Reset to the mean without disturbing the stream position.
    void reset_value() { value_ = mu_; }

    SampleType next() {
        // Exact transition: the decay is always in (0, 1), and the innovation
        // scale preserves the requested stationary deviation at every rate.
        const double diffusion = innovation_std_ * static_cast<double>(rng_.gaussian());
        value_ = std::clamp(mu_ + decay_ * (value_ - mu_) + diffusion,
                            -static_cast<double>(kClamp),
                            static_cast<double>(kClamp));
        return static_cast<SampleType>(value_);
    }

    SampleType current() const { return static_cast<SampleType>(value_); }
    SampleType value() const { return current(); }
    double theta() const { return theta_; }
    double sigma() const { return sigma_; }

    /// Closed-form stationary standard deviation, for tests and for callers
    /// sizing a modulation depth from a target excursion.
    double stationary_std() const {
        return sigma_;
    }

private:
    void update_coefficients_() {
        decay_ = std::exp(-theta_ * dt_);
        // Exact discrete-time OU transition. `sigma_` is the requested
        // stationary standard deviation, so changing update rate or theta does
        // not silently change the modulation depth.
        innovation_std_ = sigma_ * std::sqrt(std::max(0.0, 1.0 - decay_ * decay_));
    }

    Xorshift32 rng_{};
    double value_ = 0.0;
    double mu_ = 0.0;
    double theta_ = kDefaultTheta;
    double sigma_ = 0.1;
    // An unprepared walk advances in unit time. Audio/control-rate users call
    // `prepare(rate)`, while the direct per-step Round 2 surface can state
    // theta without inheriting an arbitrary hidden 48 kHz assumption.
    double rate_ = 1.0;
    double dt_ = 1.0;
    double decay_ = std::exp(-kDefaultTheta);
    double innovation_std_ =
        0.1 * std::sqrt(1.0 - std::exp(-2.0 * kDefaultTheta));
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
    static constexpr double kDefaultDepthPercent = 0.1;
    static constexpr double kDefaultCorrelationSeconds = 2.0;

    void prepare(double sample_rate) {
        const double sr = sample_rate > 0.0 ? sample_rate : 1.0;
        const double control_rate = sr / static_cast<double>(kDecimation);
        pitch_walk_.prepare(control_rate);
        fraction_walk_.prepare(control_rate);
        multiplier_walk_.prepare(control_rate);
        multiplier_walk_.set_theta(1.0 / correlation_s_);
        multiplier_walk_.set_sigma(depth_percent_ * 0.01);
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
        multiplier_walk_.seed(static_cast<std::uint32_t>(mix64(s ^ 0xC6BC2796u) & 0xFFFFFFFFu));
    }

    void set_seed(std::uint32_t s) { seed(s); }

    void set_depth_percent(double percent) {
        if (!std::isfinite(percent)) return;
        depth_percent_ = std::max(0.0, percent);
        multiplier_walk_.set_sigma(depth_percent_ * 0.01);
    }

    void set_correlation_time(double seconds) {
        if (!(std::isfinite(seconds) && seconds > 0.0)) return;
        correlation_s_ = seconds;
        multiplier_walk_.set_theta(1.0 / correlation_s_);
    }

    void reset() {
        pitch_walk_.reset();
        fraction_walk_.reset();
        multiplier_walk_.reset();
        counter_ = 0;
        pitch_prev_ = pitch_next_ = pitch_walk_.current();
        fraction_prev_ = fraction_next_ = fraction_walk_.current();
        multiplier_prev_ = multiplier_next_ = multiplier_walk_.current();
        pitch_now_ = pitch_prev_;
        fraction_now_ = fraction_prev_;
        multiplier_now_ = multiplier_prev_;
    }

    /// Advance one audio sample. Call once per sample; read the outputs after.
    SampleType next() {
        if (counter_ == 0) {
            pitch_prev_ = pitch_next_;
            fraction_prev_ = fraction_next_;
            pitch_next_ = pitch_walk_.next();
            fraction_next_ = fraction_walk_.next();
            multiplier_prev_ = multiplier_next_;
            multiplier_next_ = multiplier_walk_.next();
        }
        const SampleType t = static_cast<SampleType>(counter_)
                             / static_cast<SampleType>(kDecimation);
        pitch_now_ = pitch_prev_ + (pitch_next_ - pitch_prev_) * t;
        fraction_now_ = fraction_prev_ + (fraction_next_ - fraction_prev_) * t;
        multiplier_now_ = multiplier_prev_ + (multiplier_next_ - multiplier_prev_) * t;
        if (++counter_ >= kDecimation) counter_ = 0;
        return static_cast<SampleType>(
            std::max(kMinMultiplier, 1.0 + static_cast<double>(multiplier_now_)));
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
    static constexpr double kMinMultiplier = 1e-3;
    OuWalkT<SampleType> pitch_walk_{};
    OuWalkT<SampleType> fraction_walk_{};
    OuWalkT<SampleType> multiplier_walk_{};
    double cents_ = 5.0;
    double depth_percent_ = kDefaultDepthPercent;
    double correlation_s_ = kDefaultCorrelationSeconds;
    int counter_ = 0;
    SampleType pitch_prev_{};
    SampleType pitch_next_{};
    SampleType fraction_prev_{};
    SampleType fraction_next_{};
    SampleType pitch_now_{};
    SampleType fraction_now_{};
    SampleType multiplier_prev_{};
    SampleType multiplier_next_{};
    SampleType multiplier_now_{};
};

using Drift = DriftT<float>;
using Drift64 = DriftT<double>;

} // namespace pulp::signal
