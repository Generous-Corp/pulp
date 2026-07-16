// Virtual-analog static shapes: measured alias rejection, level, DC, and the
// boundary cases where a correction stage either misfires or fails to fire.
//
// ── Measurement discipline ────────────────────────────────────────────────
//
// Alias numbers come from `measure_aliasing` — a joint least-squares fit of the
// ideal harmonic series and every above-Nyquist harmonic's fold site. It is
// projection, not windowed-FFT bin classification, so a 0 dB fundamental has no
// leakage skirt to bury a small alias under. Three rules, each with a failure
// mode attached to skipping it:
//
//   1. **Band-qualified to 20 kHz.** A full-band alias claim is impossible for
//      any method: fold-back is continuous across Nyquist, so a component at
//      Nyquist + eps lands at Nyquist - eps where no correction can reach it.
//   2. **The floor is PROVEN by negative control, never derived.** The
//      analyzer's `detection_floor_db` is a ~2 sigma bound assuming a WHITE
//      residual — false here, since aliases are discrete tones. Asserting it
//      would be the analyzer grading its own homework. Instead a signal with
//      zero alias content by construction must read as collapsed, and injected
//      aliases of known level must be recovered.
//   3. **A gate sits above the floor, and the fit must have resolved.**
//
// ── What these shapes are, and are not ────────────────────────────────────
//
// polyBLEP is a two-point approximation, so saw/square/triangle are IMPROVED,
// not alias-free: 11.6-15.6 dB better than the trivial shape, measured. That is
// what the kernels buy. The improvement gates are set at 8 dB — below the
// measured minimum (11.58 dB) with margin for platform arithmetic, and far above
// the two physically meaningful landmarks: 0 dB (the correction did nothing) and
// negative (it did harm). They are regression tripwires, not certifications of
// polyBLEP's quality. The real figure per shape is logged via INFO.
//
// Sine is exact and is the strongest null available: it has no discontinuity, so
// a correction stage that fires on it at all is broken. That is asserted as
// bit-exactness against a plain `std::sin`, not as a tolerance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/osc/va.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::osc::PhaseAccumulator;
using pulp::signal::osc::threshold_crossings;
using pulp::signal::osc::VaOscillator;
using pulp::signal::osc::VaShape;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kRenderLength = 8192;
constexpr double kBandHz = 20000.0;
constexpr double kMinImprovementDb = 8.0;

// f0 set spanning the band where aliasing bites. None is a small-denominator
// fraction of the sample rate: an f0 dividing 48 kHz evenly folds a high
// harmonic exactly onto the fundamental, which is a degenerate fit rather than a
// measurement. Every case asserts the fit resolved, so a bad choice fails loudly.
constexpr double kTestF0[] = {1103.0, 2153.0, 4100.0, 6301.0};

// A start phase that sits on no discontinuity of any shape — not 0 (the wrap),
// not 0.5 (the triangle apex), not any pulse width under test. Starting ON a
// discontinuity is a real and separate case, covered by its own test.
constexpr double kCleanStartPhase = 0.13;

// Harmonics accounted for, as a multiple of the sample rate. The worst in-band
// alias comes from the lowest harmonic above Nyquist, and the reading is
// unchanged from 1.5x to 6x coverage; 3x is past convergence.
int harmonics_for(double f0) {
    return static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
}

const char* shape_name(VaShape s) {
    switch (s) {
        case VaShape::sine: return "sine";
        case VaShape::saw: return "saw";
        case VaShape::square: return "square";
        case VaShape::triangle: return "triangle";
    }
    return "unknown";
}

// The trivial shape: the same waveform with no correction whatsoever. This is
// the baseline the improvement is measured against, written out independently so
// it cannot inherit a bug from the oscillator it is grading.
double trivial_value(VaShape shape, double p, double pulse_width) {
    switch (shape) {
        case VaShape::sine: return std::sin(2.0 * std::numbers::pi * p);
        case VaShape::saw: return 2.0 * p - 1.0;
        case VaShape::square: return p < pulse_width ? 1.0 : -1.0;
        case VaShape::triangle: return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p);
    }
    return 0.0;
}

std::vector<double> render_trivial(VaShape shape, double f0, double pulse_width = 0.5,
                                   double start_phase = kCleanStartPhase,
                                   int length = kRenderLength) {
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    double p = start_phase;
    for (int i = 0; i < length; ++i) {
        out[static_cast<std::size_t>(i)] = trivial_value(shape, p, pulse_width);
        p += increment;
        p -= std::floor(p);
    }
    return out;
}

std::vector<double> render_va(VaShape shape, double f0, double pulse_width = 0.5,
                              double start_phase = kCleanStartPhase,
                              int length = kRenderLength) {
    VaOscillator osc;
    osc.set_shape(shape);
    osc.set_pulse_width(pulse_width);
    osc.reset(start_phase);
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = osc.next(increment);
    return out;
}

// An EXACTLY bandlimited saw: every harmonic below Nyquist summed at its own
// frequency in double precision. No wrap, no discontinuity, so it carries zero
// alias energy BY CONSTRUCTION rather than by assertion — which is what makes it
// usable as a negative control. Optionally injects a tone of known level on a
// fold site to build a known-alias fixture.
std::vector<double> bandlimited_saw(double f0, double alias_hz = 0.0,
                                    double alias_amplitude = 0.0) {
    const int max_h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0));
    std::vector<double> out(static_cast<std::size_t>(kRenderLength), 0.0);
    for (int i = 0; i < kRenderLength; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double v = 0.0;
        for (int h = 1; h <= max_h; ++h) {
            const double sign = (h % 2 == 1) ? 1.0 : -1.0;
            v += sign * std::sin(2.0 * std::numbers::pi * h * f0 * t + 0.3) / h;
        }
        v *= 2.0 / std::numbers::pi;
        if (alias_amplitude > 0.0)
            v += alias_amplitude * std::sin(2.0 * std::numbers::pi * alias_hz * t + 0.9);
        out[static_cast<std::size_t>(i)] = v;
    }
    return out;
}

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

// The lowest harmonic above Nyquist whose fold site lands inside the band.
// Harmonics just above Nyquist fold to just below it — outside the qualifier,
// where they are correctly ignored — so a gradeable fixture must search.
int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1;
         h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

double rms(const std::vector<double>& v) {
    double sum = 0.0;
    for (const double x : v) sum += x * x;
    return std::sqrt(sum / static_cast<double>(v.size()));
}

double mean(const std::vector<double>& v) {
    double sum = 0.0;
    for (const double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}

double peak(const std::vector<double>& v) {
    double m = 0.0;
    for (const double x : v) m = std::fmax(m, std::fabs(x));
    return m;
}

// An f0 placing a whole number of cycles in the render, so a mean over the
// buffer equals the waveform's true mean. Without this the leftover partial
// cycle IS the reading — a fractional cycle of a saw contributes ~1e-3 of
// apparent DC and would be mistaken for an offset.
double coherent_f0(int cycles, int length = kRenderLength) {
    return static_cast<double>(cycles) * kSampleRate / static_cast<double>(length);
}

// Width, in samples, of the narrower of a square's two regions.
//
// A pulse of width `w` leaves a high region `w` of a period long and a low
// region `1 - w` long, and it is the SHORTER of the two that decides whether the
// shape is resolvable — a 90% duty pulse is exactly as hard as a 10% one, from
// the other side.
double narrowest_region_samples(double width, double f0) {
    return std::fmin(width, 1.0 - width) * kSampleRate / f0;
}

// How many samples a region must span before polyBLEP's corrections for its two
// edges stop overlapping. Each kernel reaches one sample either side of its
// discontinuity, so two edges closer than ~2 samples are correcting each other's
// territory; 3 leaves a margin. Below this the shape is bounded but not exact,
// which is the kernel's limit and not a defect to gate against.
constexpr double kResolvableRegionSamples = 3.0;

void require_trustworthy(const AliasReport& r) {
    REQUIRE_FALSE(r.has_unresolved_in_band_alias);
    REQUIRE(r.worst_alias_db > r.detection_floor_db);
}

} // namespace

// ── The crossing finder, checked against the accumulator that defines it ───

TEST_CASE("threshold_crossings reproduces the accumulator's wrap positions",
          "[signal][osc][phase]") {
    // The apex and the pulse edge are not wraps, so the accumulator does not
    // report them and this file locates them itself. That is only trustworthy if
    // the finder agrees with the accumulator where they overlap: at threshold 0
    // a crossing IS a wrap, so the two must produce the same count and the same
    // sub-sample positions. Any disagreement means the apex lands somewhere the
    // wrap would not — the exact error a shared derivation exists to prevent.
    const double increments[] = {0.02083, 0.1, 0.37, 0.9, 1.7, 2.5, -0.1, -0.37, -1.7};
    const double starts[] = {0.0, 0.05, 0.1, 0.5, 0.9, 0.999};

    int compared = 0;
    for (const double increment : increments) {
        for (const double start : starts) {
            PhaseAccumulator accumulator;
            accumulator.reset(start);
            accumulator.advance(increment);

            double fracs[PhaseAccumulator::max_events_per_sample];
            const int n = threshold_crossings(start, increment, 0.0, std::span<double>(fracs));

            INFO("increment " << increment << ", start phase " << start);
            REQUIRE(n == static_cast<int>(accumulator.events().size()));
            for (int k = 0; k < n; ++k) {
                // Bit-exact, not merely close: both compute (level - p0)/delta.
                CHECK(fracs[k] == accumulator.events()[static_cast<std::size_t>(k)].frac);
                ++compared;
            }
        }
    }
    // Guard against the comparison loop silently never running.
    CHECK(compared > 0);
}

TEST_CASE("threshold_crossings locates a threshold where the algebra says",
          "[signal][osc][phase]") {
    // Ground truth by hand: the phase is linear in sub-sample time, so it
    // reaches level `m + threshold` at t = (m + threshold - p0) / increment.
    double f[PhaseAccumulator::max_events_per_sample];

    SECTION("forward, one crossing") {
        // 0.1 -> 0.3 crosses 0.25 at (0.25 - 0.1) / 0.2 = 0.75.
        REQUIRE(threshold_crossings(0.1, 0.2, 0.25, std::span<double>(f)) == 1);
        CHECK_THAT(f[0], WithinAbs(0.75, 1e-12));
        // 0.4 -> 0.6 crosses the triangle apex at (0.5 - 0.4) / 0.2 = 0.5.
        REQUIRE(threshold_crossings(0.4, 0.2, 0.5, std::span<double>(f)) == 1);
        CHECK_THAT(f[0], WithinAbs(0.5, 1e-12));
    }

    SECTION("backward, one crossing") {
        // 0.6 -> 0.4 crosses 0.5 at (0.5 - 0.6) / -0.2 = 0.5.
        REQUIRE(threshold_crossings(0.6, -0.2, 0.5, std::span<double>(f)) == 1);
        CHECK_THAT(f[0], WithinAbs(0.5, 1e-12));
    }

    SECTION("an increment past 1 crosses the same threshold more than once") {
        // 0.1 -> 2.1 passes 0.5 and 1.5, i.e. the apex twice, at 0.2 and 0.7.
        REQUIRE(threshold_crossings(0.1, 2.0, 0.5, std::span<double>(f)) == 2);
        CHECK_THAT(f[0], WithinAbs(0.2, 1e-12));
        CHECK_THAT(f[1], WithinAbs(0.7, 1e-12));
    }

    SECTION("no crossing when the threshold is not passed") {
        CHECK(threshold_crossings(0.1, 0.05, 0.5, std::span<double>(f)) == 0);
        CHECK(threshold_crossings(0.6, 0.05, 0.5, std::span<double>(f)) == 0);
    }

    SECTION("a phase that is not advancing crosses nothing") {
        CHECK(threshold_crossings(0.5, 0.0, 0.5, std::span<double>(f)) == 0);
        CHECK(threshold_crossings(0.5, std::nan(""), 0.5, std::span<double>(f)) == 0);
        CHECK(threshold_crossings(0.5, INFINITY, 0.5, std::span<double>(f)) == 0);
    }
}

// ── Sine: the null ────────────────────────────────────────────────────────

TEST_CASE("sine is exact and the correction stage never fires on it",
          "[signal][osc][alias]") {
    // Sine has no discontinuity, so any correction applied to it is spurious by
    // definition. Asserting bit-exactness against a plain std::sin is the
    // strongest available form of that claim: a tolerance would let a small
    // wrong correction through, and the correct answer here is not "small" but
    // "none". This is what catches a corrector that fires on every wrap
    // regardless of whether the shape actually steps there.
    for (const double f0 : kTestF0) {
        const auto produced = render_va(VaShape::sine, f0);
        const auto reference = render_trivial(VaShape::sine, f0);
        REQUIRE(produced.size() == reference.size());

        int differing = 0;
        for (std::size_t i = 0; i < produced.size(); ++i)
            if (produced[i] != reference[i]) ++differing;

        INFO("f0 " << f0 << " Hz: " << differing << " of " << produced.size()
                   << " samples differ from a plain sin(2*pi*phase)");
        CHECK(differing == 0);
    }
}

// ── The measurement, before anything is measured with it ──────────────────

TEST_CASE("alias measurement separates a clean signal from a known-bad one",
          "[signal][osc][alias]") {
    // ── THE NEGATIVE CONTROL. Every alias gate below is read through this
    // measurement, and a gate whose measurement cannot see failure passes
    // silently. The analyzer's own derived floor cannot establish this: it
    // assumes a white residual, and aliases are discrete tones, so it can read
    // optimistically exactly where the analyzer is blind. Proof by construction
    // instead — a signal with no alias must collapse, and injected aliases of
    // known level must come back at that level.
    for (const double f0 : kTestF0) {
        const int h = first_in_band_alias_harmonic(f0);
        REQUIRE(h > 0);
        const double site_hz = fold_frequency(h * f0, kSampleRate);
        REQUIRE(site_hz <= kBandHz);

        SECTION("alias removed: the reading collapses") {
            const auto report = analyze(bandlimited_saw(f0), f0);
            INFO("f0 " << f0 << ": bandlimited saw with no alias by construction reads "
                       << report.worst_alias_db << " dBc (noise " << report.noise_db << ")");
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            // Reads -168 to -182 dBc in practice; -140 keeps the proof honest
            // under platform arithmetic while staying ~90 dB below every gate.
            CHECK(report.worst_alias_db < -140.0);
        }

        SECTION("alias injected at a known level: it is recovered") {
            // Ground truth is constructed, never taken from the analyzer's own
            // output.
            for (const double truth_db : {-60.0, -100.0, -120.0}) {
                const double amplitude =
                    std::pow(10.0, truth_db / 20.0) * kBandlimitedFundamental;
                const auto report = analyze(bandlimited_saw(f0, site_hz, amplitude), f0);
                INFO("f0 " << f0 << ": injected " << truth_db << " dBc at " << site_hz
                           << " Hz (h=" << h << "); read " << report.worst_alias_db << " dBc");
                REQUIRE_FALSE(report.has_unresolved_in_band_alias);
                CHECK(report.worst_alias_index == h);
                CHECK_THAT(report.worst_alias_db, WithinAbs(truth_db, 1.0));
            }
        }
    }
}

// ── The shapes, measured ──────────────────────────────────────────────────

TEST_CASE("bandlimited shapes improve alias rejection below 20 kHz",
          "[signal][osc][alias]") {
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
        for (const double f0 : kTestF0) {
            const auto trivial = analyze(render_trivial(shape, f0), f0);
            const auto corrected = analyze(render_va(shape, f0), f0);
            const double improvement = trivial.worst_alias_db - corrected.worst_alias_db;

            INFO(shape_name(shape)
                 << " at f0 " << f0 << " Hz: worst in-band alias (<= 20 kHz) "
                 << trivial.worst_alias_db << " dBc trivial -> " << corrected.worst_alias_db
                 << " dBc corrected = " << improvement << " dB improvement (at "
                 << corrected.worst_alias_hz << " Hz, h=" << corrected.worst_alias_index
                 << "); full-band " << corrected.full_band_worst_alias_db
                 << " dBc; proven floor <= -120 dBc");

            require_trustworthy(corrected);
            REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
            CHECK(improvement >= kMinImprovementDb);
            // The reading sits ~50 dB above the floor proven above, so it is the
            // oscillator being measured and not the analyzer.
            CHECK(corrected.worst_alias_db > -120.0);
        }
    }
}

TEST_CASE("band qualification is load-bearing, not decorative",
          "[signal][osc][alias]") {
    // Why every claim here says "below 20 kHz". Harmonics just above Nyquist
    // fold to just below it, where no correction can reach: the kernel would
    // have to reject at Nyquist and pass at Nyquist. The unqualified number
    // being measurably worse is what makes the qualifier physics rather than a
    // convenient omission.
    for (const double f0 : kTestF0) {
        const auto corrected = analyze(render_va(VaShape::saw, f0), f0);
        INFO("f0 " << f0 << ": worst alias below 20 kHz " << corrected.worst_alias_db
                   << " dBc, full-band " << corrected.full_band_worst_alias_db << " dBc at "
                   << corrected.full_band_worst_alias_hz << " Hz");
        CHECK(corrected.full_band_worst_alias_db > corrected.worst_alias_db);
        CHECK(corrected.full_band_worst_alias_hz > kBandHz);
    }
}

// ── Level, DC, and the degenerate corners ─────────────────────────────────

TEST_CASE("triangle level does not depend on pitch", "[signal][osc][va]") {
    // A regression test against the shape this replaces. Deriving a triangle by
    // running a square through a leaky integrator makes its level a function of
    // the cutoff-to-fundamental ratio, so the triangle goes quiet as it climbs.
    // Deriving it from the phase removes that coupling by construction, and this
    // is the assertion that keeps it removed.
    //
    // Flat is not the same as identical: BLAMP rounds the apex, and the rounding
    // is a larger fraction of a shorter period, so the level falls slightly with
    // pitch. That is correct — a bandlimited triangle's apex IS rounded — and it
    // is a 0.22 dB effect over six octaves, not the many-dB slide an integrator
    // produces.
    constexpr double kIdealRms = 0.57735026918962576; // 1/sqrt(3), a triangle's RMS.
    double lowest = 1e9;
    double highest = -1e9;

    for (const double f0 : {55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0}) {
        const double level = rms(render_va(VaShape::triangle, f0));
        lowest = std::fmin(lowest, level);
        highest = std::fmax(highest, level);
        INFO("f0 " << f0 << " Hz: RMS " << level << " ("
                   << 20.0 * std::log10(level / kIdealRms) << " dB re 1/sqrt(3))");
        // Each level is within a quarter dB of an ideal triangle's.
        CHECK_THAT(20.0 * std::log10(level / kIdealRms), WithinAbs(0.0, 0.25));
    }

    const double spread_db = 20.0 * std::log10(highest / lowest);
    INFO("RMS spread across 55 Hz .. 3520 Hz (six octaves) = " << spread_db << " dB");
    // Measures 0.22 dB. Gating at 0.5 dB is a tripwire on the coupling being
    // reintroduced, not a certification of the apex rounding's size.
    CHECK(spread_db < 0.5);
}

TEST_CASE("DC is what the shape's geometry says it is", "[signal][osc][va]") {
    // Measured over a whole number of cycles, because a fractional cycle's
    // leftover is itself a DC-looking reading — a partial saw cycle shows ~1e-3
    // of apparent offset that has nothing to do with the oscillator.
    SECTION("shapes symmetric about zero have none") {
        for (const int cycles : {188, 367, 700}) {
            const double f0 = coherent_f0(cycles);
            for (const VaShape shape : {VaShape::saw, VaShape::triangle, VaShape::sine}) {
                const double dc = mean(render_va(shape, f0));
                INFO(shape_name(shape) << " at f0 " << f0 << " (" << cycles
                                       << " whole cycles): mean " << dc);
                // Measures ~1e-18. The bound is the accumulated rounding of
                // summing 8192 doubles, not a tolerance for real offset.
                CHECK_THAT(dc, WithinAbs(0.0, 1e-9));
            }
        }
    }

    SECTION("a square's DC follows its pulse width") {
        // Not a defect to correct: a pulse high for `w` of its period and low for
        // the rest has a mean of 2w - 1 by construction. A square that measured
        // zero DC at w = 0.1 would be the broken one.
        //
        // Asserted only where both of the square's regions are wider than the
        // kernel's reach — see `narrowest_region_samples`. Demanding exactness
        // outside that domain would be demanding something polyBLEP cannot give,
        // and a gate that no correct implementation can pass is not a gate.
        for (const int cycles : {94, 188}) {
            const double f0 = coherent_f0(cycles);
            for (const double width : {0.1, 0.25, 0.5, 0.75, 0.9}) {
                const double resolvable = narrowest_region_samples(width, f0);
                INFO("square, pulse width " << width << " at f0 " << f0 << ": narrowest region "
                                            << resolvable << " samples");
                // The test states its own validity domain rather than assuming it.
                REQUIRE(resolvable >= kResolvableRegionSamples);

                const double dc = mean(render_va(VaShape::square, f0, width));
                INFO("mean " << dc << ", geometry says " << (2.0 * width - 1.0));
                CHECK_THAT(dc, WithinAbs(2.0 * width - 1.0, 1e-9));
            }
        }
    }

    SECTION("a pulse narrower than the kernel's reach loses DC accuracy, boundedly") {
        // The honest other half. Each polyBLEP edge spans the sample either side
        // of it, so once a square's two edges sit closer than about two samples
        // their corrections overlap and neither is the correction for an isolated
        // step any more. DC drifts off the geometry as a result.
        //
        // This is characterization, not a target: the claim is that the error
        // stays small and bounded as the pulse collapses, NOT that it vanishes.
        // Asserting it vanishes would be asserting polyBLEP is something it is
        // not, and this section exists so that limit is written down in a form
        // that fails if it ever gets worse.
        double worst = 0.0;
        for (const int cycles : {367, 550, 700, 1000}) {
            const double f0 = coherent_f0(cycles);
            const double width = 0.1;
            const double resolvable = narrowest_region_samples(width, f0);
            if (resolvable >= kResolvableRegionSamples) continue;

            const double dc = mean(render_va(VaShape::square, f0, width));
            const double error = std::fabs(dc - (2.0 * width - 1.0));
            worst = std::fmax(worst, error);
            INFO("pulse width " << width << " at f0 " << f0 << " spans " << resolvable
                                << " samples: DC " << dc << " vs geometry "
                                << (2.0 * width - 1.0) << ", error " << error);
            CHECK(error < 1e-3);
        }
        INFO("worst DC error across the unresolvable pulses = " << worst);
        // Measures ~5e-5. It must be small, and it must not be zero — a zero here
        // would mean the sweep never reached the regime it claims to cover.
        CHECK(worst > 0.0);
        CHECK(worst < 1e-3);
    }
}

TEST_CASE("starting exactly on a discontinuity is a transient, not an offset",
          "[signal][osc][va]") {
    // Resetting the phase onto an edge puts a discontinuity at sample 0, whose
    // `before` tap would land at sample -1 — a sample that does not exist,
    // because the signal starts here. So sample 0 takes the trivial value rather
    // than the step's midpoint, and the render carries exactly one sample of
    // error.
    //
    // "Exactly one sample, ever" and "a DC offset" are different claims and are
    // distinguishable: a fixed one-sample error contributes 1/N to the mean and
    // vanishes as the render lengthens, while an offset does not. Asserting the
    // 1/N is what makes this a finding rather than an excuse.
    const double width = 0.25;
    double previous_scaled = 0.0;

    for (const int length : {2048, 4096, 8192, 16384}) {
        const int cycles = static_cast<int>(std::round(length * 1101.5625 / kSampleRate));
        const double f0 = coherent_f0(cycles, length);
        const double dc = mean(render_va(VaShape::square, f0, width, width, length));
        const double error = dc - (2.0 * width - 1.0);
        const double scaled = error * static_cast<double>(length);

        INFO("start phase on the pulse edge, " << length << " samples: mean error " << error
                                               << ", error * N = " << scaled);
        // One sample's worth of error, whatever N is: the trivial value is 1
        // away from the midpoint the corrected sample would have taken.
        CHECK_THAT(scaled, WithinAbs(-1.0, 1e-9));
        if (previous_scaled != 0.0) CHECK_THAT(scaled, WithinAbs(previous_scaled, 1e-9));
        previous_scaled = scaled;
    }

    SECTION("a start phase off every discontinuity has no such error") {
        for (const int length : {2048, 8192, 16384}) {
            const int cycles = static_cast<int>(std::round(length * 1101.5625 / kSampleRate));
            const double dc =
                mean(render_va(VaShape::square, coherent_f0(cycles, length), width,
                               kCleanStartPhase, length));
            INFO(length << " samples from a clean start phase: mean " << dc);
            CHECK_THAT(dc, WithinAbs(2.0 * width - 1.0, 1e-9));
        }
    }
}

TEST_CASE("pulse width stays bounded at and near its degenerate ends",
          "[signal][osc][va]") {
    // A width of exactly 0 or 1 leaves the square with no edge at all: it is DC,
    // which is the shape's honest limit. Getting there requires the trivial shape
    // to agree with its own limit at the top of the half-open period — a width of
    // 1 is high everywhere INCLUDING the limit at 1, and a corrector that reads
    // that limit as low manufactures a full-scale step at a wrap with no edge on
    // either side of it, doubling the output.
    SECTION("a degenerate width is DC, at the right level") {
        for (const double f0 : {1103.0, 6301.0}) {
            for (const double width : {0.0, 1.0}) {
                const auto rendered = render_va(VaShape::square, f0, width);
                INFO("pulse width " << width << " at f0 " << f0 << ": peak " << peak(rendered)
                                    << ", mean " << mean(rendered));
                CHECK_THAT(peak(rendered), WithinAbs(1.0, 1e-9));
                CHECK_THAT(mean(rendered), WithinAbs(2.0 * width - 1.0, 1e-9));
            }
        }
    }

    SECTION("a pulse narrower than a sample stays finite and bounded") {
        // polyBLEP cannot represent a pulse whose edges land inside one sample:
        // their corrections overlap. The contract is that the output stays
        // bounded, NOT that the pulse stays accurate — claiming accuracy here
        // would be claiming something the kernel cannot deliver.
        for (const double f0 : {1103.0, 6301.0}) {
            for (const double width : {1e-9, 1e-6, 1e-4, 0.001, 0.01, 0.99, 0.999, 0.999999}) {
                const auto rendered = render_va(VaShape::square, f0, width);
                INFO("pulse width " << width << " at f0 " << f0 << ": peak " << peak(rendered));
                for (const double x : rendered) REQUIRE(std::isfinite(x));
                CHECK(peak(rendered) <= 1.0 + 1e-9);
            }
        }
    }

    SECTION("a width outside [0, 1] is clamped rather than propagated") {
        VaOscillator osc;
        osc.set_pulse_width(-1.0);
        CHECK(osc.pulse_width() == 0.0);
        osc.set_pulse_width(2.0);
        CHECK(osc.pulse_width() == 1.0);
        osc.set_pulse_width(std::nan(""));
        CHECK(osc.pulse_width() == 0.0);
    }
}

TEST_CASE("every shape stays bounded across the audible range", "[signal][osc][va]") {
    // A correction moves a sample toward the discontinuity's midpoint, so it can
    // never push the output past the trivial shape's own range. Anything above 1
    // means a correction is being added with the wrong sign or at the wrong
    // magnitude.
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square,
                                VaShape::triangle}) {
        for (const double f0 : {20.0, 55.0, 440.0, 4100.0, 11000.0, 20000.0}) {
            const auto rendered = render_va(shape, f0);
            INFO(shape_name(shape) << " at f0 " << f0 << ": peak " << peak(rendered));
            for (const double x : rendered) REQUIRE(std::isfinite(x));
            CHECK(peak(rendered) <= 1.0 + 1e-9);
        }
    }
}

TEST_CASE("oscillator output is deterministic", "[signal][osc][va]") {
    SECTION("identical settings reproduce bit-exactly") {
        for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square,
                                    VaShape::triangle}) {
            const auto first = render_va(shape, 4100.0, 0.3);
            const auto second = render_va(shape, 4100.0, 0.3);
            REQUIRE(first.size() == second.size());
            for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
        }
    }

    SECTION("reset returns the oscillator to its initial state") {
        VaOscillator osc;
        osc.set_shape(VaShape::saw);
        osc.reset(kCleanStartPhase);
        std::vector<double> first;
        for (int i = 0; i < 512; ++i) first.push_back(osc.next(4100.0 / kSampleRate));

        // Run it somewhere else entirely, then reset: any state the corrector
        // carried across samples — the pending correction in particular — must
        // not survive.
        for (int i = 0; i < 97; ++i) osc.next(377.0 / kSampleRate);
        osc.reset(kCleanStartPhase);
        for (int i = 0; i < 512; ++i)
            REQUIRE(osc.next(4100.0 / kSampleRate) == first[static_cast<std::size_t>(i)]);
    }
}
