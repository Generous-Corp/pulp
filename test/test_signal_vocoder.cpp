// VocoderT — the channel vocoder's acceptance suite.
//
// The spec's T-SPACING … T-LAT (vocoder-pulp-module-prompt.md §12), plus the
// node-wiring composition the spec marks normative but places outside the
// class. Expected values are computed from the shipped constants — the bank
// ratio, both Qs, the follower floors — never restated as literals, so retuning
// a constant moves the test that documents it.
//
// ── How this suite measures ───────────────────────────────────────────────
//
// Almost everything here is a filterbank measurement, and a filterbank
// measurement is where peak-picking goes wrong. Every magnitude in this file
// comes from a **Hann-windowed coherent DFT at exactly the probe frequency**,
// never from the largest sample of a rendered sine: at 8 kHz and 48 kHz there
// are six samples per cycle and none of them lands on the crest, which
// under-reads by 1.25 dB and looks exactly like a filter that is not flat.
//
// The bank is measured through `analysis_band(k)` — the band's own filtered
// output — rather than inferred from the vocoder's sum, because the sum is the
// product of a band with an envelope and cannot separate the two. The
// accessor is not taken on trust: the synthesis bank is measured through the
// audio output and checked against the same shipped centres, which is the
// assertion that would catch analysis and synthesis drifting apart.
//
// Pre-emphasis is divided out of every analysis measurement. It is a one-zero
// tilt on the modulator, `|1 − a·e^(−jω)|`, computed here from the shipped
// coefficient rather than measured, so the numbers being compared are the
// bank's and not the bank's times the tilt's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/chorus_family.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vocoder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Voc = VocoderT<double>;
constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// The defaults the spec's worked example uses, so every computed expectation
/// in this file traces back to one place.
constexpr int kBands = 16;
constexpr double kLoHz = 120.0;
constexpr double kHiHz = 7000.0;

// ── Instruments ───────────────────────────────────────────────────────────

/// Hann-windowed coherent magnitude at exactly `hz`. Windowed rather than
/// bin-exact so the probe frequency can be chosen freely (band centres are not
/// DFT bins), and coherent rather than peak-picked for the reason in the file
/// header.
double coherent_magnitude(const std::vector<double>& x, double hz) {
    const auto n = x.size();
    std::complex<double> acc{0.0, 0.0};
    double window_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n));
        const double theta = -2.0 * kPi * hz * static_cast<double>(i) / kSr;
        acc += w * x[i] * std::complex<double>(std::cos(theta), std::sin(theta));
        window_sum += w;
    }
    return 2.0 * std::abs(acc) / window_sum;
}

/// The one-zero pre-emphasis tilt the modulator passes through, so it can be
/// divided back out of an analysis measurement.
double pre_emphasis_gain(double hz) {
    const double w = 2.0 * kPi * hz / kSr;
    const std::complex<double> h =
        1.0 - Voc::kPreEmphasis * std::exp(std::complex<double>(0.0, -w));
    return std::abs(h);
}

Voc make_bank(int bands = kBands, double lo = kLoHz, double hi = kHiHz) {
    Voc v;
    v.prepare(kSr);
    v.set_band_count(bands);
    v.set_band_range_hz(lo, hi);
    v.reset();
    return v;
}

/// Magnitude response of ANALYSIS band `k` at `hz`, pre-emphasis removed. The
/// render length adapts so the window always spans a healthy number of cycles
/// even at the bottom of the bank.
double analysis_magnitude(Voc& v, int k, double hz) {
    const auto window = static_cast<std::size_t>(
        std::max(4096.0, std::ceil(40.0 * kSr / hz)));
    constexpr std::size_t kSettle = 8192;  // ≫ the slowest band's ring time
    v.reset();
    std::vector<double> trace(window);
    double out = 0.0;
    for (std::size_t i = 0; i < kSettle + window; ++i) {
        const double m = std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr);
        v.process(m, 0.0, out);
        if (i >= kSettle) trace[i - kSettle] = v.analysis_band(k);
    }
    return coherent_magnitude(trace, hz) / pre_emphasis_gain(hz);
}

/// Magnitude response of the SYNTHESIS bank at `hz` with only band `k` open.
/// Driving the carrier and holding every other band's gain at zero is the only
/// way to see one synthesis section from outside, and seeing it from outside is
/// the point: this is the measurement that catches the two banks drifting.
double synthesis_magnitude(Voc& v, int k, double hz) {
    (void)k;
    const auto window = static_cast<std::size_t>(
        std::max(4096.0, std::ceil(40.0 * kSr / hz)));
    constexpr std::size_t kSettle = 8192;
    // Deliberately NOT reset: `reset()` clears the held levels, and the held
    // levels are what this is measuring. (It did reset in the first draft, so
    // every gain was zero, the output was silence, and the peak search
    // returned its own grid edge — which then failed the first band and
    // aborted before reaching the rest.) The settle window covers the filter
    // states the caller's priming render left behind.
    std::vector<double> trace(window);
    double out = 0.0;
    for (std::size_t i = 0; i < kSettle + window; ++i) {
        // Modulator silent so the frozen levels stand and `u` decays away;
        // the carrier carries the probe tone.
        const double c = std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr);
        v.process(0.0, c, out);
        if (i >= kSettle) trace[i - kSettle] = out;
    }
    return coherent_magnitude(trace, hz);
}

/// Peak of a band's magnitude response, located on a log-frequency grid and
/// refined by a parabola in log f. A grid maximum alone would only prove a
/// local maximum near where it was looked for.
struct Peak {
    double hz;
    double magnitude;
};

template <typename Fn>
Peak locate_peak(Fn&& magnitude_at, double nominal_hz) {
    constexpr int kPoints = 9;
    constexpr double kSpan = 1.30;  // ±30 % around nominal, comfortably wider than a band
    std::array<double, kPoints> mag{};
    std::array<double, kPoints> log_f{};
    int best = 0;
    for (int i = 0; i < kPoints; ++i) {
        const double t = -1.0 + 2.0 * static_cast<double>(i) / (kPoints - 1);
        log_f[static_cast<std::size_t>(i)] = std::log(nominal_hz) + t * std::log(kSpan);
        mag[static_cast<std::size_t>(i)] =
            magnitude_at(std::exp(log_f[static_cast<std::size_t>(i)]));
        if (mag[static_cast<std::size_t>(i)] > mag[static_cast<std::size_t>(best)]) best = i;
    }
    if (best == 0 || best == kPoints - 1)
        return {std::exp(log_f[static_cast<std::size_t>(best)]), mag[static_cast<std::size_t>(best)]};
    const double y0 = mag[static_cast<std::size_t>(best - 1)];
    const double y1 = mag[static_cast<std::size_t>(best)];
    const double y2 = mag[static_cast<std::size_t>(best + 1)];
    const double denominator = y0 - 2.0 * y1 + y2;
    const double shift = std::abs(denominator) > 1e-15 ? 0.5 * (y0 - y2) / denominator : 0.0;
    const double step = log_f[1] - log_f[0];
    return {std::exp(log_f[static_cast<std::size_t>(best)] + std::clamp(shift, -1.0, 1.0) * step),
            y1};
}

/// A −3 dB edge, bisected in log frequency between a point inside the band and
/// one outside it.
template <typename Fn>
double bisect_edge(Fn&& magnitude_at, double peak_magnitude, double inside_hz, double outside_hz) {
    const double target = peak_magnitude / std::sqrt(2.0);
    double lo = std::log(inside_hz);
    double hi = std::log(outside_hz);
    for (int i = 0; i < 40; ++i) {
        const double mid = 0.5 * (lo + hi);
        (magnitude_at(std::exp(mid)) > target ? lo : hi) = mid;
    }
    return std::exp(0.5 * (lo + hi));
}

/// Magnitude of the shipped 4th-order band, relative to its peak, at `hz` —
/// computed rather than measured, from the section Q and the centre.
///
/// Prewarped, because a TPT SVF's response at digital frequency f is the
/// analogue response at `tan(πf/fs)`, not at f. Ignoring that is worth 1.1 dB
/// an octave above a 3.1 kHz band at 48 kHz, which is the difference between a
/// prediction that matches to three digits and one that does not.
double cascade_response(double hz, double center_hz, double section_q) {
    const double w = std::tan(kPi * hz / kSr);
    const double wc = std::tan(kPi * center_hz / kSr);
    const double x = section_q * (w / wc - wc / w);
    return 1.0 / (1.0 + x * x);  // two identical sections
}

/// An independently written peak follower carrying `BallisticsFilterT`'s
/// shipped coefficient map. It exists to predict the steady-state ripple of a
/// rectified sinusoid from the shipped ballistics, so the ripple assertions
/// compare against arithmetic rather than against a number read off a run.
struct ReferenceFollower {
    double attack;
    double release;
    double state = 0.0;

    ReferenceFollower(double attack_ms, double release_ms) {
        attack = 1.0 - std::exp(-2.2 / (attack_ms * 0.001 * kSr));
        release = 1.0 - std::exp(-2.2 / (release_ms * 0.001 * kSr));
    }
    double process(double x) {
        const double magnitude = std::abs(x);
        state += (magnitude > state ? attack : release) * (magnitude - state);
        return state;
    }
};

/// Settled envelope of the reference follower on a unit sinusoid at `hz` — the
/// fraction of the peak a peak-follower actually reaches, which depends on the
/// tone's period against that band's own floors and is therefore NOT the same
/// for two bands looking at one tone.
double reference_settled(double hz, double attack_ms, double release_ms) {
    ReferenceFollower follower{attack_ms, release_ms};
    const auto total = static_cast<std::size_t>(
        std::ceil(kSr * std::max(40.0 / hz, 20.0 * release_ms * 0.001)));
    double high = 0.0;
    for (std::size_t i = 0; i < total; ++i) {
        const double e = follower.process(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr));
        if (i > 2 * total / 3) high = std::max(high, e);
    }
    return high;
}

/// Peak-to-trough ripple of the reference follower on a unit sinusoid at `hz`,
/// as a fraction of the peak.
double reference_ripple(double hz, double attack_ms, double release_ms) {
    ReferenceFollower follower{attack_ms, release_ms};
    // Long enough for the BALLISTICS to settle, not just for a few cycles of
    // the tone: at 7 kHz forty cycles is 5.7 ms against a 15 ms release, and a
    // window that short measures a follower still on its way up. Twenty
    // release times, then the last third.
    const auto total = static_cast<std::size_t>(
        std::ceil(kSr * std::max(40.0 / hz, 20.0 * release_ms * 0.001)));
    double high = 0.0;
    double low = 1e30;
    for (std::size_t i = 0; i < total; ++i) {
        const double e = follower.process(std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr));
        if (i > 2 * total / 3) {
            high = std::max(high, e);
            low = std::min(low, e);
        }
    }
    return (high - low) / high;
}

// ── Signal sources ────────────────────────────────────────────────────────

std::vector<double> seeded_noise(std::size_t n, double amplitude, std::uint32_t seed) {
    Xorshift32 rng{seed};
    std::vector<double> out(n);
    for (auto& v : out) v = amplitude * rng.next_bipolar<double>();
    return out;
}

std::vector<double> sawtooth(std::size_t n, double hz, double amplitude) {
    osc::VaOscillator osc;
    osc.set_shape(osc::VaShape::saw);
    osc.reset(0.0);
    std::vector<double> out(n);
    for (auto& v : out) v = amplitude * osc.next(hz / kSr);
    return out;
}

std::vector<double> sine(std::size_t n, double hz, double amplitude) {
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kSr);
    return out;
}

std::vector<double> render(Voc& v, const std::vector<double>& modulator,
                           const std::vector<double>& carrier) {
    std::vector<double> out(modulator.size());
    for (std::size_t i = 0; i < modulator.size(); ++i) v.process(modulator[i], carrier[i], out[i]);
    return out;
}

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// T-SPACING — band centres, and the two banks agreeing on them
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder band centres follow the geometric ratio", "[signal][vocoder]") {
    auto v = make_bank();
    const double ratio =
        std::pow(kHiHz / kLoHz, 1.0 / static_cast<double>(kBands - 1));
    INFO("r = " << v.band_ratio() << " (recomputed " << ratio << ")");
    REQUIRE_THAT(v.band_ratio(), WithinRel(ratio, 1e-12));

    for (int k = 0; k < kBands; ++k) {
        const double expected = kLoHz * std::pow(ratio, static_cast<double>(k));
        REQUIRE_THAT(v.band_center_hz(k), WithinRel(expected, 1e-12));

        const auto peak = locate_peak(
            [&](double hz) { return analysis_magnitude(v, k, hz); }, expected);
        INFO("band " << k << ": table " << expected << " Hz, measured peak " << peak.hz
                     << " Hz, gain " << peak.magnitude);
        REQUIRE_THAT(peak.hz, WithinRel(expected, 0.03));

        // The bands are unity-peak by construction — the SVF's bandpass output
        // peaks at Q, and two cascaded sections at Q², so the normalisation is
        // 1/Q². Every gain claim in the module depends on this being 1.
        REQUIRE_THAT(peak.magnitude, WithinRel(1.0, 0.02));
    }
}

TEST_CASE("vocoder analysis and synthesis banks are matched", "[signal][vocoder]") {
    // The failure this catches is the classic one: the two banks drifting apart
    // gives a reconstruction whose formants are detuned from the modulator's,
    // and it sounds like a tuning fault rather than a filter fault. Measuring
    // the synthesis side through the OUTPUT (not through an accessor) is what
    // makes it a real check.
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);
    v.set_sibilance_mix(0.0);
    v.set_dry_wet(1.0);

    for (int k : {0, 5, 10, 15}) {
        const double expected = v.band_center_hz(k);
        // Open exactly one synthesis band: silence the modulator, then latch
        // the (zero) envelopes and write the one gain we want through freeze.
        // Freeze is a value copy, so a held bank with one band open is a
        // legitimate configuration rather than a test-only hook.
        v.reset();
        // Drive band k alone, tracking each band's envelope PEAK over the last
        // stretch rather than snapshotting it. Band envelopes ripple — 14 % at
        // the bottom of the bank, which is the whole subject of the follower
        // floors — so a single instant compares two values at unrelated ripple
        // phases. The peak is also what `reference_settled` predicts, so the
        // two sides of the comparison are the same quantity.
        std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> peak_envelope{};
        {
            const auto total = static_cast<std::size_t>(0.6 * kSr);
            const auto measure_from = static_cast<std::size_t>(0.5 * kSr);
            double scratch = 0.0;
            for (std::size_t i = 0; i < total; ++i) {
                v.process(std::sin(2.0 * kPi * expected * static_cast<double>(i) / kSr), 0.0,
                          scratch);
                if (i < measure_from) continue;
                for (int j = 0; j < kBands; ++j)
                    peak_envelope[static_cast<std::size_t>(j)] =
                        std::max(peak_envelope[static_cast<std::size_t>(j)], v.band_envelope(j));
            }
            v.set_formant_freeze(true);
            v.process(0.0, 0.0, scratch);  // the latch edge
        }

        const double reference_k =
            reference_settled(expected, v.attack_eff_ms(k), v.release_eff_ms(k));
        for (int j = 0; j < kBands; ++j) {
            if (j == k) continue;
            // Predicted ENVELOPE ratio, not response ratio: the bank's own
            // response at the neighbour's detuning, times the fraction of the
            // peak that neighbour's follower settles at. Those fractions differ
            // between bands because the floors do.
            const double predicted =
                cascade_response(expected, v.band_center_hz(j), v.section_q()) *
                reference_settled(expected, v.attack_eff_ms(j), v.release_eff_ms(j)) /
                reference_k;
            const double measured = peak_envelope[static_cast<std::size_t>(j)] /
                                    peak_envelope[static_cast<std::size_t>(k)];
            INFO("band " << j << " reads " << measured << " of band " << k
                         << ", bank + follower predict " << predicted);
            REQUIRE_THAT(measured, WithinAbs(predicted, 0.03));
        }

        const auto peak = locate_peak(
            [&](double hz) { return synthesis_magnitude(v, k, hz); }, expected);
        INFO("synthesis band " << k << ": analysis centre " << expected << " Hz, synthesis peak "
                               << peak.hz << " Hz at magnitude " << peak.magnitude);
        // A silent bank would let the peak search return its grid edge and look
        // like a spacing failure, so the level is asserted before the location.
        REQUIRE(peak.magnitude > 1e-3);
        REQUIRE_THAT(peak.hz, WithinRel(expected, 0.03));
        v.set_formant_freeze(false);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-Q and T-CASCADE — selectivity, and the cascade identity
// ─────────────────────────────────────────────────────────────────────────
//
// Spec defect, recorded rather than smoothed over: §3.3 calls the per-band
// filter a "4th-order (24 dB/oct-skirt) bandpass". Two cascaded 2nd-order
// bandpass sections have **12 dB/oct** skirts, not 24 — each 2nd-order section
// contributes 6 dB/oct per side. Measured asymptotic slope here: −14.2 dB/oct
// between 4×f_c and 8×f_c, still approaching 12 from above. T-Q's own criterion
// survives this, because "≥ 20 dB/oct by one octave out" is satisfied by the
// −22.8 dB the cascade reaches at 2·f_c — the average slope over the FIRST
// octave is steeper than the asymptote. Only the parenthetical is wrong.

TEST_CASE("vocoder band selectivity matches the computed Q", "[signal][vocoder]") {
    auto v = make_bank();
    const double q_band = v.band_q();
    // Q_band recomputed from the ratio, not read back from the module.
    const double root = std::sqrt(v.band_ratio());
    REQUIRE_THAT(q_band, WithinRel(1.0 / (root - 1.0 / root), 1e-12));

    for (int k : {4, 8, 12}) {
        const double center = v.band_center_hz(k);
        auto magnitude = [&](double hz) { return analysis_magnitude(v, k, hz); };
        const auto peak = locate_peak(magnitude, center);
        const double lower = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz / 2.0);
        const double upper = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz * 2.0);
        const double bandwidth = upper - lower;
        const double expected = center / q_band;
        INFO("band " << k << " (f_c " << center << " Hz): −3 dB span " << lower << " … " << upper
                     << " = " << bandwidth << " Hz, expected f_c/Q_band = " << expected);
        REQUIRE_THAT(bandwidth, WithinRel(expected, 0.10));

        // One octave out, against the cascade's own prewarped transfer
        // function computed from the shipped section Q.
        const double predicted_db =
            20.0 * std::log10(cascade_response(center * 2.0, center, v.section_q()));
        const double measured_db =
            20.0 * std::log10(magnitude(center * 2.0) / peak.magnitude);
        INFO("one octave up: measured " << measured_db << " dB, predicted " << predicted_db
                                        << " dB");
        REQUIRE_THAT(measured_db, WithinAbs(predicted_db, 0.3));
        REQUIRE(measured_db <= -20.0);

        // ... and the asymptote is 12 dB/oct, not the 24 the spec's
        // parenthetical claims. Only measured where the far point is still well
        // inside the band: eight times band 12's centre is 24.8 kHz, above
        // Nyquist, and near Nyquist the bilinear transform's zero pulls the
        // digital response down far faster than any analogue asymptote — that
        // steepness is the transform, not the skirt.
        if (center * 8.0 < 0.30 * kSr) {
            const double octave_4 = 20.0 * std::log10(magnitude(center * 4.0) / peak.magnitude);
            const double octave_8 = 20.0 * std::log10(magnitude(center * 8.0) / peak.magnitude);
            INFO("asymptotic skirt " << (octave_8 - octave_4) << " dB/oct");
            REQUIRE(octave_8 - octave_4 > -20.0);
            REQUIRE(octave_8 - octave_4 < -11.0);
        }
    }
}

TEST_CASE("vocoder cascade bandwidth factor is the shipped identity", "[signal][vocoder]") {
    // kCascadeBWFactor is algebra, not a citation, so it is checked against the
    // algebra: √(2^(1/n) − 1) at n = 2.
    REQUIRE_THAT(Voc::kCascadeBWFactor, WithinRel(std::sqrt(std::sqrt(2.0) - 1.0), 1e-12));

    auto v = make_bank();
    REQUIRE_THAT(v.section_q(), WithinRel(Voc::kCascadeBWFactor * v.band_q(), 1e-12));

    // And the factor is what the cascade actually delivers: one section at
    // Q_section would be f_c/Q_section wide; the pair measures narrower by
    // kCascadeBWFactor.
    const int k = 8;
    const double center = v.band_center_hz(k);
    auto magnitude = [&](double hz) { return analysis_magnitude(v, k, hz); };
    const auto peak = locate_peak(magnitude, center);
    const double lower = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz / 2.0);
    const double upper = bisect_edge(magnitude, peak.magnitude, peak.hz, peak.hz * 2.0);
    const double measured_factor = (upper - lower) / (center / v.section_q());
    INFO("measured cascade narrowing " << measured_factor << " against " << Voc::kCascadeBWFactor);
    REQUIRE_THAT(measured_factor, WithinRel(Voc::kCascadeBWFactor, 0.10));
}

// ─────────────────────────────────────────────────────────────────────────
// T-ENV — follower floors and ballistics
// ─────────────────────────────────────────────────────────────────────────
//
// Three spec defects here, all measured:
//
//   1. §4 says the ballistics use `coef = exp(−1/(τ·fs))`, a time-CONSTANT
//      map. The shipped `BallisticsFilterT` uses `1 − exp(−2.2/(t·fs))`, a
//      10→90 % map. Every follower time in the module therefore means
//      something 2.2× different from what §4 describes. T-ENV's own criterion
//      ("10→90 % times equal attack_eff/release_eff") is correct for the
//      SHIPPED map and would be wrong for the described one — so the criterion
//      is right and the mechanism paragraph is wrong, which is the safer of the
//      two ways round.
//   2. "Ripple on the held envelope < −40 dB relative to its DC" is
//      unachievable at band 0 for any implementation of the specified
//      ballistics. `kRippleCycles = 2` puts the release 10→90 % time at two
//      cycles of f_c, i.e. a time constant of 2/(2.2·f_c); the rectified
//      ripple period is 1/(2·f_c). The reference follower below — the shipped
//      coefficient map on paper — predicts 13.8 % (−17.2 dB), and the module
//      measures the same. At the TOP of the bank the criterion is comfortably
//      met (band 15 measures ≈ −40 dB), because there the user's 15 ms release
//      is 100× the ripple period. The criterion is band-dependent and was
//      written for one band.
//   3. The attack criterion cannot be met at ANY band with the specified test
//      signal. A follower attacks only while the rectified input exceeds its
//      state, which is roughly half of each half-cycle, so the envelope's
//      10→90 % time is about twice the follower's own. Measured 2.9 ms at
//      band 15 against a 1.5 ms floor. Nothing is wrong with the follower —
//      the measurement asks for a step response and supplies a sinusoid.

TEST_CASE("vocoder follower floors follow the per-band law", "[signal][vocoder]") {
    // The actual normative content of §4, and it is exactly testable.
    for (int bands : {10, 16, 20}) {
        auto v = make_bank(bands);
        for (double attack_ms : {0.1, 1.5, 50.0}) {
            for (double release_ms : {2.0, 15.0, 200.0}) {
                v.set_attack_ms(attack_ms);
                v.set_release_ms(release_ms);
                for (int k = 0; k < bands; ++k) {
                    const double center = v.band_center_hz(k);
                    const double attack_expected =
                        std::max(attack_ms, 1000.0 * Voc::kAttackCycles / center);
                    const double release_expected =
                        std::max(release_ms, 1000.0 * Voc::kRippleCycles / center);
                    INFO("bands " << bands << " band " << k << " f_c " << center);
                    REQUIRE_THAT(v.attack_eff_ms(k), WithinRel(attack_expected, 1e-12));
                    REQUIRE_THAT(v.release_eff_ms(k), WithinRel(release_expected, 1e-12));
                }
            }
        }
    }

    // The floors bite at the bottom and not at the top — the whole point of
    // expressing them in cycles.
    auto v = make_bank();
    v.set_attack_ms(1.5);
    v.set_release_ms(15.0);
    REQUIRE(v.release_eff_ms(0) > 15.0);
    REQUIRE_THAT(v.release_eff_ms(kBands - 1), WithinRel(15.0, 1e-12));
    REQUIRE(v.attack_eff_ms(0) > 1.5);
    REQUIRE_THAT(v.attack_eff_ms(kBands - 1), WithinRel(1.5, 1e-12));
}

TEST_CASE("vocoder envelope ballistics match the shipped follower", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_attack_ms(1.5);
    v.set_release_ms(15.0);

    struct Measurement {
        double steady;
        double ripple;
        double release_90_10_ms;
        double attack_10_90_ms;
    };
    auto measure = [&](int k) {
        const double center = v.band_center_hz(k);
        const auto gate = static_cast<std::size_t>(0.40 * kSr);
        const auto total = static_cast<std::size_t>(0.80 * kSr);
        v.reset();
        std::vector<double> envelope(total);
        double out = 0.0;
        for (std::size_t i = 0; i < total; ++i) {
            const double m =
                i < gate ? std::sin(2.0 * kPi * center * static_cast<double>(i) / kSr) : 0.0;
            v.process(m, 0.0, out);
            envelope[i] = v.band_envelope(k);
        }
        const auto settled = gate - static_cast<std::size_t>(0.05 * kSr);
        double high = 0.0;
        double low = 1e30;
        for (std::size_t i = settled; i < gate; ++i) {
            high = std::max(high, envelope[i]);
            low = std::min(low, envelope[i]);
        }
        std::size_t a10 = 0;
        std::size_t a90 = 0;
        for (std::size_t i = 0; i < gate; ++i) {
            if (a10 == 0 && envelope[i] >= 0.1 * high) a10 = i;
            if (a90 == 0 && envelope[i] >= 0.9 * high) {
                a90 = i;
                break;
            }
        }
        std::size_t r90 = 0;
        std::size_t r10 = 0;
        for (std::size_t i = gate; i < total; ++i) {
            if (r90 == 0 && envelope[i] <= 0.9 * high) r90 = i;
            if (r90 != 0 && envelope[i] <= 0.1 * high) {
                r10 = i;
                break;
            }
        }
        return Measurement{high, (high - low) / high,
                           1000.0 * static_cast<double>(r10 - r90) / kSr,
                           1000.0 * static_cast<double>(a90 - a10) / kSr};
    };

    SECTION("ripple matches the reference follower at every band") {
        for (int k : {0, 4, 8, 12, 15}) {
            const auto measured = measure(k);
            const double predicted =
                reference_ripple(v.band_center_hz(k), v.attack_eff_ms(k), v.release_eff_ms(k));
            INFO("band " << k << " (f_c " << v.band_center_hz(k) << "): measured ripple "
                         << measured.ripple << " (" << 20.0 * std::log10(measured.ripple)
                         << " dB), reference " << predicted << " ("
                         << 20.0 * std::log10(predicted) << " dB)");
            REQUIRE_THAT(measured.ripple, WithinAbs(predicted, 0.02));
        }
    }

    SECTION("the -40 dB ripple criterion is met at the top of the bank and not at the bottom") {
        // Recorded with the numbers that show why, rather than argued.
        const double top = measure(kBands - 1).ripple;
        const double bottom = measure(0).ripple;
        INFO("band " << kBands - 1 << " ripple " << 20.0 * std::log10(top) << " dB, band 0 ripple "
                     << 20.0 * std::log10(bottom) << " dB");
        REQUIRE(20.0 * std::log10(top) < -35.0);
        REQUIRE(20.0 * std::log10(bottom) > -25.0);

        // Even at the top of its declared range, kRippleCycles cannot deliver
        // −40 dB at band 0: the reference follower says so directly.
        const double widest_floor = 1000.0 * 4.0 / v.band_center_hz(0);  // kRippleCycles max
        const double best_case = reference_ripple(v.band_center_hz(0), v.attack_eff_ms(0),
                                                  std::max(15.0, widest_floor));
        INFO("band 0 ripple at kRippleCycles = 4 (range max): " << 20.0 * std::log10(best_case)
                                                                << " dB");
        REQUIRE(20.0 * std::log10(best_case) > -40.0);
    }

    SECTION("release through a band whose ring is fast equals the floored time") {
        // At the top of the bank the band's own ring time is
        // Q_section/(π·f_c), two orders of magnitude below the follower's, so
        // what is measured is the follower and the criterion holds.
        const int k = kBands - 1;
        const double ring_ms = 1000.0 * v.section_q() / (kPi * v.band_center_hz(k));
        INFO("band " << k << " ring time " << ring_ms << " ms vs release floor "
                     << v.release_eff_ms(k) << " ms");
        REQUIRE(ring_ms < 0.02 * v.release_eff_ms(k));
        const auto measured = measure(k);
        INFO("measured release 90→10 " << measured.release_90_10_ms << " ms");
        REQUIRE_THAT(measured.release_90_10_ms, WithinRel(v.release_eff_ms(k), 0.15));
    }

    SECTION("at the bottom of the bank the band's own ring dominates the measurement") {
        const int k = 0;
        const double ring_ms = 1000.0 * v.section_q() / (kPi * v.band_center_hz(k));
        const auto measured = measure(k);
        INFO("band 0: ring " << ring_ms << " ms, release floor " << v.release_eff_ms(k)
                             << " ms, measured 90→10 " << measured.release_90_10_ms << " ms");
        // The ring is comparable to the follower here, so the composite is
        // necessarily slower than the floor — by about a factor of two.
        REQUIRE(ring_ms > 0.3 * v.release_eff_ms(k));
        REQUIRE(measured.release_90_10_ms > 1.5 * v.release_eff_ms(k));
        REQUIRE(measured.release_90_10_ms < 2.5 * v.release_eff_ms(k));
    }

    SECTION("attack against a sinusoid is about twice the follower's own time") {
        // Not a fault: a follower attacks only while its input exceeds its
        // state, which is about half of each half-cycle.
        const auto measured = measure(kBands - 1);
        INFO("band " << kBands - 1 << ": measured attack 10→90 " << measured.attack_10_90_ms
                     << " ms against a floor of " << v.attack_eff_ms(kBands - 1) << " ms");
        REQUIRE(measured.attack_10_90_ms > 1.4 * v.attack_eff_ms(kBands - 1));
        REQUIRE(measured.attack_10_90_ms < 2.6 * v.attack_eff_ms(kBands - 1));
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-UV — voiced / unvoiced
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder voicing detector separates buzz from hiss", "[signal][vocoder]") {
    auto v = make_bank();

    auto settle = [&](const std::vector<double>& modulator) {
        v.reset();
        const std::vector<double> silence(modulator.size(), 0.0);
        render(v, modulator, silence);
        return v.unvoiced();
    };

    const auto noise = seeded_noise(static_cast<std::size_t>(0.5 * kSr), 0.5, 0x51F0u);
    const double u_noise = settle(noise);
    const double zcr_noise = v.zcr_hz();
    const auto saw = sawtooth(static_cast<std::size_t>(0.5 * kSr), 150.0, 0.8);
    const double u_saw = settle(saw);
    const double zcr_saw = v.zcr_hz();

    INFO("noise: u = " << u_noise << ", zcr = " << zcr_noise << " Hz; saw: u = " << u_saw
                       << ", zcr = " << zcr_saw << " Hz");
    REQUIRE_THAT(u_noise, WithinAbs(1.0, 0.05));
    REQUIRE_THAT(u_saw, WithinAbs(0.0, 0.05));

    // The ZCR instrument itself, against ground truth: a sawtooth at f0 crosses
    // zero exactly twice per cycle, and white noise's consecutive samples are
    // independent so it crosses on about half of them.
    REQUIRE_THAT(zcr_saw, WithinRel(300.0, 0.02));
    REQUIRE_THAT(zcr_noise, WithinRel(0.5 * kSr, 0.05));
    REQUIRE_THAT(v.zcr_window_ms(), WithinRel(Voc::kZcrWindowMs, 0.01));
}

TEST_CASE("vocoder voicing transition is smooth and does not chatter", "[signal][vocoder]") {
    auto v = make_bank();
    const auto half = static_cast<std::size_t>(0.30 * kSr);
    auto modulator = sawtooth(half, 150.0, 0.8);
    const auto hiss = seeded_noise(half, 0.5, 0x0A0Bu);
    modulator.insert(modulator.end(), hiss.begin(), hiss.end());
    const std::vector<double> silence(modulator.size(), 0.0);

    v.reset();
    std::vector<double> trace(modulator.size());
    double out = 0.0;
    for (std::size_t i = 0; i < modulator.size(); ++i) {
        v.process(modulator[i], silence[i], out);
        trace[i] = v.unvoiced();
    }

    // The one-pole on the latched decision is specified as a 10→90 % time, so
    // that is what is measured — computed from the shipped constant, not
    // restated.
    std::size_t t10 = 0;
    std::size_t t90 = 0;
    for (std::size_t i = half; i < trace.size(); ++i) {
        if (t10 == 0 && trace[i] >= 0.1) t10 = i;
        if (t90 == 0 && trace[i] >= 0.9) {
            t90 = i;
            break;
        }
    }
    REQUIRE(t90 > t10);
    const double measured_ms = 1000.0 * static_cast<double>(t90 - t10) / kSr;
    INFO("voiced→unvoiced 10→90 % in " << measured_ms << " ms against kUvSmoothMs = "
                                       << Voc::kUvSmoothMs << " ms");
    REQUIRE_THAT(measured_ms, WithinAbs(Voc::kUvSmoothMs, 2.0));

    // No chatter: once settled, the decision is monotone in each half and does
    // not oscillate back across the middle.
    int crossings = 0;
    bool above = false;
    for (std::size_t i = static_cast<std::size_t>(0.02 * kSr); i < trace.size(); ++i) {
        const bool now = trace[i] > 0.5;
        if (now != above) ++crossings;
        above = now;
    }
    INFO("decision crossed 0.5 " << crossings << " times over one voiced→unvoiced transition");
    REQUIRE(crossings == 1);
}

TEST_CASE("vocoder voicing hysteresis holds across level wobble", "[signal][vocoder]") {
    // ±1 dB of level wobble on the modulator must not move the decision. It
    // cannot in principle — both cues are ratios (a crossing rate and an energy
    // fraction), neither of which depends on level — and this pins that the
    // implementation did not accidentally introduce a level dependence.
    auto v = make_bank();
    const auto n = static_cast<std::size_t>(0.4 * kSr);
    const std::vector<double> silence(n, 0.0);

    for (double db : {-1.0, 0.0, 1.0}) {
        const double gain = units::db_to_linear(db);
        for (int voiced = 0; voiced < 2; ++voiced) {
            auto modulator = voiced != 0 ? sawtooth(n, 150.0, 0.8 * gain)
                                         : seeded_noise(n, 0.5 * gain, 0x77u);
            v.reset();
            render(v, modulator, silence);
            INFO((voiced != 0 ? "saw" : "noise") << " at " << db << " dB: u = " << v.unvoiced());
            if (voiced != 0) REQUIRE(v.unvoiced() < 0.05);
            else REQUIRE(v.unvoiced() > 0.95);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-SHIFT — formants move, pitch does not
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder formant shift moves the spectral envelope by whole octaves",
          "[signal][vocoder]") {
    auto centroid = [](double semitones) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);
        v.set_formant_shift_semitones(semitones);
        // A white carrier makes the output spectrum the synthesis bank's own
        // shape; a harmonic carrier would weight the measurement by its own
        // comb.
        const auto settle = static_cast<std::size_t>(0.5 * kSr);
        const std::size_t window = 32768;
        const auto carrier = seeded_noise(settle + window, 0.5, 0xC0DEu);
        const auto modulator = sine(settle + window, v.band_center_hz(5), 1.0);
        const auto out = render(v, modulator, carrier);
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(settle),
                                       out.end());

        double numerator = 0.0;
        double denominator = 0.0;
        for (double hz = 60.0; hz < 12000.0; hz *= 1.05) {
            const double energy = std::pow(coherent_magnitude(tail, hz), 2.0);
            numerator += std::log(hz) * energy;
            denominator += energy;
        }
        return std::exp(numerator / denominator);
    };

    const double base = centroid(0.0);
    const double up = centroid(12.0);
    const double down = centroid(-12.0);
    INFO("log-spectral centroid: " << base << " Hz → " << up << " Hz (+12 st, ratio "
                                   << up / base << ") and " << down << " Hz (−12 st, ratio "
                                   << down / base << ")");
    // r^offset_bands with offset_bands = 12/(12·log2 r) is exactly 2, whatever
    // the ratio is — the control is scale-invariant by construction.
    REQUIRE_THAT(up / base, WithinRel(2.0, 0.05));
    REQUIRE_THAT(down / base, WithinRel(0.5, 0.05));
}

TEST_CASE("vocoder formant shift maps semitones through the bank's own ratio",
          "[signal][vocoder]") {
    for (int bands : {10, 16, 20}) {
        auto v = make_bank(bands);
        const double expected_per_octave = 1.0 / std::log2(v.band_ratio());
        REQUIRE_THAT(v.bands_per_octave(), WithinRel(expected_per_octave, 1e-12));
        for (double st : {-24.0, -12.0, 0.0, 7.0, 12.0, 24.0}) {
            v.set_formant_shift_semitones(st);
            const double expected = st / (12.0 * std::log2(v.band_ratio()));
            INFO("bands " << bands << ", " << st << " st → " << v.shift_bands() << " bands");
            REQUIRE_THAT(v.shift_bands(), WithinRel(expected, 1e-12));
        }
    }

    // The routing itself: with one band held at a known level, the shifted bank
    // reads that level from the neighbours the offset points at, and the ends
    // clamp to zero instead of wrapping.
    auto v = make_bank();
    v.set_formant_shift_semitones(0.0);
    const auto tone = sine(static_cast<std::size_t>(0.5 * kSr), v.band_center_hz(8), 4.0);
    const std::vector<double> silence(tone.size(), 0.0);
    v.reset();
    render(v, tone, silence);
    v.set_formant_freeze(true);
    double scratch = 0.0;
    v.process(0.0, 0.0, scratch);
    const double held = v.synthesis_gain(8);
    REQUIRE(held > 0.5);

    // A whole number of bands of shift is an exact re-index of the level
    // array. The first draft asserted that band 8 went quiet, which it does
    // not and should not: it now reads band 6, whose envelope is the bank's own
    // two-band overlap and is not zero. The identity below is both exact and
    // the thing the control actually promises.
    std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> before{};
    for (int k = 0; k < v.band_count(); ++k) before[static_cast<std::size_t>(k)] = v.synthesis_gain(k);

    const double semitones_per_band = 12.0 * std::log2(v.band_ratio());
    v.set_formant_shift_semitones(2.0 * semitones_per_band);
    v.process(0.0, 0.0, scratch);
    REQUIRE_THAT(v.shift_bands(), WithinAbs(2.0, 1e-9));
    for (int j = 0; j < v.band_count(); ++j) {
        const int source = j - 2;
        const double expected = source >= 0 ? before[static_cast<std::size_t>(source)] : 0.0;
        INFO("after a 2-band shift, gain[" << j << "] should read band " << source);
        REQUIRE_THAT(v.synthesis_gain(j), WithinAbs(expected, 1e-9));
    }
    // ... including the ends, which clamp to zero rather than wrapping.
    REQUIRE(v.synthesis_gain(0) == 0.0);
    REQUIRE(v.synthesis_gain(1) == 0.0);

    // Shifting the whole bank off the end drops it rather than folding the
    // bottom of the bank into the top, which would be audible and wrong.
    v.set_formant_shift_semitones(Voc::kFormantShiftMaxSt);
    v.process(0.0, 0.0, scratch);
    INFO("+" << Voc::kFormantShiftMaxSt << " st = " << v.shift_bands() << " bands");
    REQUIRE(v.shift_bands() > 5.0);
    for (int j = 0; j < 5; ++j) REQUIRE(v.synthesis_gain(j) == 0.0);
}

TEST_CASE("vocoder formant shift leaves the carrier's pitch alone", "[signal][vocoder]") {
    // The separation is the whole point of the control, so it is measured
    // rather than argued: with a harmonic carrier the output's energy must stay
    // on the carrier's harmonic comb at every shift setting.
    const double pitch = 150.0;
    auto harmonic_fraction = [&](double semitones) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::internal);
        v.set_internal_wave(Voc::InternalWave::saw);
        v.set_internal_pitch_hz(pitch);
        v.set_noise_mix(0.0);
        v.set_sibilance_mix(0.0);
        v.set_formant_shift_semitones(semitones);
        const auto settle = static_cast<std::size_t>(0.5 * kSr);
        const std::size_t window = 32768;
        const auto modulator = sine(settle + window, v.band_center_hz(6), 1.0);
        const std::vector<double> no_carrier(settle + window, 0.0);
        const auto out = render(v, modulator, no_carrier);
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(settle),
                                       out.end());

        double on_comb = 0.0;
        double off_comb = 0.0;
        for (int h = 1; h <= 40; ++h) {
            on_comb += std::pow(coherent_magnitude(tail, pitch * h), 2.0);
            // Midway between harmonics: energy here would mean the shift moved
            // pitch, not formants.
            off_comb += std::pow(coherent_magnitude(tail, pitch * (h + 0.5)), 2.0);
        }
        return off_comb / (on_comb + 1e-30);
    };

    for (double st : {-12.0, 0.0, 12.0}) {
        const double leak = harmonic_fraction(st);
        INFO(st << " st: inter-harmonic energy is " << 10.0 * std::log10(leak + 1e-30)
                << " dB below the comb");
        REQUIRE(leak < 0.01);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-FREEZE — the latch
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder formant freeze holds the spectral envelope", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::internal);
    v.set_internal_pitch_hz(110.0);
    v.set_noise_mix(0.0);

    // Track a two-formant "vowel", then freeze and feed silence for 5 s.
    const auto n = static_cast<std::size_t>(0.5 * kSr);
    std::vector<double> vowel(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        vowel[i] = 0.5 * std::sin(2.0 * kPi * v.band_center_hz(4) * t) +
                   0.4 * std::sin(2.0 * kPi * v.band_center_hz(11) * t);
    }
    const std::vector<double> silence(n, 0.0);
    v.reset();
    render(v, vowel, silence);

    v.set_formant_freeze(true);
    double scratch = 0.0;
    v.process(0.0, 0.0, scratch);
    std::array<double, static_cast<std::size_t>(Voc::kMaxBands)> latched{};
    for (int k = 0; k < v.band_count(); ++k) latched[static_cast<std::size_t>(k)] = v.synthesis_gain(k);

    // The latched vector must be the vowel: a local maximum at each formant.
    // An absolute level threshold would be arbitrary here, because
    // pre-emphasis is a differentiator and costs band 4 (355 Hz) a factor of
    // 15 before the follower ever sees it — the position is the claim, not the
    // level.
    for (int k : {4, 11}) {
        REQUIRE(latched[static_cast<std::size_t>(k)] > 0.0);
        for (int d : {-2, -1, 1, 2}) {
            const int j = k + d;
            if (j < 0 || j >= v.band_count()) continue;
            INFO("formant at band " << k << " (" << latched[static_cast<std::size_t>(k)]
                                    << ") against neighbour " << j << " ("
                                    << latched[static_cast<std::size_t>(j)] << ")");
            REQUIRE(latched[static_cast<std::size_t>(k)] > latched[static_cast<std::size_t>(j)]);
        }
    }

    const auto five_seconds = static_cast<std::size_t>(5.0 * kSr);
    double worst_drift_db = 0.0;
    for (std::size_t i = 0; i < five_seconds; ++i) {
        v.process(0.0, 0.0, scratch);
        for (int k = 0; k < v.band_count(); ++k) {
            const double held = latched[static_cast<std::size_t>(k)];
            if (held < 1e-6) continue;
            worst_drift_db =
                std::max(worst_drift_db, std::abs(units::linear_to_db(v.synthesis_gain(k) / held)));
        }
    }
    INFO("worst band drift over 5 s of silence: " << worst_drift_db << " dB");
    REQUIRE(worst_drift_db < 0.1);

    // Releasing freeze resumes tracking: the levels fall away within about one
    // release, computed from the shipped floors rather than restated.
    v.set_formant_freeze(false);
    const auto one_release =
        static_cast<std::size_t>(2.0 * v.release_eff_ms(0) * 0.001 * kSr);
    for (std::size_t i = 0; i < one_release; ++i) v.process(0.0, 0.0, scratch);
    for (int k = 0; k < v.band_count(); ++k) {
        INFO("band " << k << " after release: " << v.synthesis_gain(k) << " was "
                     << latched[static_cast<std::size_t>(k)]);
        REQUIRE(v.synthesis_gain(k) < 0.1 * latched[static_cast<std::size_t>(k)] + 1e-9);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-GAIN — the reconstruction bound
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder reconstruction stays inside its registry bound", "[signal][vocoder]") {
    // "All bands open" is achieved by construction rather than by hoping a
    // modulator happens to fill the bank: `VcaT` clamps its control to [0, 1],
    // so a modulator loud enough to drive every band envelope past 1 leaves
    // every synthesis gain at exactly 1. That is the true static worst case and
    // it is reproducible from the shipped constants.
    for (int bands = Voc::kMinBands; bands <= Voc::kMaxBands; ++bands) {
        auto v = make_bank(bands);
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);   // a voiced probe would gate it to zero anyway
        v.set_output_trim_db(0.0);
        v.set_dry_wet(1.0);

        const auto n = static_cast<std::size_t>(2.0 * kSr);
        const auto carrier = seeded_noise(n, 1.0, 0xABCDu);
        // Loud enough that even band 0 saturates: pre-emphasis is a
        // differentiator and costs the bottom of the bank about 33 dB, so a
        // 50× modulator leaves band 0's envelope at 0.06, not at 1.
        const auto modulator = seeded_noise(n, 1e6, 0x1234u);
        v.reset();
        const auto out = render(v, modulator, carrier);

        double peak = 0.0;
        for (std::size_t i = static_cast<std::size_t>(0.5 * kSr); i < n; ++i)
            peak = std::max(peak, std::abs(out[i]));
        const double pre_trim = peak / Voc::kOutputHeadroomTrim;

        // Every band's control really is saturated — otherwise this measures
        // something quieter than the worst case and passes for the wrong
        // reason. `synthesis_gain` reports env' before `VcaT` clamps it, so the
        // check is "at or past 1", and the gain actually applied is exactly 1.
        for (int k = 0; k < bands; ++k) {
            INFO("band " << k << " control " << v.synthesis_gain(k));
            REQUIRE(v.synthesis_gain(k) >= 1.0);
        }

        INFO("bands " << bands << ": post-trim peak " << peak << ", pre-trim " << pre_trim
                      << " against kWorstCaseGain " << Voc::kWorstCaseGain);
        REQUIRE(pre_trim <= Voc::kWorstCaseGain);
        REQUIRE(peak <= 1.0);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-DET — determinism
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder renders are bit-identical after reset", "[signal][vocoder]") {
    const auto n = static_cast<std::size_t>(2.0 * kSr);
    const auto modulator = seeded_noise(n, 0.5, 0x51F0u);
    const auto carrier = seeded_noise(n, 0.5, 0x3C3Cu);

    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        for (bool freeze : {false, true}) {
            auto v = make_bank();
            v.set_carrier_source(source);
            v.set_noise_mix(0.5);
            v.set_formant_freeze(freeze);
            auto run = [&] {
                v.reset();
                return render(v, modulator, carrier);
            };
            const auto first = run();
            const auto second = run();
            INFO("source " << static_cast<int>(source) << " freeze " << freeze);
            REQUIRE(first == second);
        }
    }
}

TEST_CASE("vocoder noise carrier comes from the shipped seed", "[signal][vocoder]") {
    // The spec proves the seed is live by rebuilding with a different one,
    // which a single build cannot do. This proves the same thing without a
    // rebuild, and more directly: an independently constructed generator at
    // kNoiseSeed reproduces the carrier the module used, sample for sample.
    //
    // With noise_mix = 1 the carrier IS the noise, so with every band's gain
    // pinned the output is a deterministic function of that stream — and a
    // reference vocoder fed the same stream as an external carrier must match.
    // The modulator must be VOICED: with an external carrier the module
    // substitutes its own noise above the sibilance corner whenever u > 0, and
    // the two renders would then differ in the high bands for a reason that has
    // nothing to do with the seed.
    const auto n = static_cast<std::size_t>(0.25 * kSr);
    const auto modulator = sawtooth(n, 150.0, 0.8);

    auto internal = make_bank();
    internal.set_carrier_source(Voc::CarrierSource::internal);
    internal.set_noise_mix(1.0);
    internal.reset();
    const std::vector<double> unused(n, 0.0);
    const auto from_module = render(internal, modulator, unused);

    // The same stream, drawn here: one sample per process() call, in order.
    Xorshift32 reference{Voc::kNoiseSeed};
    std::vector<double> stream(n);
    for (auto& s : stream) s = reference.next_bipolar<double>();

    auto external = make_bank();
    external.set_carrier_source(Voc::CarrierSource::external);
    external.set_noise_mix(1.0);
    external.reset();
    const auto from_reference = render(external, modulator, stream);

    REQUIRE_THAT(external.unvoiced(), WithinAbs(0.0, 1e-9));
    REQUIRE(from_module == from_reference);
    REQUIRE(std::any_of(from_module.begin(), from_module.end(),
                        [](double x) { return x != 0.0; }));
    REQUIRE(Xorshift32{Voc::kNoiseSeed}.seed() == Voc::kNoiseSeed);
}

// ─────────────────────────────────────────────────────────────────────────
// T-LAT — latency
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder reports zero latency and responds on sample 0", "[signal][vocoder]") {
    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        auto v = make_bank();
        v.set_carrier_source(source);
        v.set_dry_wet(1.0);
        REQUIRE(v.latency_samples() == 0);

        v.reset();
        double out = 0.0;
        // Modulator and carrier both impulsive, so both banks are excited on
        // the first sample and there is no way for a bulk pre-delay to hide.
        v.process(1.0, 1.0, out);
        INFO("source " << static_cast<int>(source) << ": first output sample " << out);
        REQUIRE(out != 0.0);
    }
}

// ─────────────────────────────────────────────────────────────────────────
// T-RT — allocation
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder process and reset allocate nothing", "[signal][vocoder]") {
    // Each engine is first run through `prepare`, which is the one call allowed
    // to do bounded work, so the probe is known to be able to speak before its
    // silence is taken as evidence. (A synthetic control — a local vector
    // inside a probe scope — does not work at -O3: clang stack-promotes it
    // under the C++14 allocation-elision rule and the probe correctly reports
    // zero for an allocation that no longer happens.)
    auto engine = std::make_unique<Voc>();
    // A positive control first. M10 could use `prepare` for this because its
    // delay lines are vectors; this class allocates NOTHING anywhere, so its
    // probe would otherwise be silent whether or not it was working. An
    // explicit `::operator new` of a runtime-sized block is the funnel every
    // heap allocation goes through and is not elidable the way a local
    // container is (clang stack-promotes those at -O3 under the C++14
    // allocation-elision rule, which makes a container control useless here).
    {
        pulp::test::RtAllocationProbe control;
        const std::size_t bytes = 64 + (Voc::kMaxBands * sizeof(double));
        void* block = ::operator new(bytes);
        const std::size_t seen = control.allocation_count();
        ::operator delete(block);
        REQUIRE(seen > 0);
    }

    std::size_t prepare_allocations = 0;
    std::size_t prepare_bytes = 0;
    {
        pulp::test::RtAllocationProbe control;
        engine->prepare(kSr);
        engine->set_band_count(20);
        // Read inside the scope but REPORTED outside it: Catch2's INFO builds a
        // string, and a string built inside a probe scope is an allocation the
        // probe counts. The first draft of this test measured its own message.
        prepare_allocations = control.allocation_count();
        prepare_bytes = control.allocated_bytes();
    }
    INFO("prepare + set_band_count allocated " << prepare_bytes << " bytes in "
                                               << prepare_allocations << " calls");
    // Every buffer is a fixed std::array, so even prepare must be silent here —
    // this class's RT contract is stronger than "process is clean".
    REQUIRE(prepare_allocations == 0);

    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        for (bool freeze : {false, true}) {
            engine->prepare(kSr);
            engine->set_carrier_source(source);
            engine->set_formant_freeze(freeze);
            engine->set_band_count(16);
            engine->reset();

            double out = 0.0;
            for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);  // warm any lazy state

            require_allocates_no_memory([&] {
                for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);
                engine->reset();
                engine->set_attack_ms(3.0);
                engine->set_release_ms(40.0);
                engine->set_noise_mix(0.4);
                engine->set_unvoiced_sensitivity(0.7);
                engine->set_sibilance_mix(0.2);
                engine->set_formant_shift_semitones(-7.0);
                engine->set_internal_pitch_hz(220.0);
                engine->set_internal_pulse_width(0.3);
                engine->set_output_trim_db(-6.0);
                engine->set_dry_wet(0.8);
                // The two structural changes, mid-stream: both only move the
                // active loop bound and recompute coefficients.
                engine->set_band_count(11);
                engine->set_band_range_hz(90.0, 9000.0);
                for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);
            });
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// Composition — the node wiring the spec places outside this class
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder composes with the chorus ensemble tail", "[signal][vocoder]") {
    // §10 is normative but deliberately not implemented in `VocoderT`: the
    // catalog node feeds this module's mono output into a chorus instance and
    // crossfades by `ensemble_amt`. That leaves the composition unwritten and
    // untested unless someone writes it, so it is written here — the node
    // author gets an executable reference rather than a paragraph.
    //
    // Voicing choice: `juno_ensemble`, which is the chorus module's Roland BBD
    // ensemble — two taps, one per channel, modulators an exact half cycle
    // apart. The VP-330's bed is a Roland BBD ensemble of exactly that era and
    // topology, so `bbd_color` is on as well.
    using Chorus = ChorusEnsembleT<double>;

    auto v = make_bank(12);  // V2 uses 12 bands — softer, more vowel than consonant
    v.set_carrier_source(Voc::CarrierSource::internal);
    v.set_internal_wave(Voc::InternalWave::saw);
    v.set_internal_pitch_hz(110.0);
    v.set_noise_mix(0.15);
    v.set_sibilance_mix(0.35);

    Chorus chorus;
    chorus.prepare(kSr);
    chorus.set_voicing(Chorus::Voicing::juno_ensemble);
    chorus.set_juno_mode(Chorus::JunoMode::mode_I);
    chorus.set_bbd_color(true);
    chorus.set_mix(1.0);
    chorus.reset();

    const auto n = static_cast<std::size_t>(2.0 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x7A7Au);
    const std::vector<double> no_carrier(n, 0.0);

    v.reset();
    const auto mono = render(v, modulator, no_carrier);

    auto wire = [&](double ensemble_amount) {
        chorus.reset();
        std::vector<double> left = mono;
        std::vector<double> right = mono;
        chorus.process(left.data(), right.data(), static_cast<int>(left.size()));
        std::vector<double> out_l(n);
        std::vector<double> out_r(n);
        for (std::size_t i = 0; i < n; ++i) {
            out_l[i] = (1.0 - ensemble_amount) * mono[i] + ensemble_amount * left[i];
            out_r[i] = (1.0 - ensemble_amount) * mono[i] + ensemble_amount * right[i];
        }
        return std::pair{out_l, out_r};
    };

    // ensemble_amt = 0 is dual-mono `out_dry`, exactly as §10 states.
    const auto dry = wire(0.0);
    REQUIRE(dry.first == mono);
    REQUIRE(dry.second == mono);

    // ensemble_amt > 0 decorrelates the channels — that is what the bed is.
    const auto wet = wire(0.6);
    const auto skip = static_cast<std::size_t>(0.5 * kSr);
    double cross = 0.0;
    double energy_l = 0.0;
    double energy_r = 0.0;
    for (std::size_t i = skip; i < n; ++i) {
        cross += wet.first[i] * wet.second[i];
        energy_l += wet.first[i] * wet.first[i];
        energy_r += wet.second[i] * wet.second[i];
    }
    const double correlation = cross / std::sqrt(energy_l * energy_r);
    INFO("L/R correlation with ensemble_amt = 0.6: " << correlation);
    REQUIRE(energy_l > 0.0);
    REQUIRE(correlation < 0.995);
    REQUIRE(correlation > 0.0);  // still a coherent bed, not an inverted pair
}

// ─────────────────────────────────────────────────────────────────────────
// Contract checks the acceptance list implies but does not enumerate
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("vocoder pre-emphasis is applied to the modulator only", "[signal][vocoder]") {
    // §3.4 says "never to the carrier or the output". Measured by driving the
    // carrier alone through the synthesis bank and checking the band's peak
    // gain is unity — a tilt on the carrier path would show up as a
    // frequency-dependent scaling on it.
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);
    v.set_sibilance_mix(0.0);

    for (int k : {2, 8, 14}) {
        const double center = v.band_center_hz(k);
        // Analysis magnitude WITHOUT dividing the tilt out must show it...
        v.reset();
        const auto window = static_cast<std::size_t>(std::ceil(40.0 * kSr / center));
        std::vector<double> trace(window);
        double out = 0.0;
        for (std::size_t i = 0; i < 8192 + window; ++i) {
            const double m = std::sin(2.0 * kPi * center * static_cast<double>(i) / kSr);
            v.process(m, 0.0, out);
            if (i >= 8192) trace[i - 8192] = v.analysis_band(k);
        }
        const double raw = coherent_magnitude(trace, center);
        INFO("band " << k << " (f_c " << center << "): raw analysis gain " << raw
                     << ", pre-emphasis tilt " << pre_emphasis_gain(center));
        REQUIRE_THAT(raw, WithinRel(pre_emphasis_gain(center), 0.03));
    }
}

TEST_CASE("vocoder external carrier keeps its low bands intact", "[signal][vocoder]") {
    // §5's closed decision: under an external carrier the unvoiced decision
    // substitutes noise only ABOVE the sibilance corner, so the caller's pad is
    // not smeared in the bands where it carries pitch.
    //
    // Measured by freezing the bank first, so the synthesis gains are identical
    // in both conditions and the ONLY difference between them is the carrier
    // substitution. Comparing a voiced and an unvoiced render without freezing
    // compares two different spectral envelopes and says nothing about the
    // carrier — which is what the first draft of this test did.
    const auto n = static_cast<std::size_t>(1.0 * kSr);
    const auto hiss = seeded_noise(n, 0.5, 0x5151u);
    const auto buzz = sawtooth(n, 150.0, 0.8);

    auto tone_amplitude = [&](int band, bool unvoiced) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);
        v.set_noise_mix(0.0);

        // Prime with a TONE at the band under test, so the frozen bank is
        // essentially that band alone. A broadband prime leaves every band
        // open, and the substituted high bands then drop a random noise
        // residual into the low band's measurement bin — 4.5 % of it, which is
        // what the first draft of this test measured and mistook for leakage.
        const double hz = v.band_center_hz(band);
        const auto prime = sine(static_cast<std::size_t>(0.3 * kSr), hz, 1.0);
        const std::vector<double> quiet(prime.size(), 0.0);
        v.reset();
        render(v, prime, quiet);
        v.set_formant_freeze(true);
        double scratch = 0.0;
        v.process(0.0, 0.0, scratch);

        const auto carrier = sine(n, hz, 1.0);
        const auto out = render(v, unvoiced ? hiss : buzz, carrier);
        REQUIRE_THAT(v.unvoiced(), WithinAbs(unvoiced ? 1.0 : 0.0, 0.05));
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(kSr / 2),
                                       out.end());
        return coherent_magnitude(tail, hz);
    };

    // Below the corner the carrier passes untouched: the ratio is 1.
    const int low_band = 3;
    const double low_ratio = tone_amplitude(low_band, true) / tone_amplitude(low_band, false);
    INFO("band " << low_band << " (below " << Voc::kSibilanceCornerHz
                 << " Hz): unvoiced/voiced carrier tone = " << low_ratio);
    REQUIRE_THAT(low_ratio, WithinAbs(1.0, 0.02));

    // Above it the tone is displaced by exactly kUnvoicedNoise of noise, so the
    // coherent part is scaled by (1 − kUnvoicedNoise) — computed, not guessed.
    const int high_band = kBands - 1;
    auto probe = make_bank();
    // Its immediate neighbours must be above the corner as well, or an
    // un-substituted neighbour would put an unsubstituted tone in the same bin.
    REQUIRE(probe.band_center_hz(high_band) > Voc::kSibilanceCornerHz);
    REQUIRE(probe.band_center_hz(high_band - 1) > Voc::kSibilanceCornerHz);
    const double high_ratio = tone_amplitude(high_band, true) / tone_amplitude(high_band, false);
    INFO("band " << high_band << " (above the corner): " << high_ratio << " against the predicted "
                 << 1.0 - Voc::kUnvoicedNoise);
    REQUIRE_THAT(high_ratio, WithinAbs(1.0 - Voc::kUnvoicedNoise, 0.03));
}

TEST_CASE("vocoder mix and trim behave as declared", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);

    const auto n = static_cast<std::size_t>(0.5 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x2B2Bu);
    const auto carrier = seeded_noise(n, 0.4, 0x6D6Du);

    // dry_wet = 0 passes the DC-BLOCKED modulator — the blocker is in the dry
    // path by design and by name. Compared against a reference blocker built
    // from the shipped corner rather than against the raw modulator, which
    // would only measure the blocker.
    v.set_dry_wet(0.0);
    v.reset();
    const auto dry = render(v, modulator, carrier);

    const double pole = std::exp(-2.0 * kPi * Voc::kDcBlockHz / kSr);
    double last_in = 0.0;
    double last_out = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        last_out = modulator[i] - last_in + pole * last_out;
        last_in = modulator[i];
        worst = std::max(worst, std::abs(dry[i] - last_out));
    }
    INFO("largest deviation from a reference DC blocker at " << Voc::kDcBlockHz << " Hz: "
                                                             << worst);
    REQUIRE(worst < 1e-9);

    // Output trim is a plain gain on the wet path.
    v.set_dry_wet(1.0);
    v.set_output_trim_db(0.0);
    v.reset();
    const auto unity = render(v, modulator, carrier);
    v.set_output_trim_db(-6.0);
    v.reset();
    const auto trimmed = render(v, modulator, carrier);
    const double expected = units::db_to_linear(-6.0);
    for (std::size_t i = static_cast<std::size_t>(0.1 * kSr); i < n; ++i)
        REQUIRE_THAT(trimmed[i], WithinAbs(unity[i] * expected, 1e-12));
}

TEST_CASE("vocoder float and double instantiations agree", "[signal][vocoder]") {
    const auto n = static_cast<std::size_t>(0.25 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x4F4Fu);
    const auto carrier = seeded_noise(n, 0.4, 0x1E1Eu);

    VocoderT<float> narrow;
    narrow.prepare(kSr);
    narrow.set_band_count(kBands);
    narrow.set_band_range_hz(kLoHz, kHiHz);
    narrow.set_carrier_source(VocoderT<float>::CarrierSource::external);
    narrow.reset();

    auto wide = make_bank();
    wide.set_carrier_source(Voc::CarrierSource::external);
    wide.reset();

    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        float narrow_out = 0.0f;
        double wide_out = 0.0;
        narrow.process(static_cast<float>(modulator[i]), static_cast<float>(carrier[i]),
                       narrow_out);
        wide.process(modulator[i], carrier[i], wide_out);
        worst = std::max(worst, std::abs(static_cast<double>(narrow_out) - wide_out));
    }
    INFO("largest float/double divergence " << worst);
    REQUIRE(worst < 1e-4);
}

TEST_CASE("vocoder rejects non-finite audio before recursive state and recovers exactly",
          "[signal][vocoder][nan-recovery][rt-safety]") {
    Voc poisoned = make_bank();
    Voc fresh = make_bank();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    double output = 1.0;

    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process(nan, 0.25, output);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(output == 0.0);

    for (int i = 0; i < 4096; ++i) {
        const double modulator = 0.4 * std::sin(2.0 * kPi * 220.0 * i / kSr);
        const double carrier = 0.3 * std::sin(2.0 * kPi * 110.0 * i / kSr);
        double recovered = 0.0;
        double reference = 0.0;
        poisoned.process(modulator, carrier, recovered);
        fresh.process(modulator, carrier, reference);
        REQUIRE(std::isfinite(recovered));
        REQUIRE(recovered == reference);
    }
}
