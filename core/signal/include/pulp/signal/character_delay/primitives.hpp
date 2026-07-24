#pragma once

// Shared building blocks for the multi-character delay (character_delay.hpp).
//
// Why these live here rather than being pulled from the wider signal library:
// every block in this file is used INSIDE a recirculating feedback loop, where
// a repeat is processed once per pass and coloration compounds exponentially.
// That imposes two requirements the general-purpose blocks do not make:
//
//   * Double-precision state. A 20 Hz highpass at 96 kHz has a pole close
//     enough to z = 1 that float state accumulates audible error after a few
//     hundred recirculations; the delay READ POSITION likewise needs double so
//     a 2 s line still resolves sub-sample motion. Audio STORAGE stays float
//     (the buffers dominate the module's memory), state does not.
//   * Exactly invertible shelves. The emphasis pairs (tape record/playback EQ,
//     vintage pre/de-emphasis) exist to bracket a nonlinearity: whatever the
//     shelf does on the way in must be undone exactly on the way out, or the
//     residual error is itself compounded per repeat. A naive "same corner,
//     negated dB" mirror does not invert a first-order shelf; see
//     FirstOrderShelf::set_inverse for the pole/zero swap that does.
//
// All filters are TPT/Zavalishin trapezoidal forms (Zavalishin, *The Art of VA
// Filter Design*; Simper/Cytomic's trapezoidal SVF note for the 2-pole mixing
// coefficients), which stay well-behaved under per-sample coefficient
// modulation — the delay's cutoffs move with delay time (BBD) and with the
// character macro.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal::chardelay {

inline constexpr double kPi = 3.14159265358979323846;

// Denormal guard for IIR state. -360 dBFS is far below any audible or
// measurable level, so flushing below it is free of audio consequence while
// keeping the loop off the denormal slow path during long decays.
inline constexpr double kDenormalFloor = 1e-18;

inline double flush_denormal(double v) noexcept {
    return (v <= kDenormalFloor && v >= -kDenormalFloor) ? 0.0 : v;
}

// ── Loop saturator ────────────────────────────────────────────────────────
// Padé-form rational approximation of tanh (classic public-domain numerical
// approximation). At |v| = 3 it evaluates to exactly ±1, which is also where
// tanh has effectively reached its asymptote — so clamping the argument there
// makes the function both continuous AND bounded to ±1. The unclamped rational
// is asymptotically v/9, i.e. NOT bounded; the clamp is what lets a >unity
// feedback loop (the deliberate self-oscillating dub behavior) settle at a
// bounded level instead of merely decaying more slowly than it grows.
inline double fast_tanh(double v) noexcept {
    const double x = std::clamp(v, -3.0, 3.0);
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

// ── Deterministic randomness ──────────────────────────────────────────────
// xorshift32 (Marsaglia 2003). Per-instance state, re-seeded on reset(), so
// two renders from a fresh reset() are bit-identical — the determinism the
// acceptance suite asserts. Streams that must not correlate (hiss vs chew vs
// clock jitter, left vs right) each own an instance with a distinct seed.
class Xorshift32 {
public:
    explicit Xorshift32(std::uint32_t seed = 74207u) : state_(seed | 1u), seed_(seed | 1u) {}

    void reseed(std::uint32_t seed) noexcept {
        seed_ = seed | 1u;
        state_ = seed_;
        gauss_valid_ = false;
    }

    void reset() noexcept {
        state_ = seed_;
        gauss_valid_ = false;
    }

    std::uint32_t next() noexcept {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }

    /// Uniform in [0, 1].
    double uniform() noexcept {
        return static_cast<double>(next()) / static_cast<double>(UINT32_MAX);
    }

    /// Uniform in [-1, 1].
    double bipolar() noexcept { return uniform() * 2.0 - 1.0; }

    /// Standard normal via Box-Muller (Box & Muller 1958). The transform
    /// produces two independent deviates per pair of draws; the second is
    /// cached so the PRNG consumption rate stays one draw per sample on
    /// average, which keeps long-run stream alignment stable.
    double gaussian() noexcept {
        if (gauss_valid_) {
            gauss_valid_ = false;
            return gauss_spare_;
        }
        double u1 = uniform();
        if (u1 <= 0.0) u1 = 1.0 / static_cast<double>(UINT32_MAX);  // guard log(0)
        const double u2 = uniform();
        const double r = std::sqrt(-2.0 * std::log(u1));
        const double a = 2.0 * kPi * u2;
        gauss_spare_ = r * std::sin(a);
        gauss_valid_ = true;
        return r * std::cos(a);
    }

private:
    std::uint32_t state_;
    std::uint32_t seed_;
    double gauss_spare_ = 0.0;
    bool gauss_valid_ = false;
};

// ── Calibration-table interpolation ───────────────────────────────────────
// Every character's tuning lives in a knot table (see tables.hpp): an ascending
// c01 axis plus one value row per tuned quantity. Reading through this function
// is what makes retuning a data edit rather than a logic edit.
template <std::size_t N>
inline double interpolate_knots(const std::array<double, N>& axis,
                                const std::array<double, N>& values, double c) noexcept {
    static_assert(N >= 2, "a knot table needs at least two knots");
    if (c <= axis[0]) return values[0];
    for (std::size_t i = 1; i < N; ++i) {
        if (c <= axis[i]) {
            const double span = axis[i] - axis[i - 1];
            const double t = span > 0.0 ? (c - axis[i - 1]) / span : 0.0;
            return values[i - 1] + t * (values[i] - values[i - 1]);
        }
    }
    return values[N - 1];
}

// ── One-pole parameter glide ──────────────────────────────────────────────
// A single exponential smoother, used for both the ~5 ms de-zipper on scalar
// params and the per-character delay-time slew (which is not de-zippering at
// all but modeled transport inertia — the reason a tape time change swoops).
class Smoother {
public:
    void configure(double time_constant_s, double fs) noexcept {
        coefficient_ = (time_constant_s <= 0.0 || fs <= 0.0)
                           ? 0.0
                           : std::exp(-1.0 / (time_constant_s * fs));
    }

    void snap(double v) noexcept { current_ = v; }
    double current() const noexcept { return current_; }

    double process(double target) noexcept {
        current_ = target + coefficient_ * (current_ - target);
        return current_;
    }

private:
    double coefficient_ = 0.0;
    double current_ = 0.0;
};

// ── Fractional delay line ─────────────────────────────────────────────────
// Circular buffer, float storage, 3rd-order (4-point) Lagrange fractional read
// (Laakso et al. 1996; Dattorro 1997 Part 2) — flat passband magnitude at a
// cost that suits a per-sample-modulated read position.
//
// Kernel centering: the four taps sit at delays {D-1, D, D+1, D+2}, so a
// fractional part in [0, 1) already places the read point in the CENTRAL
// D..D+1 segment, which is where a Lagrange kernel is most accurate and where
// its response is continuous as the read position sweeps across integer
// boundaries. Shifting the window by another sample (a re-centering step that
// is correct for tap layouts anchored differently) would move the read point
// into the outer segment and reintroduce the kernel-edge discontinuity it is
// meant to remove, so this implementation does not do it.
class FractionalDelayLine {
public:
    /// Allocates. Control thread only.
    void prepare(std::size_t capacity_samples) {
        capacity_ = std::max<std::size_t>(capacity_samples, 8u);
        buffer_.assign(capacity_, 0.0f);
        write_ = 0;
    }

    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.0f);
        write_ = 0;
    }

    std::size_t capacity() const noexcept { return capacity_; }

    /// Largest delay the Lagrange kernel can address without walking off the
    /// buffer (the top three slots are the kernel's forward reach).
    double max_delay() const noexcept {
        return capacity_ > 4 ? static_cast<double>(capacity_ - 4) : 1.0;
    }

    void push(double x) noexcept {
        buffer_[write_] = static_cast<float>(x);
        if (++write_ >= capacity_) write_ = 0;
    }

    /// Sample `m` samples in the past; m = 0 is the most recent push().
    double tap(std::size_t m) const noexcept {
        const std::size_t index = (write_ + capacity_ - 1u - m) % capacity_;
        return static_cast<double>(buffer_[index]);
    }

    double read(double delay_samples) const noexcept {
        const double d = std::clamp(delay_samples, 1.0, max_delay());
        const auto integer = static_cast<std::size_t>(d);
        const double frac = d - static_cast<double>(integer);

        const double s0 = tap(integer - 1u);
        const double s1 = tap(integer);
        const double s2 = tap(integer + 1u);
        const double s3 = tap(integer + 2u);

        const double d0 = frac + 1.0;
        const double d1 = frac;
        const double d2 = frac - 1.0;
        const double d3 = frac - 2.0;

        return (-d1 * d2 * d3 / 6.0) * s0 + (d0 * d2 * d3 * 0.5) * s1 +
               (-d0 * d1 * d3 * 0.5) * s2 + (d0 * d1 * d2 / 6.0) * s3;
    }

    /// Linear interpolation between adjacent taps. Used by the vintage
    /// character's internal-rate line, where the signal has already been
    /// band-limited to 0.45× that grid and a higher-order kernel would model
    /// precision the modeled converter never had.
    double read_linear(double delay_samples) const noexcept {
        const double d = std::clamp(delay_samples, 0.0, max_delay());
        const auto integer = static_cast<std::size_t>(d);
        const double frac = d - static_cast<double>(integer);
        const double s0 = tap(integer);
        const double s1 = tap(integer + 1u);
        return s0 + frac * (s1 - s0);
    }

private:
    std::vector<float> buffer_;
    std::size_t capacity_ = 8;
    std::size_t write_ = 0;
};

// ── 1-pole TPT filter ─────────────────────────────────────────────────────
// Produces lowpass and highpass from one state. Used for DC blocking, loop
// damping, and as the pole of the first-order shelves below.
class OnePole {
public:
    void set_cutoff(double cutoff_hz, double fs) noexcept {
        const double nyquist_guard = 0.49 * fs;
        const double fc = std::clamp(cutoff_hz, 0.1, nyquist_guard);
        const double g = std::tan(kPi * fc / fs);
        gain_ = g / (1.0 + g);
    }

    void reset() noexcept { state_ = 0.0; }

    double lowpass(double x) noexcept {
        const double v = (x - state_) * gain_;
        const double y = v + state_;
        state_ = flush_denormal(y + v);
        return y;
    }

    double highpass(double x) noexcept { return x - lowpass(x); }

private:
    double gain_ = 0.5;
    double state_ = 0.0;
};

// ── First-order shelf, exactly invertible ─────────────────────────────────
// H(s) = (1 + G·τs) / (1 + τs), τ = 1/(2π·fc): unity at DC, gain G above fc.
// Realized as lowpass + G·highpass off one TPT pole.
//
// The exact inverse is (1 + τs)/(1 + G·τs) — the pole and zero swapped, which
// as a shelf is gain 1/G with its pole at fc/G. set_inverse() applies that
// substitution, so a boost/cut pair cancels to unity everywhere instead of
// leaving a residual mid-band bump for the feedback loop to compound.
class FirstOrderShelf {
public:
    void set(double corner_hz, double gain_linear, double fs) noexcept {
        gain_ = gain_linear;
        pole_.set_cutoff(corner_hz, fs);
    }

    /// Configure as the exact inverse of set(corner_hz, gain_linear, fs).
    void set_inverse(double corner_hz, double gain_linear, double fs) noexcept {
        const double g = (gain_linear > 1e-9) ? gain_linear : 1e-9;
        set(corner_hz / g, 1.0 / g, fs);
    }

    void reset() noexcept { pole_.reset(); }

    double process(double x) noexcept {
        const double low = pole_.lowpass(x);
        return low + gain_ * (x - low);
    }

private:
    OnePole pole_;
    double gain_ = 1.0;
};

// ── 2-pole TPT state-variable filter ──────────────────────────────────────
// Simper/Cytomic trapezoidal SVF. One structure, several responses selected by
// the (m0, m1, m2) output mix — including the bell used for the tape head bump
// and the Butterworth sections of the vintage converter's anti-alias cascade.
class Svf2 {
public:
    void set_lowpass(double cutoff_hz, double q, double fs) noexcept {
        configure(std::tan(kPi * clamp_cutoff(cutoff_hz, fs) / fs), 1.0 / std::max(q, 1e-4));
        m0_ = 0.0;
        m1_ = 0.0;
        m2_ = 1.0;
    }

    void set_highpass(double cutoff_hz, double q, double fs) noexcept {
        configure(std::tan(kPi * clamp_cutoff(cutoff_hz, fs) / fs), 1.0 / std::max(q, 1e-4));
        m0_ = 1.0;
        m1_ = -k_;
        m2_ = -1.0;
    }

    /// Peaking ("bell") section: `gain_db` at `cutoff_hz`, unity far away.
    void set_bell(double cutoff_hz, double q, double gain_db, double fs) noexcept {
        const double a = std::pow(10.0, gain_db / 40.0);
        configure(std::tan(kPi * clamp_cutoff(cutoff_hz, fs) / fs),
                  1.0 / (std::max(q, 1e-4) * a));
        m0_ = 1.0;
        m1_ = k_ * (a * a - 1.0);
        m2_ = 0.0;
    }

    void reset() noexcept {
        ic1_ = 0.0;
        ic2_ = 0.0;
    }

    double process(double v0) noexcept {
        const double v3 = v0 - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = flush_denormal(2.0 * v1 - ic1_);
        ic2_ = flush_denormal(2.0 * v2 - ic2_);
        return m0_ * v0 + m1_ * v1 + m2_ * v2;
    }

private:
    static double clamp_cutoff(double cutoff_hz, double fs) noexcept {
        return std::clamp(cutoff_hz, 1.0, 0.49 * fs);
    }

    void configure(double g, double k) noexcept {
        g_ = g;
        k_ = k;
        a1_ = 1.0 / (1.0 + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    double g_ = 0.0, k_ = 1.0;
    double a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
    double m0_ = 0.0, m1_ = 0.0, m2_ = 1.0;
    double ic1_ = 0.0, ic2_ = 0.0;
};

// ── Butterworth lowpass cascade ───────────────────────────────────────────
// Three 2-pole SVF sections at the analytic 6th-order Butterworth pole Qs,
// Q_k = 1/(2·cos((2k+1)π/12)). These are textbook pole placements, not tuned
// values. Used as the vintage converter's matched anti-alias / reconstruction
// pair, where a gentler rolloff would let the internal grid alias audibly.
class Butterworth6Lowpass {
public:
    static constexpr std::array<double, 3> kSectionQ = {0.5176380902, 0.7071067812,
                                                        1.9318516526};

    void set_cutoff(double cutoff_hz, double fs) noexcept {
        for (std::size_t i = 0; i < sections_.size(); ++i)
            sections_[i].set_lowpass(cutoff_hz, kSectionQ[i], fs);
    }

    void reset() noexcept {
        for (auto& s : sections_) s.reset();
    }

    double process(double x) noexcept {
        for (auto& s : sections_) x = s.process(x);
        return x;
    }

private:
    std::array<Svf2, 3> sections_;
};

// ── FIR with externally owned coefficients ────────────────────────────────
// The wider signal library's FIR owns (and reallocates) its coefficients,
// which is the right shape when the coefficients are set once. The physical
// tape tier's loss filter is different: its coefficients are re-derived at
// CONTROL RATE by interpolating pre-designed banks, and during a tape-speed
// change two coefficient sets are convolved against the same history so the
// banks can be crossfaded instead of swapped. Separating the history from the
// coefficients makes both of those allocation-free.
class FixedFir {
public:
    /// Allocates. Control thread only.
    void prepare(std::size_t max_taps) {
        capacity_ = std::max<std::size_t>(max_taps, 1u);
        history_.assign(capacity_, 0.0);
        write_ = 0;
    }

    void reset() noexcept {
        std::fill(history_.begin(), history_.end(), 0.0);
        write_ = 0;
    }

    void push(double x) noexcept {
        history_[write_] = x;
        if (++write_ >= capacity_) write_ = 0;
    }

    double convolve(const double* coefficients, std::size_t taps) const noexcept {
        const std::size_t n = std::min(taps, capacity_);
        double sum = 0.0;
        std::size_t index = write_;
        for (std::size_t i = 0; i < n; ++i) {
            index = (index == 0) ? capacity_ - 1u : index - 1u;
            sum += coefficients[i] * history_[index];
        }
        return sum;
    }

private:
    std::vector<double> history_;
    std::size_t capacity_ = 1;
    std::size_t write_ = 0;
};

// ── Attack/release envelope follower ──────────────────────────────────────
// Peak-fed, asymmetric one-pole. Used by the ducker's sidechain and by the
// BBD compander's rectifier loop.
class EnvelopeFollower {
public:
    void configure(double attack_s, double release_s, double rate_hz) noexcept {
        attack_ = (attack_s <= 0.0) ? 0.0 : std::exp(-1.0 / (attack_s * rate_hz));
        release_ = (release_s <= 0.0) ? 0.0 : std::exp(-1.0 / (release_s * rate_hz));
    }

    void reset(double value = 0.0) noexcept { env_ = value; }
    double value() const noexcept { return env_; }

    double process(double magnitude) noexcept {
        const double c = (magnitude > env_) ? attack_ : release_;
        env_ = magnitude + c * (env_ - magnitude);
        return env_;
    }

private:
    double attack_ = 0.0;
    double release_ = 0.0;
    double env_ = 0.0;
};

}  // namespace pulp::signal::chardelay
