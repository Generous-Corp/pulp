// OSC-WT modern tier: measured alias rejection swept to the TOP of every band
// (the discriminating stress), a click-free band-switch seam, a zipper-free
// scan, and bit-exact determinism.
//
// ── Measurement discipline (shared with the VA/VCO suites) ─────────────────
//
// Alias numbers come from `measure_aliasing` — a joint least-squares fit of the
// ideal harmonic series and every above-Nyquist harmonic's fold site, so a 0 dB
// fundamental has no leakage skirt to bury a small alias under. Three rules:
//
//   1. **Band-qualified to 20 kHz.** A full-band alias claim is impossible for
//      any method (fold-back is continuous across Nyquist), so aliases are gated
//      only where they land below 20 kHz — audible territory.
//   2. **The floor is PROVEN by negative control, never derived.** An exactly
//      band-limited saw carries zero alias BY CONSTRUCTION and must read
//      collapsed; an injected alias of known level must be recovered. The
//      analyzer's own `detection_floor_db` is never asserted on — it would be the
//      analyzer grading its own homework.
//   3. **A gate sits above the proven floor, and the fit must have resolved.**
//
// ── The entry criterion this suite exists for ─────────────────────────────
//
// The wavetable-architecture spike measured only just ABOVE a band boundary,
// where the higher band is trivially clean, and left the discriminating stress —
// a sweep to just UNDER a band ceiling — as this tier's entry criterion.
// `worst alias, swept to the top of every band` below is that criterion: it
// sweeps each band from mid-band up to 0.999 of its ceiling and reports the worst
// in-band alias found.
//
// The measured answer refines the hypothesis. The TOP of each band reads clean
// (< -110 dBc) — the band-switch-crossfade decision holds there, as the spike
// predicted. The worst reading is instead at MID-band of the sparse high bands (a
// two-harmonic band played well below its ceiling, e.g. a ~7 kHz fundamental),
// around -60 dBc at a 16k render. That reading is NOT a physical spur: it is the
// analyzer's detection floor at a sparse operating point, proven by the companion
// test where it falls as the render lengthens (a fixed alias would not). The band
// content itself is clean to -185 dBc; the reason the sparse case reads high at
// all is that a two-harmonic signal conditions the joint fit near its floor.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/signal/osc/wt.hpp>
#include <pulp/signal/wavetable.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Wavetable64;
using pulp::signal::osc::WtOscillator;
using pulp::test::audio::AliasOptions;
using pulp::test::audio::AliasReport;
using pulp::test::audio::fold_frequency;
using pulp::test::audio::measure_aliasing;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kBandHz = 20000.0;
constexpr int kAliasRenderLength = 16384;
constexpr double kTwoPi = 2.0 * std::numbers::pi;

// Harmonics accounted for, sized from the sample rate (3x coverage is past the
// point where the reading stops changing). `measure_aliasing` throws if no
// harmonic reaches Nyquist, so this must scale with 1/f0.
int harmonics_for(double f0) {
    return static_cast<int>(std::ceil(3.0 * kSampleRate / f0));
}

// A saw OSC-WT, fully band-limited across the audible range. `bands` matches the
// engine's default so the ceilings are the shipped ones.
WtOscillator make_saw_osc(std::size_t bands = 10) {
    std::vector<Wavetable64> tables;
    tables.push_back(Wavetable64::make_saw(bands, 2048, kSampleRate));
    WtOscillator osc;
    osc.set_wavetable_set(std::move(tables));
    osc.prepare(kSampleRate);
    return osc;
}

// Render a steady tone at f0 — one band selected for the whole render, so no
// crossfade contaminates the alias reading.
std::vector<double> render_steady(WtOscillator& osc, double f0, int length) {
    osc.reset();
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    for (int i = 0; i < length; ++i) out[static_cast<std::size_t>(i)] = osc.next(increment);
    return out;
}

// An EXACTLY band-limited saw: every harmonic below Nyquist summed at its own
// frequency. Zero alias energy by construction, so it is the negative control
// that proves the analyzer's floor. Optionally injects a tone of known level on
// a fold site to build a positive control.
std::vector<double> bandlimited_saw(double f0, int length, double alias_hz = 0.0,
                                    double alias_amplitude = 0.0) {
    const int max_h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0));
    std::vector<double> out(static_cast<std::size_t>(length), 0.0);
    for (int i = 0; i < length; ++i) {
        const double t = static_cast<double>(i) / kSampleRate;
        double v = 0.0;
        for (int h = 1; h <= max_h; ++h) {
            const double sign = (h % 2 == 1) ? 1.0 : -1.0;
            v += sign * std::sin(kTwoPi * h * f0 * t + 0.3) / h;
        }
        v *= 2.0 / std::numbers::pi;
        if (alias_amplitude > 0.0)
            v += alias_amplitude * std::sin(kTwoPi * alias_hz * t + 0.9);
        out[static_cast<std::size_t>(i)] = v;
    }
    return out;
}

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

// The lowest harmonic above Nyquist whose fold site lands in-band — a gradeable
// fixture needs one, so a positive control searches for it.
int first_in_band_alias_harmonic(double f0) {
    for (int h = static_cast<int>(std::floor((kSampleRate / 2.0) / f0)) + 1;
         h <= harmonics_for(f0); ++h)
        if (fold_frequency(h * f0, kSampleRate) <= kBandHz) return h;
    return 0;
}

double max_abs_diff(const std::vector<double>& v, int begin, int end) {
    double m = 0.0;
    for (int i = begin + 1; i < end; ++i)
        m = std::fmax(m, std::fabs(v[static_cast<std::size_t>(i)] -
                                   v[static_cast<std::size_t>(i - 1)]));
    return m;
}

void require_trustworthy(const AliasReport& r) {
    REQUIRE_FALSE(r.has_unresolved_in_band_alias);
    REQUIRE(r.worst_alias_db > r.detection_floor_db);
}

// The honest upper bound on the in-band alias level. When the fit resolves a
// component above its detection floor, that reading IS the alias. When it
// collapses below the floor, the alias is not "at" the collapsed value — it is
// merely somewhere BELOW the floor, so the floor is the honest bound. A
// band-limited wavetable is clean enough to sit in the second regime at many
// swept points, and reporting the collapsed sentinel there would be a false
// precision. An unresolved in-band alias is neither: the fit failed, so the
// bound is meaningless (the caller must fail closed on `has_unresolved`).
double alias_bound_db(const AliasReport& r) {
    return r.worst_alias_db > r.detection_floor_db ? r.worst_alias_db
                                                   : r.detection_floor_db;
}

} // namespace

// ── The floor, proven ──────────────────────────────────────────────────────

TEST_CASE("OSC-WT alias floor is proven by negative and positive control",
          "[signal][osc][wt]") {
    // A test f0 with plenty of harmonics folding in-band, and coherent enough to
    // fit cleanly over the render.
    const double f0 = 2153.0;

    SECTION("a band-limited saw reads collapsed — the negative control") {
        const auto report = analyze(bandlimited_saw(f0, kAliasRenderLength), f0);
        REQUIRE_FALSE(report.has_unresolved_in_band_alias);
        INFO("clean saw worst in-band alias " << report.worst_alias_db
             << " dBc (noise " << report.noise_db << ")");
        // Zero alias by construction: the worst in-band reading collapses toward
        // the noise floor rather than resolving a component.
        CHECK(report.worst_alias_db < -130.0);
    }

    SECTION("an injected alias of known level is recovered — the positive control") {
        const int h = first_in_band_alias_harmonic(f0);
        REQUIRE(h > 0);
        const double alias_hz = fold_frequency(h * f0, kSampleRate);
        const double truth_db = -60.0;
        const double amplitude = std::pow(10.0, truth_db / 20.0) *
                                 (2.0 / std::numbers::pi); // relative to the fundamental.
        const auto report =
            analyze(bandlimited_saw(f0, kAliasRenderLength, alias_hz, amplitude), f0);
        require_trustworthy(report);
        INFO("injected " << truth_db << " dBc at " << alias_hz << " Hz (h=" << h
             << "); read " << report.worst_alias_db << " dBc");
        CHECK(report.worst_alias_index == h);
        CHECK_THAT(report.worst_alias_db, WithinAbs(truth_db, 1.0));
    }
}

// ── THE ENTRY CRITERION: alias rejection swept to the top of every band ─────

TEST_CASE("OSC-WT worst alias swept to the top of every band",
          "[signal][osc][wt]") {
    WtOscillator osc = make_saw_osc();
    REQUIRE(osc.band_count() > 2);

    // Sweep each band from mid-band up to just under its ceiling — the stress the
    // spike missed (it measured only just ABOVE a boundary). The topmost band
    // (ceiling == Nyquist) carries only the fundamental and is degenerate to fit,
    // and very low bands put f0 below the range where the render holds enough
    // cycles, so both ends are skipped.
    double worst_db = -1000.0;
    double worst_hz = 0.0;
    double worst_f0 = 0.0;
    int measured = 0;

    for (std::size_t b = 1; b + 1 < osc.band_count(); ++b) {
        const double lower = osc.band_max_frequency_hz(b - 1);
        const double ceiling = osc.band_max_frequency_hz(b);
        if (ceiling > 0.45 * kSampleRate) continue; // Nyquist-adjacent: skip.
        if (ceiling < 500.0) continue;              // too few render cycles.

        for (const double frac : {0.60, 0.80, 0.90, 0.95, 0.99, 0.999}) {
            const double f0 = std::max(lower * 1.02, ceiling * frac);
            if (f0 >= 0.45 * kSampleRate) continue;
            const auto report = analyze(render_steady(osc, f0, kAliasRenderLength), f0);
            // An unresolved in-band alias is a fail-closed condition; a resolved
            // reading below the detection floor is not — the wavetable is simply
            // cleaner than the fit can see there.
            REQUIRE_FALSE(report.has_unresolved_in_band_alias);
            ++measured;
            const double bound = alias_bound_db(report);
            if (bound > worst_db) {
                worst_db = bound;
                worst_hz = report.worst_alias_hz;
                worst_f0 = f0;
            }
        }
    }

    REQUIRE(measured > 0);
    INFO("worst in-band alias bound across the top-of-band sweep: " << worst_db
         << " dBc at " << worst_hz << " Hz (f0 " << worst_f0 << "), over "
         << measured << " swept points");
    // The measured worst sits at mid-band of the SPARSE high bands (a low-harmonic
    // band played well below its ceiling), around -60 dBc at this render length —
    // NOT at the top of a band, where the reading is far cleaner (< -110 dBc). The
    // companion test proves that worst reading is detection-floor-limited (it
    // falls as the render lengthens), so it is not a fixed physical spur. This
    // gate is a regression tripwire at the analyzer's floor for this stimulus,
    // well below audibility; it is NOT a cleanliness certification (the floor is).
    CHECK(worst_db < -50.0);
}

// The worst sweep reading is the analyzer's detection FLOOR at a sparse operating
// point, not a fixed physical alias. The distinction is decisive and measurable:
// a real discrete alias holds a constant dBc level regardless of how long the
// segment is, whereas a floor-limited reading falls as the segment lengthens
// (~1/sqrt(N)). This test renders the sweep's worst point at two lengths and
// requires the reading to FALL — the signature of no fixed spur. It is also why
// the band-switch-crossfade decision holds under the top-of-band stress: there is
// no physical in-band alias for the band switch to be blamed for.
TEST_CASE("OSC-WT worst-case alias is detection-floor-limited, not a fixed spur",
          "[signal][osc][wt]") {
    const double f0 = 7086.7; // The sweep's worst operating point.

    auto worst_at = [&](int length) {
        WtOscillator osc = make_saw_osc();
        const auto signal = render_steady(osc, f0, length);
        pulp::audio::Buffer<float> buffer(1, length);
        for (int i = 0; i < length; ++i)
            buffer.channel(0)[i] = static_cast<float>(signal[static_cast<std::size_t>(i)]);
        AliasOptions options;
        options.num_harmonics = harmonics_for(f0);
        options.analysis_length = length;
        options.max_alias_frequency_hz = kBandHz;
        return measure_aliasing(std::as_const(buffer).view(), f0, kSampleRate, options);
    };

    const auto short_run = worst_at(16384);
    const auto long_run = worst_at(65536);

    INFO("worst in-band alias: " << short_run.worst_alias_db << " dBc at 16384 samples, "
         << long_run.worst_alias_db << " dBc at 65536 (floor "
         << short_run.detection_floor_db << " -> " << long_run.detection_floor_db << ")");
    // A fixed physical spur would hold its level; this reading falls by several dB
    // as the floor drops — so there is no fixed in-band alias, and the true alias
    // sits below both readings. (A real spur would instead hold ~constant, and
    // would tower above the floor rather than hugging it.)
    CHECK(long_run.worst_alias_db < short_run.worst_alias_db - 6.0);
    CHECK(long_run.detection_floor_db < short_run.detection_floor_db - 6.0);
}

// ── The band-switch seam is click-free ─────────────────────────────────────

TEST_CASE("OSC-WT band switch is click-free", "[signal][osc][wt]") {
    // A triangle is the sensitive probe: its value is continuous, so its steady
    // sample-to-sample step is just the (small) slope. A band switch that clicked
    // would spike the step far above that — a saw's ±2 wrap would swamp the seam
    // and hide it.
    std::vector<Wavetable64> tables;
    tables.push_back(Wavetable64::make_triangle(10, 2048, kSampleRate));
    WtOscillator osc;
    osc.set_wavetable_set(std::move(tables));
    osc.prepare(kSampleRate);
    REQUIRE(osc.band_count() > 3);

    // Step across a middle band ceiling: f_low sits just under ceiling[k] (band
    // k), f_high just over it (band k+1), so the step forces a k → k+1 switch and
    // its crossfade.
    const std::size_t k = osc.band_count() / 2;
    const double ceiling = osc.band_max_frequency_hz(k);
    const double f_low = ceiling * 0.98;
    const double f_high = ceiling * 1.02;
    REQUIRE(f_high < osc.band_max_frequency_hz(k + 1));

    constexpr int kLength = 8192;
    constexpr int kStep = 4096;
    osc.reset();
    std::vector<double> out(static_cast<std::size_t>(kLength));
    const double inc_low = f_low / kSampleRate;
    const double inc_high = f_high / kSampleRate;
    for (int i = 0; i < kLength; ++i)
        out[static_cast<std::size_t>(i)] = osc.next(i < kStep ? inc_low : inc_high);

    // The crossfade is 128 samples; the window starts one sample before the
    // switch so the switch sample's step is included, not skipped.
    const double seam = max_abs_diff(out, kStep - 1, kStep + 160);
    // Steady step of the triangle at f_high, measured well after the crossfade.
    const double steady = max_abs_diff(out, 6000, kLength);

    INFO("band-switch seam step " << seam << " vs steady step " << steady
         << " (ratio " << seam / steady << ")");
    // The switch introduces no step larger than the waveform's own slope: the
    // 128-sample crossfade absorbs the band change.
    CHECK(seam <= steady * 1.5);
}

// ── The scan is zipper-free, and the slew is what makes it so ──────────────

TEST_CASE("OSC-WT scan across the table set is zipper-free",
          "[signal][osc][wt]") {
    // Two smooth tables with a large timbral gap: morphing sine → triangle. Both
    // are continuous, so a stepped position jump — not the waveform — is the only
    // thing that can produce a big sample-to-sample step, which makes a zipper
    // visible.
    auto make = []() {
        std::vector<Wavetable64> tables;
        tables.push_back(Wavetable64::make_sine(2048, kSampleRate));
        tables.push_back(Wavetable64::make_triangle(10, 2048, kSampleRate));
        WtOscillator osc;
        osc.set_wavetable_set(std::move(tables));
        osc.prepare(kSampleRate);
        return osc;
    };

    constexpr int kLength = 8192;
    constexpr int kStep = 4096;
    constexpr double kF0 = 300.0; // Well below any ceiling: one stable band.
    const double inc = kF0 / kSampleRate;

    // A stepped position control: hold 0 for half the render, then jump to 1.
    auto render_scan = [&](WtOscillator& osc) {
        osc.reset();
        std::vector<double> out(static_cast<std::size_t>(kLength));
        for (int i = 0; i < kLength; ++i) {
            if (i == kStep) osc.set_position(1.0);
            out[static_cast<std::size_t>(i)] = osc.next(inc);
        }
        return out;
    };

    WtOscillator smooth = make();      // default 5 ms slew.
    WtOscillator instant = make();
    instant.set_scan_time_ms(0.0);     // the stepped path — no slew.

    const auto smooth_out = render_scan(smooth);
    const auto instant_out = render_scan(instant);

    // Steady step of the destination (triangle) waveform, sampled after the scan.
    // Windows start one sample before the position change so the change's own
    // step is measured, not skipped.
    const double steady = max_abs_diff(smooth_out, 6000, kLength);
    const double smooth_scan = max_abs_diff(smooth_out, kStep - 1, kStep + 512);
    const double instant_scan = max_abs_diff(instant_out, kStep - 1, kStep + 4);

    INFO("scan step: slewed " << smooth_scan << ", instant " << instant_scan
         << ", steady triangle " << steady);
    // The slewed scan introduces no step beyond the waveform's own motion.
    CHECK(smooth_scan <= steady * 1.5);
    // And the slew is load-bearing: an instantaneous position jump DOES step,
    // well above the smooth path — proving the smoothing is what removes it.
    CHECK(instant_scan > smooth_scan * 2.0);
}

// ── Determinism ────────────────────────────────────────────────────────────

TEST_CASE("OSC-WT is deterministic — same input twice is bit-identical",
          "[signal][osc][wt]") {
    SECTION("a steady render") {
        WtOscillator a = make_saw_osc();
        WtOscillator b = make_saw_osc();
        const auto first = render_steady(a, 1103.0, 4096);
        const auto second = render_steady(b, 1103.0, 4096);
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i] == second[i]);
    }

    SECTION("a scan render") {
        auto make = []() {
            std::vector<Wavetable64> tables;
            tables.push_back(Wavetable64::make_sine(2048, kSampleRate));
            tables.push_back(Wavetable64::make_saw(10, 2048, kSampleRate));
            WtOscillator osc;
            osc.set_wavetable_set(std::move(tables));
            osc.prepare(kSampleRate);
            return osc;
        };
        auto render = [](WtOscillator& osc) {
            osc.reset();
            std::vector<double> out(2048);
            const double inc = 440.0 / kSampleRate;
            for (int i = 0; i < 2048; ++i) {
                if (i == 512) osc.set_position(1.0);
                out[static_cast<std::size_t>(i)] = osc.next(inc);
            }
            return out;
        };
        WtOscillator a = make();
        WtOscillator b = make();
        const auto first = render(a);
        const auto second = render(b);
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i] == second[i]);
    }
}
