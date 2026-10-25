#pragma once

// In-loop tone shaping: a 10-band EQ whose coefficients are shared across the
// 16 lines, and the per-channel "flux" peak that wanders.
//
// Two properties make this safe to put INSIDE a recursion:
//
//  * Shared coefficients, per-channel state. Sixteen identical filters would
//    otherwise mean sixteen coefficient recomputes per control tick, and the
//    recompute (pow, cos, sqrt) dwarfs the three multiply-adds that actually
//    filter. One design, sixteen state pairs.
//  * The flux peak is ABSORPTIVE ONLY. A boost re-applied on every pass is a
//    per-pass gain above 1, which the stability normalization would have to pay
//    for by pulling the loop gain down — and that would shorten the tail
//    whenever flux happened to be peaking, an audible decay pumping. Letting
//    the wandering peak only ever CUT keeps its worst-case contribution at
//    exactly 1, so it colours the tail without lengthening or shortening it.
//    (A boost compensated by an equal broadband trim would be worse than
//    either: the trim is itself a per-pass loss on everything else.)
//
// The static EQ has no such normalization on purpose: it is an explicit
// authoring choice, and a user who dials +6 dB into the loop should get a
// longer tail at that frequency — bounded by the stability normalization
// trading broadband decay for it. That trade is the honest one; the flux
// motion layer is the one that must be free.
//
// RT contract: configure() runs the filter design (control rate, never
// per-sample); process/reset allocate nothing.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/filter_state.hpp>
#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/sanitize.hpp>
#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::fdn {

namespace lanes = signal::detail::lanes;

// One EQ band as authored. Bands are set through the engine's API rather than
// through baked params: they are a voicing decision baked into a mode or a
// preset, not a knob the host automates.
struct EqBand {
    double freq_hz = 1000.0;
    double gain_db = 0.0;
    double q = 0.7;
};

// Default layout: one low shelf, eight peaks spread logarithmically, one high
// shelf — all flat until authored.
inline std::array<EqBand, kNumEqBands> default_eq_bands() {
    std::array<EqBand, kNumEqBands> bands{};
    bands[0] = {120.0, 0.0, 0.7};
    for (int i = 1; i <= 8; ++i) {
        // 250 Hz .. 8 kHz, geometric.
        const double t = static_cast<double>(i - 1) / 7.0;
        bands[static_cast<std::size_t>(i)] = {250.0 * std::pow(32.0, t), 0.0, 1.0};
    }
    bands[9] = {9000.0, 0.0, 0.7};
    return bands;
}

// The 10-band bank. Coefficients live once; every channel carries only its two
// state words per band.
template <typename SampleType = float>
class LoopEq {
public:
    LoopEq() : bands_(default_eq_bands()) {}

    void set_band(int index, const EqBand& band) {
        if (index < 0 || index >= kNumEqBands) return;
        bands_[static_cast<std::size_t>(index)] = band;
    }

    const EqBand& band(int index) const {
        return bands_[static_cast<std::size_t>(std::clamp(index, 0, kNumEqBands - 1))];
    }

    // Re-design every band for the tank rate. Centre frequencies are clamped
    // below 0.45 x fs_tank: at 16 kHz an 8 kHz band would otherwise sit on
    // Nyquist, where the bilinear transform's warping makes the section wild.
    void configure(double tank_rate) {
        const double max_hz = kAaCutoffFraction * tank_rate;
        worst_case_boost_ = 1.0;
        num_active_ = 0;
        BiquadT<double> designer;
        for (int b = 0; b < kNumEqBands; ++b) {
            const EqBand& band = bands_[static_cast<std::size_t>(b)];
            if (std::abs(band.gain_db) < kFlatBandDb) continue;
            const auto type = (b == 0) ? BiquadT<double>::Type::low_shelf
                              : (b == kNumEqBands - 1)
                                  ? BiquadT<double>::Type::high_shelf
                                  : BiquadT<double>::Type::peaking;
            designer.set_coefficients(type, std::min(band.freq_hz, max_hz),
                                      std::max(band.q, 0.05), tank_rate,
                                      band.gain_db);
            coeffs_[static_cast<std::size_t>(num_active_)] = designer.coefficients();
            ++num_active_;
            if (band.gain_db > 0.0)
                worst_case_boost_ *= std::pow(10.0, band.gain_db / 20.0);
        }
        reset();
    }

    void reset() {
        for (auto& s : state_) s = {};
    }

    // Conservative upper bound on the bank's magnitude response: the product of
    // every active band's linear boost. Bands that overlap can only add up to
    // this, so the stability normalization that consumes it is never optimistic.
    double worst_case_boost() const { return worst_case_boost_; }

    // Bands that are not flat — the ones process() actually runs.
    int active_bands() const {
        return num_active_;
    }

    SampleType process(int channel, SampleType x) {
        for (int k = 0; k < num_active_; ++k) {
            const auto& c = coeffs_[static_cast<std::size_t>(k)];
            x = static_cast<SampleType>(
                state_[state_index(k, channel)].process(c, static_cast<double>(x)));
        }
        return x;
    }

private:
    // A band is treated as flat (and skipped entirely) below this, so an
    // untouched EQ costs nothing per sample.
    static constexpr double kFlatBandDb = 0.01;

    static std::size_t state_index(int active_slot, int channel) {
        return static_cast<std::size_t>(active_slot) *
                   static_cast<std::size_t>(kNumChannels) +
               static_cast<std::size_t>(channel);
    }

    std::array<EqBand, kNumEqBands> bands_{};
    std::array<BiquadCoefficientsT<double>, kNumEqBands> coeffs_{};
    std::array<SanitizedBiquadState,
               static_cast<std::size_t>(kNumEqBands) * kNumChannels> state_{};
    int num_active_ = 0;
    double worst_case_boost_ = 1.0;
};

// The flux layer: one extra peaking biquad per channel whose centre frequency
// and depth both wander. Coefficients are recomputed at the control rate and
// the filter ALWAYS runs, even at zero depth — switching a biquad in and out of
// a feedback path pops, and the pop recirculates.
//
// The peak is ABSORPTIVE ONLY: its gain wanders in [-kFluxMaxDb, 0], never
// above flat. That is what lets it contribute exactly 1 to the worst-case
// bound, which in turn is what keeps the Jot decay law exact — see the header.
// It also means flux never lengthens a tail; it takes narrow, wandering bites
// out of it, and because each of the 16 lines bites in its own register the
// aggregate decay is unchanged while the colour never sits still.
//
// Coefficients and state are stored per field across the sixteen channels so
// both the per-tick redesign and the per-sample filter run as one pass over all
// channels: the redesign evaluates the RBJ peaking formula with polynomial
// sin/cos/exp that are exact to double rounding over the argument ranges the
// bank can produce, instead of three libm calls per channel per tick.
template <typename SampleType = float>
class FluxBank {
public:
    FluxBank() { configure(48000.0, kFluxDefaultDb); }

    void reset() {
        for (int i = 0; i < kNumChannels; ++i) {
            rng_[i].reset(kSeedBase + static_cast<std::uint32_t>(i) * kSeedStride);
            target_hz_[i] = base_hz_[i];
            current_hz_[i] = base_hz_[i];
            target_db_[i] = 0.0;
            current_db_[i] = 0.0;
            countdown_[i] = interval_samples(i);
            s1_[i] = 0.0;
            s2_[i] = 0.0;
        }
        redesign();
    }

    // Base frequencies are spread logarithmically across the channels, so the
    // sixteen lines each shimmer in their own register rather than all pumping
    // the same band.
    void configure(double tank_rate, double depth_db) {
        tank_rate_ = tank_rate;
        depth_db_ = std::clamp(depth_db, 0.0, kFluxMaxDb);
        const double max_hz = kAaCutoffFraction * tank_rate;
        for (int i = 0; i < kNumChannels; ++i) {
            const double t = (kNumChannels > 1)
                                 ? static_cast<double>(i) /
                                       static_cast<double>(kNumChannels - 1)
                                 : 0.0;
            base_hz_[i] = std::min(
                kFluxBaseMinHz * std::pow(kFluxBaseMaxHz / kFluxBaseMinHz, t), max_hz);
            target_hz_[i] = std::clamp(target_hz_[i], kFluxBaseMinHz * 0.5, max_hz);
            current_hz_[i] = std::clamp(current_hz_[i], kFluxBaseMinHz * 0.5, max_hz);
        }
        // One-pole glide toward the re-target, in the log domain.
        glide_ = std::exp(-1.0 / std::max(1.0, kFluxGlideMs * 0.001 * tank_rate /
                                                   kControlRateSamples));
        redesign();
    }

    // One control-rate tick: re-target the channels whose timer expired, glide
    // the rest, and redesign.
    void tick_control(int samples_elapsed) {
        for (int i = 0; i < kNumChannels; ++i) {
            countdown_[i] -= samples_elapsed;
            if (countdown_[i] <= 0) {
                const double octaves = rng_[i].next_bipolar() * kFluxSpreadOctaves;
                target_hz_[i] = base_hz_[i] * std::pow(2.0, octaves);
                target_db_[i] = -rng_[i].next_unit() * depth_db_;
                countdown_[i] = interval_samples(i);
            }
        }
        for (int i = 0; i < kNumChannels; ++i) {
            current_hz_[i] += (target_hz_[i] - current_hz_[i]) * (1.0 - glide_);
            current_db_[i] += (target_db_[i] - current_db_[i]) * (1.0 - glide_);
        }
        redesign();
    }

    // Always exactly 1: the wandering peak is absorptive only, so no pass can
    // ever be louder than flat (see the class note).
    double worst_case_boost() const { return 1.0; }

    // The designed section for one channel, normalized to a0 == 1.
    BiquadCoefficientsT<double> coefficients(int channel) const {
        const auto c = static_cast<std::size_t>(channel);
        return {b0_[c], b1_[c], b2_[c], a1_[c], a2_[c]};
    }

    // One channel, one sample, with every state word sanitized — the
    // defensive scalar path.
    SampleType process(int channel, SampleType x) {
        const auto c = static_cast<std::size_t>(channel);
        const double in = finite_or_zero(static_cast<double>(x));
        const double out = finite_or_zero(b0_[c] * in + s1_[c]);
        s1_[c] = finite_or_zero(snap_to_zero(b1_[c] * in - a1_[c] * out + s2_[c]));
        s2_[c] = finite_or_zero(snap_to_zero(b2_[c] * in - a2_[c] * out));
        return static_cast<SampleType>(out);
    }

    // The bank's coefficients and state as two-lane vectors (channels 2i and
    // 2i + 1 in lane pair i), for a block kernel to hold in local storage
    // across a block: load_lanes() before, store_lanes() after.
    struct Lanes {
        lanes::D2 b0[kNumChannels / 2];
        lanes::D2 b1[kNumChannels / 2];
        lanes::D2 b2[kNumChannels / 2];
        lanes::D2 a1[kNumChannels / 2];
        lanes::D2 a2[kNumChannels / 2];
        lanes::D2 s1[kNumChannels / 2];
        lanes::D2 s2[kNumChannels / 2];
    };

    void load_lanes(Lanes& l) const {
        for (std::size_t p = 0; p < static_cast<std::size_t>(kNumChannels / 2); ++p) {
            l.b0[p] = lanes::load(&b0_[2 * p]);
            l.b1[p] = lanes::load(&b1_[2 * p]);
            l.b2[p] = lanes::load(&b2_[2 * p]);
            l.a1[p] = lanes::load(&a1_[2 * p]);
            l.a2[p] = lanes::load(&a2_[2 * p]);
            l.s1[p] = lanes::load(&s1_[2 * p]);
            l.s2[p] = lanes::load(&s2_[2 * p]);
        }
    }

    void store_lanes(const Lanes& l) {
        for (std::size_t p = 0; p < static_cast<std::size_t>(kNumChannels / 2); ++p) {
            lanes::store(&s1_[2 * p], l.s1[p]);
            lanes::store(&s2_[2 * p], l.s2[p]);
        }
    }

    // Every channel, one sample, in place, in the tank's lane layout: v[j]
    // holds channels 4j .. 4j + 3. The tank's block path. Input is finite and
    // bounded by the tank's construction (the damping stage clamps it), so the
    // recursion runs bare and heal() snaps and sanitizes the state once per
    // block (after store_lanes()).
    static void process_lanes(Lanes& l, lanes::F4 (&v)[kNumChannels / 4]) {
        using lanes::D2;
        for (std::size_t j = 0; j < static_cast<std::size_t>(kNumChannels / 4); ++j) {
            const D2 in[2] = {lanes::widen_low(v[j]), lanes::widen_high(v[j])};
            D2 out[2];
            for (std::size_t h = 0; h < 2; ++h) {
                const std::size_t p = 2 * j + h;
                const D2 y = l.b0[p] * in[h] + l.s1[p];
                l.s1[p] = l.b1[p] * in[h] - l.a1[p] * y + l.s2[p];
                l.s2[p] = l.b2[p] * in[h] - l.a2[p] * y;
                out[h] = y;
            }
            v[j] = lanes::narrow(out[0], out[1]);
        }
    }

    // Snap decayed state to zero and zero any channel whose state left the
    // finite range. Once per block of at most a control period.
    void heal() {
        for (std::size_t c = 0; c < static_cast<std::size_t>(kNumChannels); ++c) {
            if (!std::isfinite(s1_[c]) || !std::isfinite(s2_[c])) {
                s1_[c] = 0.0;
                s2_[c] = 0.0;
            }
            s1_[c] = snap_to_zero(s1_[c]);
            s2_[c] = snap_to_zero(s2_[c]);
        }
    }

private:
    static constexpr std::uint32_t kSeedBase = 0x5EED1u;
    static constexpr std::uint32_t kSeedStride = 0x85EBCA6Bu;

    int interval_samples(int channel) {
        const double ms = kFluxIntervalMinMs +
                          rng_[channel].next_unit() *
                              (kFluxIntervalMaxMs - kFluxIntervalMinMs);
        return std::max(1, static_cast<int>(ms * 0.001 * tank_rate_));
    }

    // sin and cos of w on [0, pi]: shift to y = w - pi/2 in [-pi/2, pi/2] and
    // evaluate the Taylor series of cos(y) and sin(y) through degree 18 and 17
    // (truncation below 5e-14 there), so sin(w) = cos(y), cos(w) = -sin(y).
    static void sin_cos_0_pi(double w, double& s, double& c) {
        const double y = w - 1.5707963267948966;
        const double y2 = y * y;
        double cy = 1.0 / 6402373705728000.0;  // 1/18!
        cy = cy * y2 - 1.0 / 20922789888000.0; // 1/16!
        cy = cy * y2 + 1.0 / 87178291200.0;    // 1/14!
        cy = cy * y2 - 1.0 / 479001600.0;      // 1/12!
        cy = cy * y2 + 1.0 / 3628800.0;        // 1/10!
        cy = cy * y2 - 1.0 / 40320.0;          // 1/8!
        cy = cy * y2 + 1.0 / 720.0;            // 1/6!
        cy = cy * y2 - 1.0 / 24.0;
        cy = cy * y2 + 0.5;
        cy = 1.0 - cy * y2;
        double sy = 1.0 / 355687428096000.0;  // 1/17!
        sy = sy * y2 - 1.0 / 1307674368000.0; // 1/15!
        sy = sy * y2 + 1.0 / 6227020800.0;    // 1/13!
        sy = sy * y2 - 1.0 / 39916800.0;      // 1/11!
        sy = sy * y2 + 1.0 / 362880.0;        // 1/9!
        sy = sy * y2 - 1.0 / 5040.0;          // 1/7!
        sy = sy * y2 + 1.0 / 120.0;
        sy = sy * y2 - 1.0 / 6.0;
        sy = y + y * y2 * sy;
        s = cy;
        c = -sy;
    }

    // 10^(gain_db / 40) for gain_db in [-kFluxMaxDb, 0]: the exponent is at
    // most 0.18 in magnitude, where the Taylor series of exp through degree 10
    // is exact to double rounding.
    static double peaking_amplitude(double gain_db) {
        const double x = gain_db * (2.302585092994046 / 40.0);
        double e = 1.0 / 3628800.0;
        e = e * x + 1.0 / 362880.0;
        e = e * x + 1.0 / 40320.0;
        e = e * x + 1.0 / 5040.0;
        e = e * x + 1.0 / 720.0;
        e = e * x + 1.0 / 120.0;
        e = e * x + 1.0 / 24.0;
        e = e * x + 1.0 / 6.0;
        e = e * x + 0.5;
        e = e * x + 1.0;
        return e * x + 1.0;
    }

    // The RBJ peaking section (BiquadT::Type::peaking), normalized.
    void redesign() {
        const double max_hz = kAaCutoffFraction * tank_rate_;
        const double to_w = 6.283185307179586 / tank_rate_;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kNumChannels); ++i) {
            const double w0 = std::clamp(current_hz_[i], 20.0, max_hz) * to_w;
            double sin_w0 = 0.0;
            double cos_w0 = 0.0;
            sin_cos_0_pi(w0, sin_w0, cos_w0);
            const double alpha = sin_w0 / (2.0 * kFluxQ);
            const double A = peaking_amplitude(std::clamp(current_db_[i], -kFluxMaxDb, 0.0));
            const double inv_a0 = 1.0 / (1.0 + alpha / A);
            b0_[i] = (1.0 + alpha * A) * inv_a0;
            b1_[i] = -2.0 * cos_w0 * inv_a0;
            b2_[i] = (1.0 - alpha * A) * inv_a0;
            a1_[i] = b1_[i];
            a2_[i] = (1.0 - alpha / A) * inv_a0;
        }
    }

    std::array<XorShift32, kNumChannels> rng_{};
    std::array<double, kNumChannels> b0_{};
    std::array<double, kNumChannels> b1_{};
    std::array<double, kNumChannels> b2_{};
    std::array<double, kNumChannels> a1_{};
    std::array<double, kNumChannels> a2_{};
    std::array<double, kNumChannels> s1_{};
    std::array<double, kNumChannels> s2_{};
    std::array<double, kNumChannels> base_hz_{};
    std::array<double, kNumChannels> target_hz_{};
    std::array<double, kNumChannels> current_hz_{};
    std::array<double, kNumChannels> target_db_{};
    std::array<double, kNumChannels> current_db_{};
    std::array<int, kNumChannels> countdown_{};
    double tank_rate_ = 48000.0;
    double depth_db_ = kFluxMaxDb;
    double glide_ = 0.0;
};

}  // namespace pulp::signal::fdn
