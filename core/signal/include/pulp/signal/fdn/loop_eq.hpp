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
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/modulation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::fdn {

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

    SampleType process(int channel, SampleType x) {
        for (int k = 0; k < num_active_; ++k) {
            const auto& c = coeffs_[static_cast<std::size_t>(k)];
            State& s = state_[state_index(k, channel)];
            const double in = static_cast<double>(x);
            const double out = c.b0 * in + s.s1;
            s.s1 = snap_to_zero(c.b1 * in - c.a1 * out + s.s2);
            s.s2 = snap_to_zero(c.b2 * in - c.a2 * out);
            x = static_cast<SampleType>(out);
        }
        return x;
    }

private:
    // A band is treated as flat (and skipped entirely) below this, so an
    // untouched EQ costs nothing per sample.
    static constexpr double kFlatBandDb = 0.01;

    struct State {
        double s1 = 0.0;
        double s2 = 0.0;
    };

    static std::size_t state_index(int active_slot, int channel) {
        return static_cast<std::size_t>(active_slot) *
                   static_cast<std::size_t>(kNumChannels) +
               static_cast<std::size_t>(channel);
    }

    std::array<EqBand, kNumEqBands> bands_{};
    std::array<BiquadCoefficientsT<double>, kNumEqBands> coeffs_{};
    std::array<State, static_cast<std::size_t>(kNumEqBands) * kNumChannels> state_{};
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
            state_[i] = {};
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
            current_hz_[i] += (target_hz_[i] - current_hz_[i]) * (1.0 - glide_);
            current_db_[i] += (target_db_[i] - current_db_[i]) * (1.0 - glide_);
        }
        redesign();
    }

    // Always exactly 1: the wandering peak is absorptive only, so no pass can
    // ever be louder than flat (see the class note).
    double worst_case_boost() const { return 1.0; }

    SampleType process(int channel, SampleType x) {
        const auto& c = coeffs_[static_cast<std::size_t>(channel)];
        State& s = state_[static_cast<std::size_t>(channel)];
        const double in = static_cast<double>(x);
        const double out = c.b0 * in + s.s1;
        s.s1 = snap_to_zero(c.b1 * in - c.a1 * out + s.s2);
        s.s2 = snap_to_zero(c.b2 * in - c.a2 * out);
        return static_cast<SampleType>(out);
    }

private:
    static constexpr std::uint32_t kSeedBase = 0x5EED1u;
    static constexpr std::uint32_t kSeedStride = 0x85EBCA6Bu;

    struct State {
        double s1 = 0.0;
        double s2 = 0.0;
    };

    int interval_samples(int channel) {
        const double ms = kFluxIntervalMinMs +
                          rng_[channel].next_unit() *
                              (kFluxIntervalMaxMs - kFluxIntervalMinMs);
        return std::max(1, static_cast<int>(ms * 0.001 * tank_rate_));
    }

    void redesign() {
        BiquadT<double> designer;
        const double max_hz = kAaCutoffFraction * tank_rate_;
        for (int i = 0; i < kNumChannels; ++i) {
            designer.set_coefficients(BiquadT<double>::Type::peaking,
                                      std::clamp(current_hz_[i], 20.0, max_hz), kFluxQ,
                                      tank_rate_, current_db_[i]);
            coeffs_[static_cast<std::size_t>(i)] = designer.coefficients();
        }
    }

    std::array<XorShift32, kNumChannels> rng_{};
    std::array<BiquadCoefficientsT<double>, kNumChannels> coeffs_{};
    std::array<State, kNumChannels> state_{};
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
