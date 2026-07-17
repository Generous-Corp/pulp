// OSC-VCO: the deterministic circuit-flavored VCO signal path. The bandlimited
// core is gated on measured alias rejection (band-qualified, floor proven by
// negative control); each hand-set character stage is gated on the level / DC /
// pitch fact it is supposed to produce, with a bit-exact null where it is neutral.
//
// ── Measurement discipline (same as the VA suite) ─────────────────────────
//
// Alias numbers come from `measure_aliasing` — a joint least-squares fit of the
// ideal harmonic series and every above-Nyquist harmonic's fold site, band-
// qualified to 20 kHz, with a floor PROVEN by a negative control (a signal with
// zero alias content by construction must read collapsed; an injected alias of
// known level must be recovered) rather than read off the analyzer's own
// white-residual bound.
//
// ── The master null ───────────────────────────────────────────────────────
//
// Every character stage defaults to its neutral value, applied as an exact
// bypass, so a default VcoOscillator is bit-for-bit a VaOscillator. That single
// equality is the strongest available proof that the composition adds nothing
// when neutral — including that the reserved drift/jitter site is inert — and it
// is asserted as bit-exactness, not a tolerance.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/osc/vco.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using pulp::signal::osc::VaOscillator;
using pulp::signal::osc::VaShape;
using pulp::signal::osc::VcoOscillator;
using pulp::signal::osc::VcoTuning;
using pulp::signal::osc::WaveshaperParams;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;
using pulp::test::audio::measure_thd;
using pulp::test::audio::response_relative_to_input;
using pulp::test::audio::ResponseOptions;
using pulp::test::audio::ThdOptions;
using pulp::test::audio::ThdResult;
using pulp::test::audio::tone_residual_db;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kRenderLength = 8192;
constexpr double kBandHz = 20000.0;
constexpr double kMinImprovementDb = 8.0;
constexpr double kCleanStartPhase = 0.13;

constexpr double kTestF0[] = {1103.0, 2153.0, 4100.0, 6301.0};

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

// A default (neutral) VCO, rendered from a clean start phase.
std::vector<double> render_vco_default(VaShape shape, double f0,
                                       double start_phase = kCleanStartPhase,
                                       int length = kRenderLength) {
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(shape);
    osc.reset(start_phase);
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = osc.next(increment);
    return out;
}

// The same shape from the shipped VaOscillator — the no-character reference the
// neutral VCO must equal bit-for-bit.
std::vector<double> render_va(VaShape shape, double f0,
                              double start_phase = kCleanStartPhase,
                              int length = kRenderLength) {
    VaOscillator osc;
    osc.set_shape(shape);
    osc.reset(start_phase);
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = osc.next(increment);
    return out;
}

double trivial_value(VaShape shape, double p) {
    switch (shape) {
        case VaShape::sine: return std::sin(2.0 * std::numbers::pi * p);
        case VaShape::saw: return 2.0 * p - 1.0;
        case VaShape::square: return p < 0.5 ? 1.0 : -1.0;
        case VaShape::triangle: return p < 0.5 ? (4.0 * p - 1.0) : (3.0 - 4.0 * p);
    }
    return 0.0;
}

std::vector<double> render_trivial(VaShape shape, double f0) {
    std::vector<double> out(static_cast<std::size_t>(kRenderLength));
    const double increment = f0 / kSampleRate;
    double p = kCleanStartPhase;
    for (int i = 0; i < kRenderLength; ++i) {
        out[static_cast<std::size_t>(i)] = trivial_value(shape, p);
        p += increment;
        p -= std::floor(p);
    }
    return out;
}

// An exactly bandlimited saw — every harmonic below Nyquist summed at its own
// frequency, so it carries zero alias energy BY CONSTRUCTION. The negative
// control the alias floor is proven with; optionally injects a known-level tone
// on a fold site.
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

pulp::audio::Buffer<float> to_buffer(const std::vector<double>& signal) {
    pulp::audio::Buffer<float> buffer(1, static_cast<int>(signal.size()));
    for (int i = 0; i < static_cast<int>(signal.size()); ++i)
        buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);
    return buffer;
}

AliasReport analyze(const std::vector<double>& signal, double f0) {
    auto buffer = to_buffer(signal);
    AliasOptions options;
    options.num_harmonics = harmonics_for(f0);
    options.analysis_length = static_cast<int>(signal.size());
    options.max_alias_frequency_hz = kBandHz;
    return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
}

int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1;
         h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

void require_trustworthy(const AliasReport& r) {
    REQUIRE_FALSE(r.has_unresolved_in_band_alias);
    REQUIRE(r.worst_alias_db > r.detection_floor_db);
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

// An f0 placing a whole number of cycles in the render, so a buffer mean equals
// the waveform's true mean (a fractional leftover cycle reads as false DC).
double coherent_f0(int cycles, int length = kRenderLength) {
    return static_cast<double>(cycles) * kSampleRate / static_cast<double>(length);
}

// Analytic magnitude, in dB, of the one-pole DC blocker H(z) = (1 - z^-1) /
// (1 - p z^-1) at frequency f — computed from the pole the VCO reports, so the
// corner claim is a statement about the filter the VCO actually applies.
double dc_block_mag_db(double f, double pole) {
    const double w = 2.0 * std::numbers::pi * f / kSampleRate;
    const double num = 2.0 - 2.0 * std::cos(w);              // |1 - e^{-jw}|^2
    const double den = 1.0 - 2.0 * pole * std::cos(w) + pole * pole; // |1 - p e^{-jw}|^2
    return 10.0 * std::log10(num / den);
}

} // namespace

// ── The master null: neutral VCO == the shipped core ──────────────────────

TEST_CASE("a neutral VCO is bit-for-bit the bandlimited core", "[signal][osc][vco]") {
    // Every character stage neutral, applied as an exact bypass, so the VCO adds
    // nothing. Bit-exactness (not a tolerance) is what proves it: the correct
    // difference here is not "small" but "none". This one equality simultaneously
    // establishes that the tuning/bow/waveshaper/level/AC stages and the reserved
    // drift-jitter site are all inert at their defaults.
    for (const VaShape shape : {VaShape::sine, VaShape::saw, VaShape::square,
                                VaShape::triangle}) {
        for (const double f0 : kTestF0) {
            const auto vco = render_vco_default(shape, f0);
            const auto core = render_va(shape, f0);
            REQUIRE(vco.size() == core.size());
            int differing = 0;
            for (std::size_t i = 0; i < vco.size(); ++i)
                if (vco[i] != core[i]) ++differing;
            INFO(shape_name(shape) << " at f0 " << f0 << ": " << differing
                                   << " samples differ from VaOscillator");
            CHECK(differing == 0);
        }
    }
}

TEST_CASE("the reserved drift/jitter site is inert", "[signal][osc][vco]") {
    // The drift/jitter depths default to 0 and are UNIMPLEMENTED on purpose (no
    // stochastic source until the time-domain analyzer lands). A knob that
    // silently half-worked would be worse than an absent one, so this pins that
    // even a non-zero depth changes nothing today — the output stays bit-identical
    // to the no-path reference. When a source is wired, this test flips to
    // asserting the perturbation instead.
    for (const double f0 : kTestF0) {
        const auto reference = render_va(VaShape::saw, f0);

        VcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::saw);
        osc.set_drift_depth(0.5);
        osc.set_jitter_depth(0.5);
        osc.reset(kCleanStartPhase);
        const double increment = f0 / kSampleRate;

        int differing = 0;
        for (std::size_t i = 0; i < reference.size(); ++i)
            if (osc.next(increment) != reference[i]) ++differing;
        INFO("f0 " << f0 << ": " << differing << " samples perturbed by reserved depths");
        CHECK(differing == 0);
    }
}

// ── The measurement, before anything is measured with it ──────────────────

TEST_CASE("VCO alias measurement separates clean from known-bad", "[signal][osc][vco][alias]") {
    // The negative control every VCO alias gate is read through. The analyzer's
    // own floor assumes a white residual (false — aliases are discrete tones), so
    // proof is by construction: a bandlimited saw with no alias must collapse, and
    // an injected alias of known level must come back at that level.
    for (const double f0 : kTestF0) {
        const int h = first_in_band_alias_harmonic(f0);
        REQUIRE(h > 0);
        const double site_hz = fold_frequency(h * f0, kSampleRate);
        REQUIRE(site_hz <= kBandHz);

        SECTION("alias removed: the reading collapses") {
            const auto report = analyze(bandlimited_saw(f0), f0);
            INFO("f0 " << f0 << ": clean bandlimited saw reads " << report.worst_alias_db
                       << " dBc (noise " << report.noise_db << ")");
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            CHECK(report.worst_alias_db < -140.0);
        }

        SECTION("alias injected at a known level: it is recovered") {
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

TEST_CASE("VCO core improves alias rejection below 20 kHz per shape",
          "[signal][osc][vco][alias]") {
    // The core's bandlimiting, measured through the neutral VCO. The improvement
    // over the trivial shape is the polyBLEP figure the composition inherits from
    // VaOscillator; the real per-shape number is logged, and the gate is a
    // regression tripwire well below it (and far above 0 dB = did nothing).
    for (const VaShape shape : {VaShape::saw, VaShape::square, VaShape::triangle}) {
        for (const double f0 : kTestF0) {
            const auto trivial = analyze(render_trivial(shape, f0), f0);
            const auto corrected = analyze(render_vco_default(shape, f0), f0);
            const double improvement = trivial.worst_alias_db - corrected.worst_alias_db;

            INFO(shape_name(shape) << " at f0 " << f0 << ": worst in-band alias "
                 << trivial.worst_alias_db << " dBc trivial -> " << corrected.worst_alias_db
                 << " dBc corrected = " << improvement << " dB improvement (at "
                 << corrected.worst_alias_hz << " Hz); full-band "
                 << corrected.full_band_worst_alias_db << " dBc");

            require_trustworthy(corrected);
            REQUIRE_FALSE(trivial.has_unresolved_in_band_alias);
            CHECK(improvement >= kMinImprovementDb);
            CHECK(corrected.worst_alias_db > -120.0);
        }
    }
}

// ── Tuning / V-oct front end ───────────────────────────────────────────────

TEST_CASE("the tuning curve is the 1 V/oct exponential law", "[signal][osc][vco][tuning]") {
    SECTION("neutral calibration is exact 2^(V/oct)") {
        VcoTuning t;
        for (const double volts : {-2.0, -1.0, 0.0, 1.0, 2.5, 5.0}) {
            const double expected = t.reference_hz * std::exp2(volts);
            INFO("V " << volts << ": " << t.frequency_hz(volts) << " vs " << expected);
            CHECK_THAT(t.frequency_hz(volts), WithinRel(expected, 1e-12));
        }
    }

    SECTION("a calibration offset shifts every note by exactly that many cents") {
        VcoTuning t;
        VcoTuning shifted = t;
        shifted.tune_offset_cents = 37.0;
        const double ratio = std::exp2(37.0 / 1200.0);
        for (const double volts : {-1.0, 0.0, 2.0}) {
            INFO("V " << volts << ": ratio "
                      << shifted.frequency_hz(volts) / t.frequency_hz(volts));
            CHECK_THAT(shifted.frequency_hz(volts) / t.frequency_hz(volts),
                       WithinRel(ratio, 1e-12));
        }
    }

    SECTION("a V/oct scale error widens octaves away from the reference") {
        VcoTuning t;
        t.scale_error = 1.01; // each octave 1% wide.
        // At the reference (0 V) it is still exact; an octave up is 1% sharp.
        CHECK_THAT(t.frequency_hz(0.0), WithinRel(t.reference_hz, 1e-12));
        CHECK_THAT(t.frequency_hz(1.0), WithinRel(t.reference_hz * std::exp2(1.01), 1e-12));
    }

    SECTION("high-end tracking compression tracks flat above the knee only") {
        VcoTuning t;
        t.hf_compression = 0.05;
        t.hf_knee_octaves = 3.0;
        // Below the knee is untouched.
        CHECK_THAT(t.frequency_hz(2.0), WithinRel(t.reference_hz * std::exp2(2.0), 1e-12));
        // Above it, five octaves up loses 0.05 * (5 - 3) = 0.1 octave.
        const double expected = t.reference_hz * std::exp2(5.0 - 0.05 * (5.0 - 3.0));
        CHECK_THAT(t.frequency_hz(5.0), WithinRel(expected, 1e-12));
        CHECK(t.frequency_hz(5.0) < t.reference_hz * std::exp2(5.0)); // flat = lower.
    }
}

TEST_CASE("the tuning front end drives the clock at the commanded pitch",
          "[signal][osc][vco][tuning]") {
    // The front end is only useful if it actually sets the oscillator's rate. For
    // several commanded pitches, render a sine at the increment the front end
    // produces and confirm — by an independent least-squares tone fit AT the
    // commanded frequency — that essentially all the energy sits there: a wrong
    // increment would leave a large residual.
    VcoTuning t;
    for (const double volts : {-1.0, 0.0, 1.5, 3.0}) {
        const double f = t.frequency_hz(volts);
        const double increment = t.phase_increment(volts, kSampleRate);
        REQUIRE_THAT(increment, WithinRel(f / kSampleRate, 1e-12));

        VcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::sine);
        osc.reset(0.0);
        std::vector<double> render(static_cast<std::size_t>(kRenderLength));
        for (int i = 0; i < kRenderLength; ++i)
            render[static_cast<std::size_t>(i)] = osc.next(increment);

        const double residual = tone_residual_db(std::span<const double>(render), f / kSampleRate);
        INFO("V " << volts << " -> " << f << " Hz: tone residual " << residual << " dB");
        CHECK(residual < -100.0);
    }
}

// ── Integrator-leak bow ────────────────────────────────────────────────────

TEST_CASE("the bow reshapes the saw without sliding level with pitch",
          "[signal][osc][vco][bow]") {
    // The bow changes the ramp's shape (its RMS moves off an ideal saw's) but,
    // because it is a pure function of phase, its level does NOT depend on pitch —
    // the exact leaky-integrator defect the suite guards against. Level stages
    // are kept neutral so the only thing under test is the bow.
    constexpr double kIdealSawRms = 0.57735026918962576; // 1/sqrt(3).
    const double bowed_ref = rms(render_vco_default(VaShape::saw, 1103.0)); // neutral, for contrast.

    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::saw);
    osc.set_bow(2.5);

    double lowest = 1e9, highest = -1e9, bowed_rms = 0.0;
    for (const double f0 : {55.0, 110.0, 220.0, 440.0, 880.0, 1760.0, 3520.0}) {
        osc.reset(kCleanStartPhase);
        std::vector<double> render(static_cast<std::size_t>(kRenderLength));
        const double increment = f0 / kSampleRate;
        for (int i = 0; i < kRenderLength; ++i)
            render[static_cast<std::size_t>(i)] = osc.next(increment);
        bowed_rms = rms(render);
        lowest = std::fmin(lowest, bowed_rms);
        highest = std::fmax(highest, bowed_rms);
        INFO("bowed saw at f0 " << f0 << ": RMS " << bowed_rms << ", peak " << peak(render));
        CHECK(peak(render) <= 1.0 + 1e-9); // never exceeds the waveform's own range.
    }

    // The shape genuinely changed: a bowed ramp's RMS is not an ideal saw's.
    INFO("bowed RMS " << bowed_rms << " vs ideal saw " << kIdealSawRms
                      << " (neutral VCO saw RMS " << bowed_ref << ")");
    CHECK(std::fabs(bowed_rms - kIdealSawRms) > 0.02);

    // And the level is pitch-independent to a tight bound (the phase-domain
    // reshape carries no cutoff-to-fundamental coupling).
    const double spread_db = 20.0 * std::log10(highest / lowest);
    INFO("bowed RMS spread across 55 Hz .. 3520 Hz = " << spread_db << " dB");
    CHECK(spread_db < 0.5);
}

TEST_CASE("bow 0 is an exact bypass", "[signal][osc][vco][bow]") {
    // The neutral value of the stage restores the core bit-for-bit — the reshape
    // must not perturb through its own arithmetic when it is off.
    const auto neutral = render_va(VaShape::saw, 2153.0);
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::saw);
    osc.set_bow(0.0);
    osc.reset(kCleanStartPhase);
    int differing = 0;
    for (std::size_t i = 0; i < neutral.size(); ++i)
        if (osc.next(2153.0 / kSampleRate) != neutral[i]) ++differing;
    CHECK(differing == 0);
}

// ── AC coupling (output DC blocker) ────────────────────────────────────────

TEST_CASE("AC coupling blocks the bow's DC where the corner says",
          "[signal][osc][vco][ac]") {
    // The bowed saw is asymmetric about zero, so it has real DC by construction;
    // the DC-blocking output cap removes it. The physically-right output DC is 0,
    // asserted as such — not a convenient near-value.
    const int cycles = 367;
    const double f0 = coherent_f0(cycles); // whole cycles: buffer mean == true mean.

    // With AC off the bowed saw carries its DC.
    VcoOscillator dc_on;
    dc_on.prepare(kSampleRate);
    dc_on.set_shape(VaShape::saw);
    dc_on.set_bow(2.5);
    dc_on.reset(0.0);
    std::vector<double> without_ac(static_cast<std::size_t>(kRenderLength));
    for (int i = 0; i < kRenderLength; ++i)
        without_ac[static_cast<std::size_t>(i)] = dc_on.next(f0 / kSampleRate);
    const double bow_dc = mean(without_ac);
    INFO("bowed saw DC before AC coupling: " << bow_dc);
    CHECK(std::fabs(bow_dc) > 0.05); // there is real DC to remove.

    // With AC on, the DC is gone after the filter settles.
    VcoOscillator ac;
    ac.prepare(kSampleRate);
    ac.set_shape(VaShape::saw);
    ac.set_bow(2.5);
    ac.set_ac_coupling(20.0);
    ac.reset(0.0);
    std::vector<double> with_ac(static_cast<std::size_t>(kRenderLength));
    for (int i = 0; i < kRenderLength; ++i)
        with_ac[static_cast<std::size_t>(i)] = ac.next(f0 / kSampleRate);
    // Measure the mean over settled samples only (skip the highpass transient).
    double settled = 0.0;
    const int skip = 2048;
    for (int i = skip; i < kRenderLength; ++i) settled += with_ac[static_cast<std::size_t>(i)];
    settled /= static_cast<double>(kRenderLength - skip);
    INFO("bowed saw DC after AC coupling: " << settled);
    CHECK_THAT(settled, WithinAbs(0.0, 1e-3));

    // The corner is where the pole the VCO reports puts it: -3 dB at 20 Hz, near
    // unity a decade up, and deep attenuation at DC.
    const double pole = ac.ac_pole();
    INFO("AC pole " << pole << ": |H(20)| " << dc_block_mag_db(20.0, pole)
                    << " dB, |H(200)| " << dc_block_mag_db(200.0, pole)
                    << " dB, |H(1)| " << dc_block_mag_db(1.0, pole));
    CHECK_THAT(dc_block_mag_db(20.0, pole), WithinAbs(-3.0, 0.4));
    CHECK(dc_block_mag_db(200.0, pole) > -0.2);
    CHECK(dc_block_mag_db(1.0, pole) < -20.0);
}

TEST_CASE("the VCO applies the DC blocker's own LTI response", "[signal][osc][vco][ac]") {
    // Prove the corner empirically, not just analytically: drive an impulse
    // through a reference DcBlocker seeded with the pole the VCO computed, and
    // confirm the measured transfer magnitude matches the requested corner. This
    // ties the VCO's reported pole to a real filter response.
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_ac_coupling(50.0);
    const double pole = osc.ac_pole();

    constexpr int n = 16384;
    std::vector<double> impulse(static_cast<std::size_t>(n), 0.0);
    impulse[0] = 1.0;
    pulp::signal::DcBlocker<double> ref;
    ref.set_pole(pole);
    std::vector<double> response(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) response[static_cast<std::size_t>(i)] = ref.process(impulse[static_cast<std::size_t>(i)]);

    auto in_buf = to_buffer(impulse);
    auto out_buf = to_buffer(response);
    ResponseOptions options;
    options.fft_length = n;
    const double checkpoints[] = {50.0, 500.0};
    const auto curve = response_relative_to_input(std::as_const(in_buf).view(),
                                                  std::as_const(out_buf).view(), kSampleRate,
                                                  std::span<const double>(checkpoints), options);
    INFO("measured |H(50)| " << curve.magnitude_db_at(50.0) << " dB, |H(500)| "
                             << curve.magnitude_db_at(500.0) << " dB (analytic corner "
                             << dc_block_mag_db(50.0, pole) << " dB)");
    CHECK_THAT(curve.magnitude_db_at(50.0), WithinAbs(-3.0, 0.6));
    CHECK(curve.magnitude_db_at(500.0) > -0.5);
}

TEST_CASE("AC coupling off is an exact bypass", "[signal][osc][vco][ac]") {
    const auto neutral = render_va(VaShape::saw, 4100.0);
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::saw);
    osc.set_ac_coupling(0.0);
    osc.reset(kCleanStartPhase);
    int differing = 0;
    for (std::size_t i = 0; i < neutral.size(); ++i)
        if (osc.next(4100.0 / kSampleRate) != neutral[i]) ++differing;
    CHECK(differing == 0);
}

// ── Per-shape waveshaper ───────────────────────────────────────────────────

namespace {

// A bin-coherent tone near `approx_hz` for a length-`n` FFT, so the fundamental
// and every harmonic land on exact bins.
double coherent_tone(double approx_hz, int n) {
    const int k = static_cast<int>(std::round(approx_hz * n / kSampleRate));
    return static_cast<double>(k) * kSampleRate / static_cast<double>(n);
}

ThdResult shaped_sine_thd(const WaveshaperParams& params, double f0) {
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::sine);
    osc.set_waveshaper(VaShape::sine, params);
    osc.reset(0.0);
    std::vector<double> render(static_cast<std::size_t>(kRenderLength));
    for (int i = 0; i < kRenderLength; ++i)
        render[static_cast<std::size_t>(i)] = osc.next(f0 / kSampleRate);
    auto buffer = to_buffer(render);
    ThdOptions options;
    options.fft_length = kRenderLength;
    return measure_thd(std::as_const(buffer).view(), f0, kSampleRate, options);
}

} // namespace

TEST_CASE("the waveshaper's character is correct and its null is exact",
          "[signal][osc][vco][shaper]") {
    const double f0 = coherent_tone(1000.0, kRenderLength);

    SECTION("no shaping is bit-exact against the unshaped sine") {
        const auto plain = render_va(VaShape::sine, f0, 0.0);
        VcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::sine);
        osc.set_waveshaper(VaShape::sine, {}); // amount 0.
        osc.reset(0.0);
        int differing = 0;
        for (std::size_t i = 0; i < plain.size(); ++i)
            if (osc.next(f0 / kSampleRate) != plain[i]) ++differing;
        CHECK(differing == 0);
    }

    SECTION("a symmetric drive makes odd harmonics, not even ones") {
        WaveshaperParams sym;
        sym.amount = 1.0;
        sym.drive = 2.0;
        const auto thd = shaped_sine_thd(sym, f0);
        REQUIRE(thd.harmonics.size() >= 3);
        const double second = thd.harmonics[1].db_below_fundamental;
        const double third = thd.harmonics[2].db_below_fundamental;
        INFO("symmetric tanh drive: 2nd " << second << " dBc, 3rd " << third << " dBc");
        CHECK(third > -45.0);          // the odd harmonic is clearly present.
        CHECK(second < third - 20.0);  // the even harmonic is far below it.
    }

    SECTION("asymmetry introduces the even harmonic the symmetric curve suppressed") {
        WaveshaperParams sym;
        sym.amount = 1.0;
        sym.drive = 2.0;
        WaveshaperParams asym = sym;
        asym.asymmetry = 0.3;
        const double second_sym = shaped_sine_thd(sym, f0).harmonics[1].db_below_fundamental;
        const double second_asym = shaped_sine_thd(asym, f0).harmonics[1].db_below_fundamental;
        INFO("2nd harmonic: symmetric " << second_sym << " dBc, asymmetric " << second_asym
                                        << " dBc");
        CHECK(second_asym > second_sym + 20.0);
    }
}

// ── Level vs pitch ─────────────────────────────────────────────────────────

TEST_CASE("the level tilt is the commanded dB per octave", "[signal][osc][vco][level]") {
    // A tilt of T dB/oct means one octave up is exactly T dB quieter. The sine is
    // used so RMS reads the level directly, with no shape harmonics in the way.
    const double tilt = 3.0;
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::sine);
    osc.set_level_tilt(tilt);

    auto render_rms = [&](double f0) {
        osc.reset(0.0);
        std::vector<double> r(static_cast<std::size_t>(kRenderLength));
        for (int i = 0; i < kRenderLength; ++i) r[static_cast<std::size_t>(i)] = osc.next(f0 / kSampleRate);
        return rms(r);
    };

    const double low = render_rms(500.0);
    const double high = render_rms(1000.0); // one octave up.
    const double measured = 20.0 * std::log10(high / low);
    INFO("500 Hz RMS " << low << ", 1000 Hz RMS " << high << ": tilt " << measured
                       << " dB/oct (commanded " << -tilt << ")");
    CHECK_THAT(measured, WithinAbs(-tilt, 0.05));
}

TEST_CASE("finite core-reset time sags level in proportion to pitch",
          "[signal][osc][vco][level]") {
    // A fixed reset duration eats a fraction reset*f of every period, so the
    // level falls as 1 - reset*f. Asserted against that geometry, and bounded
    // (never negative or exploding).
    const double reset = 5e-6; // 5 microseconds.
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::sine);
    osc.set_core_reset_time(reset);

    for (const double f0 : {440.0, 2000.0, 8000.0}) {
        osc.reset(0.0);
        std::vector<double> r(static_cast<std::size_t>(kRenderLength));
        for (int i = 0; i < kRenderLength; ++i) r[static_cast<std::size_t>(i)] = osc.next(f0 / kSampleRate);
        const double level = rms(r);
        const double expected_gain = 1.0 - reset * f0;
        const double ideal_sine_rms = std::numbers::sqrt2 / 2.0; // 1/sqrt(2).
        INFO("f0 " << f0 << ": RMS " << level << " vs 1/sqrt(2) * (1 - reset*f) = "
                   << ideal_sine_rms * expected_gain);
        CHECK_THAT(level, WithinRel(ideal_sine_rms * expected_gain, 1e-3));
        CHECK(peak(r) <= 1.0 + 1e-9);
    }
}

TEST_CASE("level stages off are an exact bypass", "[signal][osc][vco][level]") {
    const auto neutral = render_va(VaShape::sine, 1103.0);
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::sine);
    osc.set_level_tilt(0.0);
    osc.set_core_reset_time(0.0);
    osc.reset(kCleanStartPhase);
    int differing = 0;
    for (std::size_t i = 0; i < neutral.size(); ++i)
        if (osc.next(1103.0 / kSampleRate) != neutral[i]) ++differing;
    CHECK(differing == 0);
}

// ── Determinism ────────────────────────────────────────────────────────────

TEST_CASE("VCO output is deterministic with character engaged", "[signal][osc][vco]") {
    auto configure = [](VcoOscillator& osc) {
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::saw);
        osc.set_bow(1.7);
        osc.set_level_tilt(2.0);
        osc.set_core_reset_time(3e-6);
        osc.set_ac_coupling(25.0);
        WaveshaperParams w;
        w.amount = 0.6;
        w.drive = 1.5;
        w.asymmetry = 0.2;
        osc.set_waveshaper(VaShape::saw, w);
        osc.reset(kCleanStartPhase);
    };

    SECTION("identical settings reproduce bit-exactly") {
        VcoOscillator a, b;
        configure(a);
        configure(b);
        for (int i = 0; i < kRenderLength; ++i)
            REQUIRE(a.next(2153.0 / kSampleRate) == b.next(2153.0 / kSampleRate));
    }

    SECTION("reset returns every stage to its initial state") {
        VcoOscillator osc;
        configure(osc);
        std::vector<double> first;
        for (int i = 0; i < 512; ++i) first.push_back(osc.next(2153.0 / kSampleRate));
        // Run it elsewhere (exercising the AC blocker's state), then reset.
        for (int i = 0; i < 97; ++i) osc.next(377.0 / kSampleRate);
        osc.reset(kCleanStartPhase);
        for (int i = 0; i < 512; ++i)
            REQUIRE(osc.next(2153.0 / kSampleRate) == first[static_cast<std::size_t>(i)]);
    }
}
