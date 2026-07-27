#pragma once

/// @file tape_machine_components.hpp
/// Public tape standards, physical models, EQ, and compander building blocks.

#include <pulp/signal/character_delay/tape_physical.hpp>
#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/character_delay/primitives.hpp>
#include <pulp/signal/character_delay/tables.hpp>
#include <pulp/signal/character_delay/tape.hpp>
#include <pulp/signal/character_delay/tape_loss.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/units.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// Which recording standard's record/playback network pair is in force.
enum class TapeCurve : std::uint8_t {
    nab,             ///< NAB reel. Bass turnover AND treble turnover.
    iec_ccir,        ///< IEC/CCIR reel. Treble turnover only — no bass shelf.
    cassette_type1,  ///< Compact cassette, ferric ("Normal"). 3180 / 120 µs.
    cassette_type2,  ///< Compact cassette, chrome. 3180 / 70 µs.
};

/// Which machine's full preset a fresh instance starts from.
enum class TapeArchetype : std::uint8_t {
    ampex_350_440,  ///< Documented NAB-native two-speed studio recorder lineage.
    studer_a800,    ///< Documented NAB *and* CCIR-switchable multitrack.
    cassette_deck,  ///< Fixed 1.875 ips, Type I / Type II EQ switch.
};

/// Published tables and closed forms, free of any instance so a test can assert
/// the SHIPPED constant rather than a copy of it.
namespace tape {

/// The halving speed ladder, in inches per second. Published convention:
/// 30/15/7.5 ips professional, 7.5/3.75 ips domestic reel, and 1.875 ips
/// (4.75 cm/s) as the fixed Compact Cassette speed.
inline constexpr std::array<double, 5> kSpeedsIps = {1.875, 3.75, 7.5, 15.0, 30.0};

/// Inches to metres. Exact by definition of the international inch.
inline constexpr double kMetresPerInch = 0.0254;

/// The frequency every EQ curve in this module is normalized to be unity at.
/// 1 kHz is the tape-alignment reference and the frequency every published
/// record/reproduce curve is quoted relative to. See the header's note on why
/// the raw prototype cannot be used unnormalized.
inline constexpr double kEqReferenceHz = 1000.0;

/// One standard's pair of time constants, in SECONDS. `bass_s == 0` means the
/// curve has no bass turnover — the documented CCIR/IEC reel case, and NAB at
/// 30 ips.
struct EqTimeConstants {
    double bass_s = 0.0;
    double treble_s = 0.0;

    bool has_bass_shelf() const noexcept { return bass_s > 0.0; }
};

/// The published time-constant table, selected by `(curve, speed)`.
///
/// Reel speeds are matched against the ladder rather than compared for
/// equality, because `set_speed_ips` snaps to the ladder and a caller may hand
/// in 15.000000001. Speeds below a curve's lowest tabulated row fall back to
/// that row: NAB's 3180/50 µs pair is the documented reel-to-reel figure and
/// there is no separate published row for a NAB machine run at 3.75 ips.
inline EqTimeConstants eq_time_constants(TapeCurve curve, double speed_ips) noexcept {
    switch (curve) {
        case TapeCurve::cassette_type1: return {3180e-6, 120e-6};
        case TapeCurve::cassette_type2: return {3180e-6, 70e-6};
        case TapeCurve::iec_ccir:
            // CCIR adopted 70 µs (from 100 µs) in 1966; 35 µs at 15 ips and
            // 17.5 µs at 30 ips per the A800 technical data. No bass shelf at
            // any reel speed — the structural difference from NAB.
            if (speed_ips >= 22.5) return {0.0, 17.5e-6};
            if (speed_ips >= 11.25) return {0.0, 35e-6};
            return {0.0, 70e-6};
        case TapeCurve::nab:
        default:
            // The professional convention extends NAB to 30 ips by dropping the
            // bass shelf and halving the treble constant again.
            if (speed_ips >= 22.5) return {0.0, 17.5e-6};
            return {3180e-6, 50e-6};
    }
}

/// Corner frequency of a time constant, in Hz. `f = 1 / (2π·t)`.
inline double eq_corner_hz(double tau_s) noexcept {
    return tau_s > 0.0 ? 1.0 / (2.0 * chardelay::kPi * tau_s) : 0.0;
}

/// The analog record (pre-emphasis) prototype `H(s) = (1 + s·t₁)/(1 + s·t₂)`,
/// in dB, normalized to 0 dB at `kEqReferenceHz`.
///
/// This is what the realized filter is measured against. It is stated here as a
/// closed form rather than as a table of expected decibels so an acceptance test
/// computes its expectations from the shipped time constants instead of
/// restating them.
inline double eq_record_response_db(const EqTimeConstants& tc, double hz) noexcept {
    auto magnitude = [&tc](double f) {
        const double w = 2.0 * chardelay::kPi * f;
        const double numerator = tc.has_bass_shelf() ? std::hypot(1.0, w * tc.bass_s) : 1.0;
        return numerator / std::hypot(1.0, w * tc.treble_s);
    };
    return 20.0 * std::log10(magnitude(hz) / magnitude(kEqReferenceHz));
}

/// The playback (de-emphasis) network is the exact reciprocal of the record
/// network, so its response is the negation.
inline double eq_playback_response_db(const EqTimeConstants& tc, double hz) noexcept {
    return -eq_record_response_db(tc, hz);
}

// ── Reproduce and record head gaps ────────────────────────────────────────

/// Reproduce and record gap widths for one speed class, in METRES.
struct HeadGapGeometry {
    double reproduce_m = 3.5e-6;
    double record_m = 10e-6;
};

/// Gap widths by speed class. Published typical RANGES; the specific value
/// inside each range is a design parameter.
///
/// Studio 15/30 ips reproduce 3.5 µm [design parameter, range 2–5 µm — the
/// documented professional reproduce-gap range], record 10 µm [dp, 8–13 µm].
/// Consumer reel 7.5/3.75 ips reproduce 5 µm [dp, 3–7 µm], record 12 µm
/// [dp, 8–15 µm]. Cassette 1.875 ips reproduce 0.8 µm [dp, 0.5–1.2 µm — the
/// documented sub-micron cassette reproduce-gap range], record 3 µm [dp, 2–5 µm].
/// Record gaps run wider than reproduce gaps throughout, which is convention
/// rather than accident: the recording side has the lower resolution
/// requirement.
inline HeadGapGeometry head_gap_geometry(double speed_ips) noexcept {
    if (speed_ips >= 11.25) return {3.5e-6, 10e-6};   // studio 15 / 30 ips
    if (speed_ips >= 2.8125) return {5e-6, 12e-6};    // consumer reel 7.5 / 3.75 ips
    return {0.8e-6, 3e-6};                            // cassette 1.875 ips
}

/// Tape speed in metres per second.
inline double speed_m_per_s(double speed_ips) noexcept {
    return speed_ips * kMetresPerInch;
}

/// Westmijze's reproduce gap loss: `|sin(π·g/λ) / (π·g/λ)|`, `λ = v/f`.
///
/// Note this is exactly the `gap` term of the reused Wallace model, evaluated at
/// THIS module's speed-class gap rather than at the delay tier's fixed 3 µm —
/// the argument `k·g/2` there is `(2πf/v)·g/2 = π·g/λ` here. Same published
/// formula, a different head.
inline double gap_loss_magnitude(double hz, double gap_m, double speed_ips) noexcept {
    const double x = chardelay::kPi * gap_m * hz / speed_m_per_s(speed_ips);
    if (x < 1e-9) return 1.0;
    return std::abs(std::sin(x) / x);
}

inline double gap_loss_db(double hz, double gap_m, double speed_ips) noexcept {
    return 20.0 * std::log10(std::max(gap_loss_magnitude(hz, gap_m, speed_ips), 1e-12));
}

/// First extinction null, where `λ = g`: `f = v / g`.
inline double gap_null_hz(double gap_m, double speed_ips) noexcept {
    return speed_m_per_s(speed_ips) / std::max(gap_m, 1e-12);
}

// ── The age axis this module adds on top of the reused one ────────────────

/// The reused `kAgeAxis` table drives spacing, hiss, chew and degrade, and
/// `kTapeAxis` drives wow/flutter depth — all reused verbatim. These two columns
/// are new phenomena a feedback-loop delay has no way to model: print-through
/// needs storage time, and bias drift is a maintenance state rather than a
/// per-repeat one. [all design parameters]
inline constexpr std::array<double, 3> kAgeAxis = {0.0, 0.5, 1.0};

/// Storage print grows with age. [dp] Sets the `print_through_db` range: the
/// baked parameter's −80 .. −38 dB bounds ARE this table's extremes.
inline constexpr std::array<double, 3> kAgePrintThroughDb = {-80.0, -55.0, -38.0};

/// An aging bias supply drifts toward OVER-bias — the documented failure
/// direction, since oscillator components drift high more often than low. Added
/// to whatever `bias_c` the user set, then clamped. [dp]
inline constexpr std::array<double, 3> kAgeBiasDriftUnits = {0.0, 0.15, 0.4};

inline double age_print_through_db(double age01) noexcept {
    return chardelay::interpolate_knots(kAgeAxis, kAgePrintThroughDb,
                                        std::clamp(age01, 0.0, 1.0));
}

inline double age_bias_drift(double age01) noexcept {
    return chardelay::interpolate_knots(kAgeAxis, kAgeBiasDriftUnits,
                                        std::clamp(age01, 0.0, 1.0));
}

/// Magnitude, in dB, of the REALIZED `chardelay::FirstOrderShelf` — the TPT
/// one-pole shelf this module uses for both the bias shelf and the crosstalk
/// tilt.
///
/// Stated in closed form rather than measured by rendering because two callers
/// need it: the crosstalk stage normalizes its tilt at `kEqReferenceHz` using
/// it, and the suite asserts the bias shelf against it. Deriving it from the
/// analog prototype instead would leave a bilinear-warping error inside a
/// tolerance, which is exactly the kind of slop that hides a real coefficient
/// bug. From the TPT recurrence `y = g·x + (1−g)·s`, `s' = 2g·x + (1−2g)·s`:
/// `H_lp(z) = g(1 + z⁻¹) / (1 − (1−2g)z⁻¹)`, and the shelf is
/// `G + (1−G)·H_lp`.
inline double first_order_shelf_db(double corner_hz, double gain_linear, double hz,
                                   double sample_rate) noexcept {
    const double fc = std::clamp(corner_hz, 0.1, 0.49 * sample_rate);
    const double t = std::tan(chardelay::kPi * fc / sample_rate);
    const double g = t / (1.0 + t);
    const double theta = 2.0 * chardelay::kPi * hz / sample_rate;
    const std::complex<double> z_inv(std::cos(theta), -std::sin(theta));
    const std::complex<double> lp = g * (1.0 + z_inv) / (1.0 - (1.0 - 2.0 * g) * z_inv);
    const std::complex<double> shelf = gain_linear + (1.0 - gain_linear) * lp;
    return 20.0 * std::log10(std::max(std::abs(shelf), 1e-12));
}

// ── Archetype presets ─────────────────────────────────────────────────────

/// The complete baked preset for one machine. Every field is overridable by the
/// matching `set_*` after `set_archetype`.
struct ArchetypePreset {
    std::array<double, 3> legal_speeds_ips{};
    int legal_speed_count = 0;
    double default_speed_ips = 15.0;
    TapeCurve default_curve = TapeCurve::nab;
    double crosstalk_db = -40.0;
    bool companding = false;
    double age01 = 0.0;
};

/// Baked defaults per §5 of the source spec.
///
/// `ampex_350_440` legal speeds {15, 7.5} [design parameter — the documented
/// two-speed Ampex 350 configuration; 440-class descendants add 30 ips and are
/// offered as a superset by selecting the Studer row instead]. `studer_a800`
/// {30, 15, 7.5} and its ≥40 dB crosstalk figure are PUBLISHED. `cassette_deck`
/// is fixed at 1.875 ips by the cassette standard.
///
/// Crosstalk: −40 dB for the A800 is published (100 Hz–12 kHz). −38 dB for the
/// Ampex class [dp, range −45 .. −30 dB] and −30 dB for cassette [dp, range
/// −35 .. −22 dB] are honest design choices positioned against that one verified
/// figure — no comparable published number surfaced for either. Cassette's
/// narrower track pitch making crosstalk worse than open-reel is the documented
/// direction; the magnitude is ours.
///
/// Starting `age01`: 0.2 for the Ampex class [dp, range 0.1–0.35 — "a machine
/// with some hours on it"], 0.05 for the Studer [dp, 0.0–0.15 — well-maintained
/// flagship], 0.3 for cassette [dp, 0.2–0.5 — consumer-grade wear expectation].
inline ArchetypePreset archetype_preset(TapeArchetype archetype) noexcept {
    ArchetypePreset p;
    switch (archetype) {
        case TapeArchetype::studer_a800:
            p.legal_speeds_ips = {7.5, 15.0, 30.0};
            p.legal_speed_count = 3;
            p.default_speed_ips = 15.0;
            p.default_curve = TapeCurve::nab;
            p.crosstalk_db = -40.0;
            p.companding = false;
            p.age01 = 0.05;
            return p;
        case TapeArchetype::cassette_deck:
            p.legal_speeds_ips = {1.875, 0.0, 0.0};
            p.legal_speed_count = 1;
            p.default_speed_ips = 1.875;
            p.default_curve = TapeCurve::cassette_type1;
            p.crosstalk_db = -30.0;
            // Companding on by default: dbx-equipped decks were a documented
            // cassette-era convention, and the reel machines of the other two
            // classes predate consumer companding entirely.
            p.companding = true;
            p.age01 = 0.3;
            return p;
        case TapeArchetype::ampex_350_440:
        default:
            p.legal_speeds_ips = {7.5, 15.0, 0.0};
            p.legal_speed_count = 2;
            p.default_speed_ips = 15.0;
            p.default_curve = TapeCurve::nab;
            p.crosstalk_db = -38.0;
            p.companding = false;
            p.age01 = 0.2;
            return p;
    }
}

/// Nearest legal speed for an archetype. A cassette deck never reports 30 ips
/// no matter what it is asked for.
inline double snap_speed_ips(TapeArchetype archetype, double requested_ips) noexcept {
    const ArchetypePreset preset = archetype_preset(archetype);
    double best = preset.legal_speeds_ips[0];
    double best_distance = std::abs(std::log(std::max(requested_ips, 1e-6) / best));
    for (int i = 1; i < preset.legal_speed_count; ++i) {
        const double candidate = preset.legal_speeds_ips[static_cast<std::size_t>(i)];
        // Nearest in RATIO, not in difference: the ladder is geometric, so
        // 5 ips is equidistant from 3.75 and 7.5 the way an ear hears it, and a
        // linear metric would bias every midpoint upward.
        const double distance = std::abs(std::log(std::max(requested_ips, 1e-6) / candidate));
        if (distance < best_distance) {
            best = candidate;
            best_distance = distance;
        }
    }
    return best;
}

}  // namespace tape

/// One first-order EQ section — a `(curve, speed)` record network, or its exact
/// inverse.
///
/// Realized by bilinear transform with each corner prewarped INDEPENDENTLY. That
/// matters and is easy to get wrong: a single bilinear substitution can place
/// only one corner exactly, and this prototype has a zero at 50 Hz and a pole at
/// 3183 Hz that both have to land. Prewarping the zero's section and the pole's
/// section separately leaves each with a spurious `(1 + z⁻¹)` factor which
/// CANCELS between them, so the product is a plain first-order section with both
/// corners exact:
///
/// ```
///            (u₂/u₁)·[(u₁+1) + (u₁−1)z⁻¹]
///   H(z) =  ───────────────────────────────  ,   uᵢ = tan(π·fᵢ/fs),  fᵢ = 1/(2π·tᵢ)
///              (u₂+1) + (u₂−1)z⁻¹
/// ```
///
/// Its DC gain is exactly 1, as the analog prototype's is. The section is then
/// rescaled so it is unity at `kEqReferenceHz` instead.
///
/// The inverse is taken in the DIGITAL domain — numerator and denominator
/// swapped — not by bilinear-transforming `1/H(s)` separately. Two independent
/// discretizations of reciprocal analog prototypes are not reciprocal digital
/// filters; swapping the coefficients makes the pair cancel to numerical
/// precision, which is what a record/playback pair bracketing a nonlinearity has
/// to do.
class TapeEqSection {
public:
    /// How far inside the unit circle the inverse's pole is pulled when the
    /// forward section's numerator is `(1 + z⁻¹)` — i.e. when the curve has no
    /// bass shelf and the record network is a plain one-pole with a Nyquist
    /// zero. Undamped, the inverse has unbounded gain at Nyquist.
    /// [design parameter] default 0.90, range 0.70 .. 0.999.
    ///
    /// The tradeoff is monotone in both directions and was measured across
    /// every degenerate `(curve, speed)` at 44.1 / 48 / 192 kHz, worst case of
    /// each: at 0.999 the round-trip ripple over 20 Hz–15 kHz is 0.0000 dB and
    /// the ceiling +92.4 dB; at 0.98, 0.0015 dB and +67.7 dB; at 0.90,
    /// 0.0396 dB and +53.4 dB; at 0.70, 0.4260 dB and +42.9 dB. 0.90 is chosen
    /// because it buys 14 dB of ceiling for ripple that is still seven times
    /// inside the ±0.3 dB flatness criterion, and 0.70 would break that
    /// criterion to buy only ten more.
    ///
    /// Note what the numbers say about where the ceiling comes from: even at
    /// 0.70 it is +42.9 dB. A reproduce network with no bass shelf is
    /// `(1 + s·t₂)` — a differentiator — so its gain near Nyquist is a property
    /// of the standard's prototype and of the sample rate, not of this
    /// damping. The damping only decides whether that gain is large or
    /// infinite.
    static constexpr double kInversePoleDamping = 0.90;

    /// Highest corner the prewarp will place, as a fraction of the sample rate.
    /// `tan(π·f/fs)` diverges at `f = fs/2`.
    static constexpr double kPrewarpCeiling = 0.49;

    void set(const tape::EqTimeConstants& tc, double sample_rate, bool inverse) noexcept {
        const double fs = sample_rate > 0.0 ? sample_rate : 48000.0;
        const double treble_hz =
            std::min(tape::eq_corner_hz(tc.treble_s), kPrewarpCeiling * fs);
        const double u_treble = std::tan(chardelay::kPi * treble_hz / fs);

        double b0 = 0.0, b1 = 0.0;
        const double a1 = (u_treble - 1.0) / (u_treble + 1.0);
        if (tc.has_bass_shelf()) {
            const double bass_hz =
                std::min(tape::eq_corner_hz(tc.bass_s), kPrewarpCeiling * fs);
            const double u_bass = std::tan(chardelay::kPi * bass_hz / fs);
            const double scale = (u_treble / u_bass) / (u_treble + 1.0);
            b0 = scale * (u_bass + 1.0);
            b1 = scale * (u_bass - 1.0);
        } else {
            b0 = u_treble / (u_treble + 1.0);
            b1 = b0;
        }

        normalize(b0, b1, a1, fs);

        if (inverse) {
            const double inv = 1.0 / b0;
            const double damping = tc.has_bass_shelf() ? 1.0 : kInversePoleDamping;
            const double new_b0 = inv;
            const double new_b1 = a1 * inv;
            const double new_a1 = damping * b1 * inv;
            b0 = new_b0;
            b1 = new_b1;
            normalize(b0, b1, new_a1, fs);
            b0_ = b0;
            b1_ = b1;
            a1_ = new_a1;
        } else {
            b0_ = b0;
            b1_ = b1;
            a1_ = a1;
        }
    }

    void reset() noexcept {
        x1_ = 0.0;
        y1_ = 0.0;
    }

    double process(double x) noexcept {
        const double y = b0_ * x + b1_ * x1_ - a1_ * y1_;
        x1_ = x;
        y1_ = snap_to_zero(y);
        return y;
    }

    /// Magnitude response in dB. Used by the acceptance suite to compare the
    /// realized section against the analytic prototype without rendering.
    double response_db(double hz, double sample_rate) const noexcept {
        return 20.0 * std::log10(std::max(magnitude(b0_, b1_, a1_, hz, sample_rate), 1e-12));
    }

private:
    static double magnitude(double b0, double b1, double a1, double hz,
                            double sample_rate) noexcept {
        const double theta = 2.0 * chardelay::kPi * hz / sample_rate;
        const double c = std::cos(theta), s = std::sin(theta);
        // e^{-jθ} = c − js.
        const double num = std::hypot(b0 + b1 * c, -b1 * s);
        const double den = std::hypot(1.0 + a1 * c, -a1 * s);
        return den > 1e-30 ? num / den : 0.0;
    }

    static void normalize(double& b0, double& b1, double a1, double fs) noexcept {
        const double gain = magnitude(b0, b1, a1, tape::kEqReferenceHz, fs);
        if (gain > 1e-30) {
            b0 /= gain;
            b1 /= gain;
        }
    }

    double b0_ = 1.0, b1_ = 0.0, a1_ = 0.0;
    double x1_ = 0.0, y1_ = 0.0;
};

/// The 2:1 / 1:2 linear-dB compander pair.
///
/// Encode halves the programme's level in dB relative to a reference; decode
/// doubles it back. The algebra is exact and worth writing out, because it is
/// the whole reason the topology works: with `L` the input's level in dBr,
/// encode applies `−L/2`, so the encoded signal sits at `L/2` dBr; decode
/// measures THAT and applies `+L/2`, returning `L`. Both stages are unity at the
/// reference by construction, and a round trip with nothing in between is
/// transparent — not approximately, identically.
///
/// The decoder tracks the ENCODED envelope rather than being handed the
/// encoder's gain. That is the point: everything the tape does between them —
/// loss, hiss, print-through — is inside the loop the decoder measures, so noise
/// that entered after encoding gets expanded DOWN along with a quiet programme.
/// A feed-forward "remember the gain and undo it" topology would leave the noise
/// exactly where it was and do nothing at all.
template <typename SampleType = float>
class TapeCompanderT {
public:
    /// [design parameter] default 1 ms, range 0.3–5 ms. **Honest gap**: no
    /// citable dbx-specific attack constant exists in verifiable literature —
    /// the published material describes the 2:1/1:2 concept, not the ballistics.
    static constexpr double kAttackMs = 1.0;

    /// [design parameter] default 100 ms, range 50–300 ms. Same honest gap.
    static constexpr double kReleaseMs = 100.0;

    /// 0 dBr, in dBFS. [design parameter] default −20 dBFS, range −24 .. −16 —
    /// the calibration-tape-equivalent level, matching the house
    /// −20 dBFS = 0 VU convention.
    static constexpr double kRefLevelDbfs = -20.0;

    void prepare(double sample_rate) {
        for (auto* follower : {&encode_env_, &decode_env_}) {
            follower->prepare(static_cast<SampleType>(sample_rate));
            follower->set_attack_ms(static_cast<SampleType>(kAttackMs));
            follower->set_release_ms(static_cast<SampleType>(kReleaseMs));
        }
        reference_ = units::db_to_linear(kRefLevelDbfs);
        reset();
    }

    void reset() noexcept {
        encode_env_.reset();
        decode_env_.reset();
    }

    /// Record side: 2:1 compression toward the reference.
    double encode(double x) noexcept { return x * gain(encode_env_, x, -0.5); }

    /// Playback side: 1:2 expansion, exact inverse slope.
    double decode(double x) noexcept { return x * gain(decode_env_, x, +1.0); }

private:
    double gain(BallisticsFilterT<SampleType>& follower, double x, double slope) noexcept {
        const auto envelope =
            static_cast<double>(follower.process(static_cast<SampleType>(x)));
        const double level_db = units::linear_to_db(envelope / reference_);
        return units::db_to_linear(slope * level_db);
    }

    BallisticsFilterT<SampleType> encode_env_{};
    BallisticsFilterT<SampleType> decode_env_{};
    double reference_ = 0.1;
};

}  // namespace pulp::signal
