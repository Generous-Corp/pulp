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
#include <pulp/audio/analysis/pitch_track.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/dc_blocker.hpp>
#include <pulp/signal/osc/phase.hpp>
#include <pulp/signal/osc/va.hpp>
#include <pulp/signal/osc/vco.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <tuple>
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
using pulp::test::audio::PitchTrackOptions;
using pulp::test::audio::response_relative_to_input;
using pulp::test::audio::track_pitch;
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

// A sine at f0 modulated by the seeded drift/jitter source, as float samples for
// the pitch tracker. A `warmup` prefix is rendered and discarded so the drift's
// leaky integrator reaches its stationary variance before the analyzed span (the
// walk starts from 0 at reset). Sine is used so f0(t) is read with no shape
// harmonics in the way; the noise injection is upstream of the shape, so the
// choice does not affect what is measured.
std::vector<float> render_noise_sine(double f0, double drift_depth, double drift_rate,
                                     double jitter_depth, std::uint64_t seed,
                                     int analysis_len, int warmup = 0) {
    VcoOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_shape(VaShape::sine);
    osc.set_seed(seed);
    osc.set_drift_rate_hz(drift_rate);
    osc.set_drift_depth(drift_depth);
    osc.set_jitter_depth(jitter_depth);
    osc.reset(0.0);
    std::vector<float> out(static_cast<std::size_t>(analysis_len));
    const double increment = f0 / kSampleRate;
    const int total = warmup + analysis_len;
    for (int i = 0; i < total; ++i) {
        const double v = osc.next(increment);
        if (i >= warmup) out[static_cast<std::size_t>(i - warmup)] = static_cast<float>(v);
    }
    return out;
}

// Summary of an f0(t) trajectory: how much the pitch wandered (RMS in cents about
// the trajectory's log-mean, which removes any static tuning offset) and how
// coherent that wander is frame-to-frame (lag-1 autocorrelation — near 1 for a
// slow process, near 0 for a white one across non-overlapping frames).
struct PitchStats {
    int voiced = 0;
    int total = 0;
    double rms_cents = 0.0;
    double autocorr1 = 0.0;
    bool any_nonfinite = false;
};

PitchStats pitch_stats(const std::vector<float>& signal, int window, int hop) {
    PitchTrackOptions options;
    options.window_length = window;
    options.hop_length = hop;
    const auto track = track_pitch(std::span<const float>(signal), kSampleRate, options);

    PitchStats stats;
    stats.total = static_cast<int>(track.points.size());
    const std::vector<double> hz = track.voiced_hz();
    stats.voiced = static_cast<int>(hz.size());
    for (const double f : hz)
        if (!std::isfinite(f) || !(f > 0.0)) stats.any_nonfinite = true;
    if (hz.size() < 2 || stats.any_nonfinite) return stats;

    // Cents about the log-mean.
    double log_mean = 0.0;
    for (const double f : hz) log_mean += std::log2(f);
    log_mean /= static_cast<double>(hz.size());
    std::vector<double> cents(hz.size());
    for (std::size_t i = 0; i < hz.size(); ++i)
        cents[i] = 1200.0 * (std::log2(hz[i]) - log_mean);

    double sumsq = 0.0;
    for (const double c : cents) sumsq += c * c;
    const double variance = sumsq / static_cast<double>(cents.size());
    stats.rms_cents = std::sqrt(variance);

    double lag1 = 0.0;
    for (std::size_t i = 1; i < cents.size(); ++i) lag1 += cents[i] * cents[i - 1];
    lag1 /= static_cast<double>(cents.size() - 1);
    stats.autocorr1 = variance > 0.0 ? lag1 / variance : 0.0;
    return stats;
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

TEST_CASE("zero depth is a bit-exact bypass; non-zero depth perturbs the clock",
          "[signal][osc][vco][noise]") {
    // The drift/jitter site injects a seeded frequency modulation, but ONLY when a
    // depth is non-zero. At depth 0 the factor is 1.0 bit-for-bit and no noise
    // state advances, so the output stays bit-identical to the no-noise core — the
    // master null the whole composition rests on. A non-zero depth must then
    // actually change the clock (the reserved-knob-that-silently-did-nothing state
    // this replaces). The rigorous drift-vs-jitter Allan-slope SEPARATION is
    // deferred to the offline Python lane (it needs the WAV bridge); here the C++
    // behavior — bypass, perturbation, scaling, slow-vs-fast character,
    // determinism, bounds — is what is gated.
    SECTION("both depths zero: bit-identical to the bandlimited core, seed aside") {
        for (const double f0 : kTestF0) {
            const auto reference = render_va(VaShape::saw, f0);
            VcoOscillator osc;
            osc.prepare(kSampleRate);
            osc.set_shape(VaShape::saw);
            osc.set_drift_depth(0.0);
            osc.set_jitter_depth(0.0);
            osc.set_seed(0xABCDEF01u); // irrelevant while both depths are 0.
            osc.reset(kCleanStartPhase);
            int differing = 0;
            for (std::size_t i = 0; i < reference.size(); ++i)
                if (osc.next(f0 / kSampleRate) != reference[i]) ++differing;
            INFO("f0 " << f0 << ": " << differing << " samples differ at zero depth");
            CHECK(differing == 0);
        }
    }

    SECTION("a non-zero depth changes the output from the neutral path") {
        const double f0 = 1103.0;
        const auto neutral = render_va(VaShape::sine, f0, 0.0);

        auto perturbed_count = [&](double drift, double jitter) {
            VcoOscillator osc;
            osc.prepare(kSampleRate);
            osc.set_shape(VaShape::sine);
            osc.set_drift_depth(drift);
            osc.set_jitter_depth(jitter);
            osc.set_seed(1);
            osc.reset(0.0);
            int differing = 0;
            for (std::size_t i = 0; i < neutral.size(); ++i)
                if (osc.next(f0 / kSampleRate) != neutral[i]) ++differing;
            return differing;
        };

        CHECK(perturbed_count(10.0, 0.0) > 0);  // drift alone perturbs.
        CHECK(perturbed_count(0.0, 10.0) > 0);  // jitter alone perturbs.
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

    // The above ties the corner to a reference filter seeded with the REPORTED
    // pole. Also prove the pole is actually APPLIED to the VCO's own output: a
    // wiring bug that computed ac_pole_ but never called dc_blocker_.set_pole
    // would pass everything above. Render the VCO's own sine at a frequency
    // below the corner through two different corners; the higher corner is a
    // steeper highpass there, so it must attenuate that tone MORE. This can only
    // be true if the corner reaches the output path.
    auto out_rms = [](VcoOscillator& o, double f0, double corner) {
        o.prepare(kSampleRate);
        o.set_shape(VaShape::sine);
        o.set_ac_coupling(corner);
        o.reset();
        constexpr int len = 48000;
        double sum = 0.0;
        for (int i = 0; i < len; ++i) {
            const double v = o.next(f0 / kSampleRate);
            if (i > 4096) sum += v * v; // skip the highpass settling transient.
        }
        return std::sqrt(sum / (len - 4096));
    };
    VcoOscillator a, b;
    const double rms_low_corner = out_rms(a, 40.0, 50.0);
    const double rms_high_corner = out_rms(b, 40.0, 400.0);
    INFO("40 Hz tone RMS through the VCO: 50 Hz corner " << rms_low_corner
         << ", 400 Hz corner " << rms_high_corner);
    CHECK(rms_high_corner < rms_low_corner * 0.9); // higher corner cuts it more.
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

// ── Drift and jitter (seeded pitch noise), measured via track_pitch ─────────
//
// Each claim carries a number and, where a floor is involved, a negative
// control. The drift and jitter amplitudes are recovered from the f0(t)
// trajectory the shipped `track_pitch` demodulates — never hand-rolled. What is
// NOT proven here, on purpose: the rigorous drift-vs-jitter SEPARATION by Allan
// slope (drift ∝ τ^{+1/2}, jitter ∝ τ^{-1/2}) is a later offline Python-lane
// increment that needs the WAV bridge. In C++ the two are told apart by the
// coarser-but-sufficient time-domain character below — drift's f0(t) is coherent
// frame-to-frame, jitter's is not.

TEST_CASE("drift wanders the pitch slowly at ~the commanded RMS",
          "[signal][osc][vco][drift]") {
    constexpr double f0 = 1000.0;
    constexpr int kLen = static_cast<int>(10.0 * kSampleRate); // 10 s analyzed.
    constexpr int kWarmup = static_cast<int>(2.0 * kSampleRate); // let the walk settle.
    constexpr int kWin = 4096;   // 85 ms ≪ the drift correlation time, so a frame
    constexpr int kHop = 4096;   // reads the quasi-static drifted pitch; non-overlap.
    constexpr double kRate = VcoOscillator::kDefaultDriftRateHz;

    // Average measured statistics over several seeds — one 10 s realization of a
    // ~0.4 Hz process has real variance; the mean over seeds is what the tolerance
    // is set against (the plan asks for M ≥ 8 on stochastic rows).
    constexpr int kSeeds = 8;
    auto averaged = [&](double depth, std::uint64_t base) {
        PitchStats acc;
        for (int s = 0; s < kSeeds; ++s) {
            const auto sig = render_noise_sine(f0, depth, kRate, 0.0,
                                               base + static_cast<std::uint64_t>(s), kLen, kWarmup);
            const PitchStats st = pitch_stats(sig, kWin, kHop);
            REQUIRE(st.voiced > 20);
            REQUIRE_FALSE(st.any_nonfinite);
            acc.rms_cents += st.rms_cents;
            acc.autocorr1 += st.autocorr1;
        }
        acc.rms_cents /= kSeeds;
        acc.autocorr1 /= kSeeds;
        return acc;
    };

    SECTION("negative control: no drift → f0(t) is flat to the analyzer floor") {
        const auto sig = render_noise_sine(f0, 0.0, kRate, 0.0, 1, kLen, kWarmup);
        const PitchStats st = pitch_stats(sig, kWin, kHop);
        REQUIRE(st.voiced > 20);
        INFO("clean sine f0(t) RMS " << st.rms_cents << " cents over " << st.voiced
                                     << " voiced frames");
        CHECK(st.rms_cents < 0.5);
    }

    SECTION("drift on: the wander matches the commanded RMS and is slow") {
        constexpr double depth = 20.0; // cents RMS.
        const PitchStats st = averaged(depth, 100);
        INFO("drift depth " << depth << " cents: measured RMS " << st.rms_cents
                            << " cents, lag-1 autocorr " << st.autocorr1);
        CHECK(st.rms_cents > 0.6 * depth);
        CHECK(st.rms_cents < 1.4 * depth);
        // Slow: 85 ms-spaced frames are strongly correlated (a coherent low-freq
        // wander), unlike white jitter whose non-overlapping frames are not.
        CHECK(st.autocorr1 > 0.4);
    }

    SECTION("drift scales: doubling the depth ~doubles the wander") {
        const double rms10 = averaged(10.0, 200).rms_cents;
        const double rms20 = averaged(20.0, 300).rms_cents;
        const double ratio = rms20 / rms10;
        INFO("drift RMS: 10 cents -> " << rms10 << ", 20 cents -> " << rms20
                                       << " (ratio " << ratio << ")");
        CHECK(ratio > 1.5);
        CHECK(ratio < 2.5);
    }

    SECTION("a non-positive drift rate is off, not the fastest wander") {
        // Lower rate is slower, so the limit as rate -> 0 is a frozen walk with
        // no wander. A rate of 0 or below must take that limit, NOT invert to
        // full-depth per-sample white noise. With drift depth engaged but the
        // rate at 0 and below, the f0(t) RMS must stay near the clean floor, not
        // near the commanded 20 cents.
        constexpr int kWin = 4096, kHop = 1024, kLen = 96000;
        for (double rate : {0.0, -1.0}) {
            const auto sig = render_noise_sine(f0, 20.0, rate, 0.0, 1, kLen);
            const PitchStats st = pitch_stats(sig, kWin, kHop);
            INFO("drift rate " << rate << ": f0(t) RMS " << st.rms_cents << " cents");
            REQUIRE_FALSE(st.any_nonfinite);
            CHECK(st.rms_cents < 1.0); // frozen ~= no wander, far below 20 cents.
        }
    }
}

TEST_CASE("jitter adds fast, white cycle-to-cycle pitch noise",
          "[signal][osc][vco][jitter]") {
    constexpr double f0 = 3000.0;
    constexpr int kLen = static_cast<int>(6.0 * kSampleRate);
    constexpr int kWin = 1024;   // short window: less averaging of the per-sample
    constexpr int kHop = 1024;   // white FM; non-overlap so frames are independent.

    constexpr int kSeeds = 6;
    auto averaged = [&](double depth, std::uint64_t base) {
        double rms = 0.0, ac = 0.0;
        int voiced_min = 1 << 30;
        for (int s = 0; s < kSeeds; ++s) {
            const auto sig = render_noise_sine(f0, 0.0, VcoOscillator::kDefaultDriftRateHz,
                                               depth, base + static_cast<std::uint64_t>(s), kLen);
            const PitchStats st = pitch_stats(sig, kWin, kHop);
            REQUIRE_FALSE(st.any_nonfinite);
            voiced_min = std::min(voiced_min, st.voiced);
            rms += st.rms_cents;
            ac += st.autocorr1;
        }
        return std::tuple<double, double, int>{rms / kSeeds, ac / kSeeds, voiced_min};
    };

    SECTION("negative control: no jitter → f0(t) is flat") {
        const auto sig = render_noise_sine(f0, 0.0, VcoOscillator::kDefaultDriftRateHz,
                                           0.0, 1, kLen);
        const PitchStats st = pitch_stats(sig, kWin, kHop);
        REQUIRE(st.voiced > 50);
        INFO("clean sine f0(t) RMS " << st.rms_cents << " cents");
        CHECK(st.rms_cents < 0.5);
    }

    SECTION("jitter on: measurable in f0(t), and white (uncorrelated frames)") {
        constexpr double depth = 60.0; // cents RMS per sample.
        const auto [rms, ac, voiced_min] = averaged(depth, 300);
        REQUIRE(voiced_min > 50);
        // A white per-sample deviation of `depth` cents RMS averages, over a
        // window of kWin samples, to depth/sqrt(kWin) of frame-to-frame RMS.
        // Assert BOTH sides of that expectation: a lower bound alone passes a
        // source that is calibrated too STRONG, and the scaling section below
        // cannot catch a global gain error (it cancels in the 40:80 ratio). The
        // band mirrors the drift test's 0.6-1.4x.
        const double expected = depth / std::sqrt(static_cast<double>(kWin));
        INFO("jitter depth " << depth << " cents/sample: frame RMS " << rms
                             << " cents (expected ~" << expected
                             << "), lag-1 autocorr " << ac);
        CHECK(rms > expected * 0.6);
        CHECK(rms < expected * 1.4);
        CHECK(std::fabs(ac) < 0.25); // white: independent non-overlapping frames.
    }

    SECTION("jitter scales: doubling the depth ~doubles the frame noise") {
        const auto [rms40, ac40, v40] = averaged(40.0, 400);
        const auto [rms80, ac80, v80] = averaged(80.0, 500);
        (void)ac40; (void)ac80; (void)v40; (void)v80;
        const double ratio = rms80 / rms40;
        INFO("jitter RMS: 40 -> " << rms40 << ", 80 -> " << rms80 << " (ratio " << ratio << ")");
        CHECK(ratio > 1.5);
        CHECK(ratio < 2.5);
    }
}

TEST_CASE("VCO noise is deterministic and seed-controlled",
          "[signal][osc][vco][noise]") {
    auto render_noisy = [](std::uint64_t seed, double f0) {
        VcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::saw);
        osc.set_seed(seed);
        osc.set_drift_depth(15.0);
        osc.set_jitter_depth(30.0);
        osc.reset(kCleanStartPhase);
        std::vector<double> out(static_cast<std::size_t>(kRenderLength));
        for (int i = 0; i < kRenderLength; ++i)
            out[static_cast<std::size_t>(i)] = osc.next(f0 / kSampleRate);
        return out;
    };

    SECTION("same seed reproduces bit-for-bit") {
        const auto a = render_noisy(7, 1103.0);
        const auto b = render_noisy(7, 1103.0);
        int differing = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++differing;
        CHECK(differing == 0);
    }

    SECTION("different seeds differ, and both stay valid") {
        const auto a = render_noisy(7, 1103.0);
        const auto b = render_noisy(8, 1103.0);
        int differing = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (a[i] != b[i]) ++differing;
        INFO(differing << " of " << a.size() << " samples differ between seeds");
        CHECK(differing > 0);
        for (const auto* r : {&a, &b})
            for (const double x : *r) REQUIRE(std::isfinite(x));
    }

    SECTION("reset restarts the identical noise sequence") {
        VcoOscillator osc;
        osc.prepare(kSampleRate);
        osc.set_shape(VaShape::saw);
        osc.set_seed(9);
        osc.set_drift_depth(15.0);
        osc.set_jitter_depth(30.0);
        osc.reset(kCleanStartPhase);
        std::vector<double> first;
        for (int i = 0; i < 512; ++i) first.push_back(osc.next(1103.0 / kSampleRate));
        for (int i = 0; i < 97; ++i) osc.next(377.0 / kSampleRate); // run elsewhere.
        osc.reset(kCleanStartPhase);
        for (int i = 0; i < 512; ++i)
            REQUIRE(osc.next(1103.0 / kSampleRate) == first[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("VCO noise output stays bounded and finite at any depth",
          "[signal][osc][vco][noise]") {
    // A sine's amplitude is exactly the shape range and does not depend on
    // frequency, so any bound violation under noisy pitch is a real defect (not
    // polyBLEP overshoot). Depths far past musical range confirm the exp2 mapping
    // keeps the increment finite and positive.
    for (const double drift : {0.0, 20.0, 100.0, 1000.0}) {
        for (const double jitter : {0.0, 50.0, 300.0, 2000.0}) {
            VcoOscillator osc;
            osc.prepare(kSampleRate);
            osc.set_shape(VaShape::sine);
            osc.set_seed(4242);
            osc.set_drift_depth(drift);
            osc.set_jitter_depth(jitter);
            osc.reset(0.0);
            double pk = 0.0;
            bool finite = true;
            for (int i = 0; i < kRenderLength; ++i) {
                const double v = osc.next(1000.0 / kSampleRate);
                if (!std::isfinite(v)) finite = false;
                pk = std::fmax(pk, std::fabs(v));
            }
            INFO("drift " << drift << " cents, jitter " << jitter << " cents: peak " << pk);
            CHECK(finite);
            CHECK(pk <= 1.0 + 1e-9);
        }
    }
}
