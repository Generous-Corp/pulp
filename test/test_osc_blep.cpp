// polyBLEP / BLAMP correction kernels: analytic invariants, and measured alias
// rejection against the trivial waveform.
//
// ── How the alias claims here are measured, and why ────────────────────────
//
// Every alias number below comes from `measure_aliasing`, which fits the ideal
// harmonic series and the fold-back site of every harmonic above Nyquist JOINTLY
// by least squares and reads the amplitude off the fit. That is projection, not
// windowed-FFT bin classification: there is no leakage skirt for a 0 dB
// fundamental to bury a small alias under, so the measurement has dynamic range
// to spare at the levels these tests gate on.
//
// Three disciplines the alias claims here obey, each with a failure mode
// attached to skipping it:
//
//   1. **Every claim is band-qualified to 20 kHz.** A full-band "no alias above
//      X" claim is impossible for ANY method: fold-back is continuous across
//      Nyquist, so a component at Nyquist + eps lands at Nyquist - eps where no
//      correction can reach it. `AliasReport::full_band_worst_alias_db` reports
//      the unqualified number and is characterized here — never gated on. The
//      band-qualification test below demonstrates that the qualifier is
//      load-bearing rather than decorative.
//
//   2. **The detection floor is PROVEN by negative control, not derived.** The
//      analyzer's own `detection_floor_db` is a ~2 sigma bound that assumes a
//      white residual — an assumption that fails precisely here, because aliases
//      are discrete tones, not noise. Asserting that number would be the
//      analyzer grading its own homework. Instead the first alias test measures
//      a signal whose alias content is zero BY CONSTRUCTION and shows the
//      reading collapses, then recovers injected aliases of known level.
//
//   3. **A gate must sit above the floor, and the fit must have resolved.** Each
//      alias assertion also checks the reading against the measurement's own
//      floor and rejects an unresolved in-band alias, so a fit that could not
//      separate two landing sites reports inconclusive rather than passing.
//
// ── What these gates are, and are not ─────────────────────────────────────
//
// polyBLEP is a two-point approximation. It buys roughly 11-14 dB over the
// trivial waveform across the tested band — NOT the ~100 dB a tabulated
// minBLEP/BLIT reaches, and these tests do not pretend otherwise. The
// improvement gates are set at 8 dB: below the measured minimum (11.58 dB) with
// margin for platform arithmetic, and far above the two physically meaningful
// landmarks — 0 dB, meaning the kernel did nothing, and negative, meaning it did
// harm. They are regression tripwires on a cheap kernel, not a certification of
// its absolute quality. The measured figure at each f0 is logged via INFO so a
// reader sees the real number rather than only the threshold it cleared.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/blep.hpp>

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::osc::blamp_residual;
using pulp::signal::osc::blep_residual;
using pulp::signal::osc::Correction;
using pulp::signal::osc::poly_blamp;
using pulp::signal::osc::poly_blep;
using pulp::signal::osc::wrap_position;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;

// Fit length. The projection needs no power of two and no bin coherence; this is
// simply long enough that the proven floor (see the negative control) sits ~50 dB
// below the tightest gate. Doubling it moves no reading here by more than
// 0.01 dB.
constexpr int kFitLength = 8192;

// Band qualifier. Matches `AliasOptions`' default and every claim in this file.
constexpr double kBandHz = 20000.0;

// Improvement floor for the correction gates. See the file header: a tripwire
// below the measured minimum, not a certification.
constexpr double kMinImprovementDb = 8.0;

// f0 set spanning the band where aliasing bites. Each is deliberately not a
// rational fraction of the sample rate with a small denominator: an f0 that
// divides 48 kHz evenly (1000 Hz, say) folds a high harmonic exactly ONTO the
// fundamental, which is a degenerate fit rather than a measurement. Every case
// here asserts the fit resolved, so a bad choice fails loudly, not silently.
constexpr double kTestF0[] = {1103.0, 2153.0, 4100.0, 6301.0};

// Harmonics accounted for, as a multiple of the sample rate. The worst in-band
// alias always comes from the LOWEST harmonic above Nyquist (a saw's harmonic
// amplitude is 1/h, so higher ones are quieter), and the reading is identical to
// 0.01 dB anywhere from 1.5x to 6x coverage. 3x is past convergence; more only
// moves unattributed energy out of `noise_db`.
int harmonics_for(double f0) {
    return static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
}

enum class Mode {
    trivial,   ///< No correction: the naive discontinuity.
    corrected, ///< The kernel applied per its documented sign convention.
    inverted,  ///< The kernel applied with the sign flipped — the likeliest bug.
};

double mode_sign(Mode m) { return m == Mode::inverted ? -1.0 : 1.0; }

// A trivial saw, optionally polyBLEP-corrected.
//
// Corrections accumulate with `+=`, and the trivial value is added on top of
// whatever a previous iteration deposited, because the kernel's `after` term
// lands on a sample this loop has not generated yet. That two-sample reach is
// intrinsic to the kernel, not an artifact of this test: a generator must own a
// one-sample delay, a correction ring, or a second pass to apply it.
std::vector<double> render_saw(double f0, Mode mode, double start_phase = 0.25) {
    const double increment = f0 / kSampleRate;
    std::vector<double> out(static_cast<std::size_t>(kFitLength), 0.0);
    double phase = start_phase;
    for (int i = 0; i < kFitLength; ++i) {
        out[static_cast<std::size_t>(i)] += 2.0 * phase - 1.0;
        phase += increment;
        if (phase >= 1.0) {
            phase -= 1.0;
            if (mode == Mode::trivial) continue;
            // The saw wraps from +1 down to -1: after minus before = -2.
            const Correction c = poly_blep(wrap_position(phase, increment), -2.0);
            const double s = mode_sign(mode);
            out[static_cast<std::size_t>(i)] += s * c.before;
            if (i + 1 < kFitLength) out[static_cast<std::size_t>(i + 1)] += s * c.after;
        }
    }
    return out;
}

// A trivial triangle, optionally BLAMP-corrected.
//
// A triangle's VALUE is continuous everywhere — only its slope breaks, which is
// why this is the BLAMP case and not the BLEP one. It breaks twice per cycle: at
// the apex (phase 0.5) the slope goes from +4*increment to -4*increment, and at
// the trough (the wrap) it goes back. Slope is expressed per sample, which is
// where the increment enters — the kernel never sees it.
std::vector<double> render_triangle(double f0, Mode mode, double start_phase = 0.1) {
    const double increment = f0 / kSampleRate;
    const double s = mode_sign(mode);
    const auto tri = [](double p) { return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p); };
    std::vector<double> out(static_cast<std::size_t>(kFitLength), 0.0);
    double phase = start_phase;
    for (int i = 0; i < kFitLength; ++i) {
        out[static_cast<std::size_t>(i)] += tri(phase);
        const double previous = phase;
        phase += increment;

        const auto apply = [&](const Correction& c) {
            out[static_cast<std::size_t>(i)] += s * c.before;
            if (i + 1 < kFitLength) out[static_cast<std::size_t>(i + 1)] += s * c.after;
        };

        if (previous < 0.5 && phase >= 0.5 && mode != Mode::trivial) {
            const double d = 1.0 - (phase - 0.5) / increment;
            apply(poly_blamp(d, -8.0 * increment));
        }
        if (phase >= 1.0) {
            phase -= 1.0;
            if (mode == Mode::trivial) continue;
            apply(poly_blamp(wrap_position(phase, increment), +8.0 * increment));
        }
    }
    return out;
}

// An EXACTLY bandlimited saw: every harmonic below Nyquist summed at its own
// frequency in double precision. It has no wrap and no discontinuity, so it
// carries zero alias energy BY CONSTRUCTION rather than by assertion — which is
// what makes it usable as a negative control. Optionally injects one tone of
// known level at `alias_hz` to build a known-alias fixture.
std::vector<double> bandlimited_saw(double f0, double alias_hz = 0.0,
                                    double alias_amplitude = 0.0) {
    const int max_h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0));
    std::vector<double> out(static_cast<std::size_t>(kFitLength), 0.0);
    for (int i = 0; i < kFitLength; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double v = 0.0;
        for (int h = 1; h <= max_h; ++h) {
            const double sign = (h % 2 == 1) ? 1.0 : -1.0;
            v += sign * std::sin(2.0 * std::numbers::pi * h * f0 * t + 0.3) / h;
        }
        v *= 2.0 / std::numbers::pi; // Fundamental amplitude becomes 2/pi.
        if (alias_amplitude > 0.0)
            v += alias_amplitude * std::sin(2.0 * std::numbers::pi * alias_hz * t + 0.9);
        out[static_cast<std::size_t>(i)] = v;
    }
    return out;
}

// Fundamental amplitude of `bandlimited_saw`, so an injected tone can be placed
// an exact number of dB below it.
constexpr double kBandlimitedFundamental = 2.0 / std::numbers::pi;

AliasReport analyze(const std::vector<double>& signal, double f0) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int i = 0; i < static_cast<int>(signal.size()); ++i)
        buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);

    AliasOptions options;
    options.num_harmonics = harmonics_for(f0);
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

// The lowest harmonic above Nyquist whose fold site lands INSIDE the measurement
// band. Harmonics just above Nyquist fold to just below it — above the band
// qualifier, where they are correctly ignored — so a fixture that wants a
// gradeable in-band alias must search for a site rather than assume one.
int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1;
         h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

// A reading is only usable when the fit resolved every in-band site and the
// number sits above the measurement's own floor.
void require_trustworthy(const AliasReport& r) {
    REQUIRE_FALSE(r.has_unresolved_in_band_alias);
    REQUIRE(r.worst_alias_db > r.detection_floor_db);
}

} // namespace

// ── Kernel invariants: ground truth from the math, not from the code ───────

TEST_CASE("polyBLEP kernel matches its closed form at known points",
          "[signal][osc][blep]") {
    // Values below are evaluated by hand from the kernel's definition, NOT
    // captured from a run. B(x) = x^2/2 + x + 1/2 on [-1,0), x - x^2/2 - 1/2 on
    // [0,1); R(x) = integral of B, pinned to vanish at both ends.

    SECTION("BLEP vanishes at the edges of its support and steps by -1 at 0") {
        CHECK_THAT(blep_residual(-1.0), WithinAbs(0.0, 1e-15));
        CHECK_THAT(blep_residual(-0.5), WithinAbs(0.125, 1e-15));
        CHECK_THAT(blep_residual(0.0), WithinAbs(-0.5, 1e-15));
        CHECK_THAT(blep_residual(0.5), WithinAbs(-0.125, 1e-15));

        // x = 0 belongs to the after-branch, so the residual jumps from +1/2 to
        // -1/2 across the origin — a step of exactly -1, which is what cancels a
        // trivial +1 step.
        const double just_before = blep_residual(-1e-12);
        CHECK_THAT(just_before, WithinAbs(0.5, 1e-9));
        CHECK_THAT(just_before - blep_residual(0.0), WithinAbs(1.0, 1e-9));
    }

    SECTION("BLEP is compactly supported on [-1, 1)") {
        CHECK(blep_residual(-1.5) == 0.0);
        CHECK(blep_residual(1.0) == 0.0);
        CHECK(blep_residual(2.0) == 0.0);
        // Continuous into the upper edge rather than stepping to zero.
        CHECK_THAT(blep_residual(1.0 - 1e-9), WithinAbs(0.0, 1e-8));
    }

    SECTION("BLEP is odd") {
        // B(-x) = -B(x) falls out of the two branches; a sign slip in either one
        // breaks it.
        for (const double x : {0.1, 0.25, 0.5, 0.75, 0.9})
            CHECK_THAT(blep_residual(-x), WithinAbs(-blep_residual(x), 1e-15));
    }

    SECTION("a unit step plus its BLEP residual is a monotone S-curve") {
        // The whole point of the kernel, stated as a shape: the corrected step
        // rises smoothly through 1/2 at the discontinuity instead of jumping.
        const auto corrected = [](double x) {
            return (x >= 0.0 ? 1.0 : 0.0) + blep_residual(x);
        };
        CHECK_THAT(corrected(-1.0), WithinAbs(0.0, 1e-15));
        CHECK_THAT(corrected(-0.5), WithinAbs(0.125, 1e-15));
        CHECK_THAT(corrected(0.0), WithinAbs(0.5, 1e-15));
        CHECK_THAT(corrected(0.5), WithinAbs(0.875, 1e-15));
        CHECK_THAT(corrected(1.0 - 1e-9), WithinAbs(1.0, 1e-8));

        double previous = -1.0;
        for (int i = 0; i <= 200; ++i) {
            const double x = -1.0 + 2.0 * static_cast<double>(i) / 200.0 - 1e-9;
            const double v = corrected(x);
            CHECK(v >= previous - 1e-12);
            previous = v;
        }
    }

    SECTION("BLAMP vanishes at the edges, is continuous at 0, and is even") {
        CHECK_THAT(blamp_residual(-1.0), WithinAbs(0.0, 1e-15));
        CHECK_THAT(blamp_residual(0.0), WithinAbs(1.0 / 6.0, 1e-15));
        CHECK_THAT(blamp_residual(1.0 - 1e-9), WithinAbs(0.0, 1e-8));
        CHECK(blamp_residual(1.0) == 0.0);
        CHECK(blamp_residual(-1.5) == 0.0);

        // Unlike BLEP, BLAMP does not step at the origin — a slope break leaves
        // the value alone. Approaching from below must meet R(0) = 1/6.
        CHECK_THAT(blamp_residual(-1e-9), WithinAbs(1.0 / 6.0, 1e-8));

        // R(-x) = R(x): a slope break is symmetric in time.
        for (const double x : {0.1, 0.25, 0.5, 0.75, 0.9})
            CHECK_THAT(blamp_residual(-x), WithinAbs(blamp_residual(x), 1e-15));
        CHECK_THAT(blamp_residual(0.5), WithinAbs(0.0208333333333333, 1e-13));
    }
}

TEST_CASE("polyBLEP kernel handles edge and out-of-range positions",
          "[signal][osc][blep]") {
    SECTION("a sample landing exactly on a discontinuity lands on its midpoint") {
        // The physical statement of the boundary cases, and the one that matters:
        // a bandlimited step passes through the MIDPOINT of its own jump. Both
        // boundaries are ordinary operating points — a phase sitting at 0 and
        // driven backward produces d = 0, a phase wrapping to exactly 0 produces
        // d = 1 — and at each of them one tap coincides with the discontinuity.
        //
        // Which limit that tap needs depends on the value the sample already
        // carries, and the two boundaries differ: at d = 0 the `before` sample
        // was taken before the step (it needs +h/2 to reach the midpoint), while
        // at d = 1 the `after` sample was taken after it (it needs -h/2). A
        // kernel that reads both taps off one side gets d = 0 backwards by a
        // full `h`, which is worse than not correcting at all — and asserting on
        // the midpoint, rather than on a tap's raw number, is what states that
        // in terms a reader can check against the physics.
        for (const double h : {-2.0, +2.0, +1.0, -0.5}) {
            const Correction at_zero = poly_blep(0.0, h);
            INFO("h = " << h << ", d = 0: before tap " << at_zero.before << ", want " << (h / 2.0));
            CHECK_THAT(at_zero.before, WithinAbs(h / 2.0, 1e-15));
            CHECK_THAT(at_zero.after, WithinAbs(0.0, 1e-15));

            const Correction at_one = poly_blep(1.0, h);
            INFO("h = " << h << ", d = 1: after tap " << at_one.after << ", want " << (-h / 2.0));
            CHECK_THAT(at_one.before, WithinAbs(0.0, 1e-15));
            CHECK_THAT(at_one.after, WithinAbs(-h / 2.0, 1e-15));
        }

        // BLAMP has no such split: it is continuous at the origin, so both sides
        // agree and each boundary's coincident tap reads the same 1/6.
        for (const double s : {-1.6, +0.25}) {
            CHECK_THAT(poly_blamp(0.0, s).before, WithinAbs(s / 6.0, 1e-15));
            CHECK_THAT(poly_blamp(1.0, s).after, WithinAbs(s / 6.0, 1e-15));
        }
    }

    SECTION("the correction is continuous as d approaches each boundary") {
        // The boundary values are not a special case bolted onto the interior —
        // they are its limit. A tap that jumped as d crossed a boundary would
        // mean a sub-ULP change in a wrap's position could flip a correction by
        // the full step height.
        const double h = -2.0;
        CHECK_THAT(poly_blep(1e-12, h).before, WithinAbs(poly_blep(0.0, h).before, 1e-11));
        CHECK_THAT(poly_blep(1e-12, h).after, WithinAbs(poly_blep(0.0, h).after, 1e-11));
        CHECK_THAT(poly_blep(1.0 - 1e-12, h).before, WithinAbs(poly_blep(1.0, h).before, 1e-11));
        CHECK_THAT(poly_blep(1.0 - 1e-12, h).after, WithinAbs(poly_blep(1.0, h).after, 1e-11));

        for (const double d : {1e-12, 1e-6, 0.5, 1.0 - 1e-6, 1.0 - 1e-12}) {
            const Correction c = poly_blep(d, h);
            CHECK(std::isfinite(c.before));
            CHECK(std::isfinite(c.after));
            CHECK(std::abs(c.before) <= 1.0 + 1e-9);
            CHECK(std::abs(c.after) <= 1.0 + 1e-9);
        }
    }

    SECTION("a position outside [0,1] yields exactly no correction") {
        // Asserted as == 0 for every value, not merely `isfinite`. A discontinuity
        // always lands inside the sample it was found in, so an out-of-window
        // position is a caller error; the only answer that cannot make the output
        // worse than leaving it alone is none. Checking finiteness here would pass
        // on a spurious half-step, which is the failure this rules out.
        for (const double d : {-10.0, -1.0, -0.5, -1e-12, 1.0 + 1e-12, 1.5, 2.0, 5.0, 10.0}) {
            const Correction c = poly_blep(d, -2.0);
            INFO("d = " << d << " -> {" << c.before << ", " << c.after << "}");
            CHECK(c.before == 0.0);
            CHECK(c.after == 0.0);

            const Correction r = poly_blamp(d, -1.6);
            INFO("blamp d = " << d << " -> {" << r.before << ", " << r.after << "}");
            CHECK(r.before == 0.0);
            CHECK(r.after == 0.0);
        }

        // A non-finite position is a caller error too, and must not become one of
        // the caller's samples.
        const double inf = std::numeric_limits<double>::infinity();
        for (const double d : {std::nan(""), inf, -inf}) {
            CHECK(poly_blep(d, -2.0).before == 0.0);
            CHECK(poly_blep(d, -2.0).after == 0.0);
            CHECK(poly_blamp(d, -1.6).before == 0.0);
            CHECK(poly_blamp(d, -1.6).after == 0.0);
        }

        // The sentinel is guaranteed by that window check rather than by the
        // arithmetic happening to vanish, which is what makes it safe to rely on.
        CHECK(poly_blep(pulp::signal::osc::kNoDiscontinuity, -2.0).before == 0.0);
        CHECK(poly_blep(pulp::signal::osc::kNoDiscontinuity, -2.0).after == 0.0);
        CHECK(poly_blamp(pulp::signal::osc::kNoDiscontinuity, -1.6).before == 0.0);
        CHECK(poly_blamp(pulp::signal::osc::kNoDiscontinuity, -1.6).after == 0.0);
    }

    SECTION("total correction never exceeds half the step height") {
        // |B(-d)| + |B(1-d)| peaks at 1/2, at both ends of the range. A kernel
        // that overshoots this is injecting energy rather than removing it.
        for (int i = 0; i <= 1000; ++i) {
            const double d = static_cast<double>(i) / 1000.0;
            const Correction c = poly_blep(d, -2.0);
            CHECK(std::abs(c.before) + std::abs(c.after) <= 0.5 * 2.0 + 1e-12);
        }
    }

    SECTION("a non-advancing phase produces exactly no correction") {
        // The guard must yield ZERO correction, which is a stronger claim than
        // "returns some finite number". Returning 0 would satisfy the weaker
        // claim while injecting a half-step on every sample of a stopped
        // oscillator: d = 0 means "the discontinuity is on the `before` sample",
        // not "there is no discontinuity". Assert the correction, not the code.
        for (const double increment : {0.0, -1.0, -0.01}) {
            const Correction c = poly_blep(wrap_position(0.0, increment), -2.0);
            CHECK(c.before == 0.0);
            CHECK(c.after == 0.0);
            const Correction r = poly_blamp(wrap_position(0.0, increment), -0.25);
            CHECK(r.before == 0.0);
            CHECK(r.after == 0.0);
        }
        // A frozen phase divides 0 by 0; NaN survives both kernels' range checks
        // and would reach the output buffer, so the guard is load-bearing.
        CHECK(std::isfinite(wrap_position(0.0, 0.0)));
        CHECK(std::isfinite(poly_blep(wrap_position(0.0, 0.0), -2.0).before));
    }

    SECTION("a wrap that did happen is placed where it belongs") {
        // A wrap landing exactly on the boundary sits at d = 1.
        CHECK_THAT(wrap_position(0.0, 0.01), WithinAbs(1.0, 1e-15));
        // Half an increment past the boundary sits halfway between samples.
        CHECK_THAT(wrap_position(0.005, 0.01), WithinAbs(0.5, 1e-15));
        // A wrap a hair past the boundary sits just shy of the `after` sample.
        CHECK_THAT(wrap_position(0.0099, 0.01), WithinAbs(0.01, 1e-12));
    }
}

TEST_CASE("polyBLEP kernels are deterministic", "[signal][osc][blep]") {
    SECTION("identical input yields bit-identical output") {
        for (int i = 0; i <= 100; ++i) {
            const double d = static_cast<double>(i) / 100.0;
            const Correction a = poly_blep(d, -2.0);
            const Correction b = poly_blep(d, -2.0);
            REQUIRE(a.before == b.before);
            REQUIRE(a.after == b.after);
            const Correction ra = poly_blamp(d, -0.25);
            const Correction rb = poly_blamp(d, -0.25);
            REQUIRE(ra.before == rb.before);
            REQUIRE(ra.after == rb.after);
        }
    }

    SECTION("a whole rendered waveform reproduces bit-exactly") {
        const auto first = render_saw(4100.0, Mode::corrected);
        const auto second = render_saw(4100.0, Mode::corrected);
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
    }
}

// ── The measurement itself, before anything is measured with it ────────────

TEST_CASE("Alias measurement separates a clean signal from a known-bad one",
          "[signal][osc][alias]") {
    // ── THE NEGATIVE CONTROL. Nothing below this line is trustworthy without
    // it: a gate read through a measurement that cannot see failure passes
    // silently, and asserting the analyzer's own derived `detection_floor_db`
    // would only be the analyzer grading its own homework. That derived bound
    // assumes a WHITE residual, and aliases are discrete tones — the assumption
    // fails exactly where the analyzer earns its keep. So the floor is proven
    // here by construction instead: measure a signal whose alias energy is zero
    // by construction and show the reading collapses; then inject aliases of
    // known level and show they are recovered.
    for (const double f0 : kTestF0) {
        const int h = first_in_band_alias_harmonic(f0);
        REQUIRE(h > 0);
        const double site_hz = fold_frequency(h * f0, kSampleRate);
        REQUIRE(site_hz <= kBandHz);

        SECTION("alias removed: the reading collapses") {
            const auto report = analyze(bandlimited_saw(f0), f0);
            INFO("f0 " << f0
                       << ": exactly bandlimited saw (no alias by construction) reads "
                       << report.worst_alias_db << " dBc, noise " << report.noise_db);
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            // Reads -168 to -182 dBc in practice. Gating at -140 keeps the proof
            // honest under platform arithmetic while staying ~90 dB below
            // anything this file gates on.
            CHECK(report.worst_alias_db < -140.0);
        }

        SECTION("alias injected at a known level: it is recovered") {
            // Ground truth is set by construction — a tone of amplitude
            // 10^(dB/20) * fundamental placed exactly on a fold site — never by
            // running the analyzer and keeping its answer.
            for (const double truth_db : {-60.0, -80.0, -100.0, -120.0}) {
                const double amplitude =
                    std::pow(10.0, truth_db / 20.0) * kBandlimitedFundamental;
                const auto report = analyze(bandlimited_saw(f0, site_hz, amplitude), f0);
                INFO("f0 " << f0 << ": injected " << truth_db << " dBc at " << site_hz
                           << " Hz (h=" << h << "); read " << report.worst_alias_db
                           << " dBc at " << report.worst_alias_hz
                           << " Hz (h=" << report.worst_alias_index << ")");
                REQUIRE_FALSE(report.has_unresolved_in_band_alias);
                CHECK(report.worst_alias_index == h);
                CHECK_THAT(report.worst_alias_hz, WithinAbs(site_hz, 1.0));
                // Recovered to within 0.01 dB in practice, down to -120 dBc.
                CHECK_THAT(report.worst_alias_db, WithinAbs(truth_db, 1.0));
            }
        }
    }
}

// ── The kernels, measured ─────────────────────────────────────────────────

TEST_CASE("polyBLEP improves saw alias rejection below 20 kHz",
          "[signal][osc][alias]") {
    for (const double f0 : kTestF0) {
        const auto trivial = analyze(render_saw(f0, Mode::trivial), f0);
        const auto corrected = analyze(render_saw(f0, Mode::corrected), f0);
        const double improvement = trivial.worst_alias_db - corrected.worst_alias_db;

        INFO("f0 " << f0 << " Hz: worst in-band alias (<= 20 kHz) "
                   << trivial.worst_alias_db << " dBc trivial -> "
                   << corrected.worst_alias_db << " dBc corrected = " << improvement
                   << " dB improvement (at " << corrected.worst_alias_hz
                   << " Hz, h=" << corrected.worst_alias_index << "); full-band "
                   << corrected.full_band_worst_alias_db
                   << " dBc; proven floor <= -120 dBc, analyzer-derived floor "
                   << corrected.detection_floor_db << " dBc");

        require_trustworthy(corrected);
        REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
        CHECK(improvement >= kMinImprovementDb);
        // The reading sits ~70 dB above the floor proven by the negative
        // control, so it is the kernel being measured, not the analyzer.
        CHECK(corrected.worst_alias_db > -120.0);
    }
}

TEST_CASE("polyBLEP sign convention: inverting the correction makes aliasing worse",
          "[signal][osc][alias]") {
    // The cheapest guard against the most expensive bug. A sign error does not
    // fail loudly — it DOUBLES the discontinuity instead of removing it, so the
    // oscillator still produces plausible audio while aliasing harder than the
    // trivial waveform it was supposed to improve on. Only a measurement catches
    // it, and only if the measurement is pointed at the inverted case on purpose.
    for (const double f0 : kTestF0) {
        const auto trivial = analyze(render_saw(f0, Mode::trivial), f0);
        const auto corrected = analyze(render_saw(f0, Mode::corrected), f0);
        const auto inverted = analyze(render_saw(f0, Mode::inverted), f0);

        INFO("f0 " << f0 << " Hz: worst in-band alias trivial " << trivial.worst_alias_db
                   << " dBc, correct sign " << corrected.worst_alias_db
                   << " dBc, INVERTED sign " << inverted.worst_alias_db << " dBc");

        REQUIRE_FALSE(inverted.has_unresolved_in_band_alias);

        // Inverting is worse than not correcting at all: ~4.6-5.2 dB measured.
        CHECK(inverted.worst_alias_db > trivial.worst_alias_db + 2.0);
        // And ~16-19 dB worse than the correct sign.
        CHECK(inverted.worst_alias_db > corrected.worst_alias_db + 10.0);
    }
}

TEST_CASE("BLAMP improves triangle alias rejection below 20 kHz",
          "[signal][osc][alias]") {
    // A triangle's value never jumps; only its slope breaks, at the apex and the
    // trough. That is the BLAMP case — applying BLEP here would correct a step
    // that does not exist.
    for (const double f0 : {1103.0, 2153.0, 4100.0}) {
        const auto trivial = analyze(render_triangle(f0, Mode::trivial), f0);
        const auto corrected = analyze(render_triangle(f0, Mode::corrected), f0);
        const auto inverted = analyze(render_triangle(f0, Mode::inverted), f0);
        const double improvement = trivial.worst_alias_db - corrected.worst_alias_db;

        INFO("f0 " << f0 << " Hz: triangle worst in-band alias (<= 20 kHz) "
                   << trivial.worst_alias_db << " dBc trivial -> "
                   << corrected.worst_alias_db << " dBc BLAMP-corrected = " << improvement
                   << " dB improvement; inverted sign " << inverted.worst_alias_db
                   << " dBc; analyzer-derived floor " << corrected.detection_floor_db
                   << " dBc");

        require_trustworthy(corrected);
        REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
        CHECK(improvement >= kMinImprovementDb);
        // The same sign guard as the step kernel: inverting must hurt.
        CHECK(inverted.worst_alias_db > trivial.worst_alias_db + 2.0);
        CHECK(corrected.worst_alias_db > -120.0);
    }
}

TEST_CASE("Band qualification is load-bearing, not decorative",
          "[signal][osc][alias]") {
    // Why every claim in this file says "below 20 kHz". Harmonics just above
    // Nyquist fold to just below it, where no correction can reach them: the
    // kernel would have to reject at Nyquist and pass at Nyquist. Showing the
    // unqualified number is MEASURABLY worse than the in-band one is what makes
    // the qualifier a physical statement rather than a convenient omission — a
    // full-band gate would be measuring the sample rate, not the kernel.
    for (const double f0 : kTestF0) {
        const auto corrected = analyze(render_saw(f0, Mode::corrected), f0);
        INFO("f0 " << f0 << " Hz: worst alias below 20 kHz " << corrected.worst_alias_db
                   << " dBc, but full-band " << corrected.full_band_worst_alias_db
                   << " dBc at " << corrected.full_band_worst_alias_hz << " Hz");
        CHECK(corrected.full_band_worst_alias_db > corrected.worst_alias_db);
        // The unqualified worst alias lands above the band — exactly the content
        // the qualifier exists to exclude.
        CHECK(corrected.full_band_worst_alias_hz > kBandHz);
    }
}
