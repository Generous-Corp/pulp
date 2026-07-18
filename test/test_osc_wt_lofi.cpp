// OSC-WT lo-fi tier: the pitch-tracking variable-clock ZOH image ladder that IS
// the lo-fi sound, measured against the analytic sinc model; the odd-harmonic
// 8-bit grit; the reconstruction stage that kills the naive in-band fold; and
// the faithful stepped wave-scan (the opposite of the modern tier's zipper-free
// scan). These are the measurable properties that justify a dedicated engine.
//
// ── Measurement discipline ─────────────────────────────────────────────────
//
// The image ladder is measured by least-squares TONE PROJECTION (`fit_tone`) at
// the known analytic image frequencies, not by an FFT: the sites are known in
// advance (`n·L·f0`), and projection has no leakage skirt, so a small image next
// to a 0 dB fundamental is read for what it is. Each image's level is compared
// to the closed-form variable-clock-ZOH sinc model:
//
//   a component of baseband line m replicated into the n-th ZOH image at
//   (nL ± m)·f0 has amplitude, relative to the fundamental, of
//   sinc(π·(nL±m)/L) / sinc(π·m/L)   [sinc(x) = sin(x)/x].
//
// For a pure-sine table only m = 1 exists, so the ladder is at (nL ± 1)·f0. That
// is the analytic reference `zoh_image_db` returns.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/analysis/audio_spectrum.hpp>
#include <pulp/signal/osc/wt.hpp>
#include <pulp/signal/osc/wt_lofi.hpp>
#include <pulp/signal/wavetable.hpp>

#include <cmath>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

using Catch::Matchers::WithinAbs;
using pulp::signal::Wavetable64;
using pulp::signal::osc::LofiWtOscillator;
using pulp::signal::osc::WtOscillator;
using pulp::test::audio::fit_tone;
using pulp::test::audio::fold_frequency;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr std::size_t kTableLength = 128; // L: pins the analytic ladder.
constexpr double kTwoPi = 2.0 * std::numbers::pi;

double sinc_ratio(double idx, double denom_idx, std::size_t L) {
    const double dl = static_cast<double>(L);
    auto sinc = [](double x) {
        return std::abs(x) < 1e-15 ? 1.0 : std::sin(x) / x;
    };
    return std::abs(sinc(std::numbers::pi * idx / dl) /
                    sinc(std::numbers::pi * denom_idx / dl));
}

// Analytic level (dBc) of the n-th ZOH image of baseband line m, sign = +1 for
// the upper sideband (nL + m)·f0 and -1 for the lower (nL - m)·f0.
double zoh_image_db(int n, int sign, int m, std::size_t L) {
    const double idx = static_cast<double>(n) * static_cast<double>(L) +
                       sign * static_cast<double>(m);
    return 20.0 * std::log10(sinc_ratio(idx, static_cast<double>(m), L));
}

// A raw single-cycle sine table of length L, peak 1.0.
std::vector<double> sine_table(std::size_t L) {
    std::vector<double> t(L);
    for (std::size_t k = 0; k < L; ++k)
        t[k] = std::sin(kTwoPi * static_cast<double>(k) / static_cast<double>(L));
    return t;
}

// A raw single-cycle table of harmonic h (period-L, so smooth across the wrap).
std::vector<double> harmonic_table(std::size_t L, int h) {
    std::vector<double> t(L);
    for (std::size_t k = 0; k < L; ++k)
        t[k] = std::sin(kTwoPi * static_cast<double>(h) * static_cast<double>(k) /
                        static_cast<double>(L));
    return t;
}

LofiWtOscillator make_lofi(std::vector<std::vector<double>> tables, int bit_depth,
                           bool reconstruction, int oversample = 16) {
    LofiWtOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_oversample_factor(oversample);
    osc.set_reconstruction(reconstruction);
    osc.set_tables(std::move(tables), bit_depth);
    return osc;
}

std::vector<double> render(LofiWtOscillator& osc, double f0, int length) {
    osc.reset();
    std::vector<double> out(static_cast<std::size_t>(length));
    const double increment = f0 / kSampleRate;
    for (int i = 0; i < length; ++i)
        out[static_cast<std::size_t>(i)] = osc.next(increment);
    return out;
}

// Fitted amplitude of a tone at `hz` over a settled window (skips FIR warmup).
double tone_amp(const std::vector<double>& x, double hz, int offset = 4096) {
    const std::size_t begin = static_cast<std::size_t>(offset);
    std::span<const double> seg(x.data() + begin, x.size() - begin);
    return fit_tone(seg, hz / kSampleRate).amplitude;
}

// dBc of a tone at `hz` relative to the fundamental at `f0`.
double tone_db(const std::vector<double>& x, double hz, double f0, int offset = 4096) {
    const double fund = tone_amp(x, f0, offset);
    const double a = tone_amp(x, hz, offset);
    return 20.0 * std::log10(a / fund);
}

double rms(const std::vector<double>& x) {
    double acc = 0.0;
    for (double v : x) acc += v * v;
    return std::sqrt(acc / static_cast<double>(x.size()));
}

double max_abs_step(const std::vector<double>& x, int begin, int end) {
    double m = 0.0;
    for (int i = begin + 1; i < end; ++i)
        m = std::fmax(m, std::fabs(x[static_cast<std::size_t>(i)] -
                                   x[static_cast<std::size_t>(i - 1)]));
    return m;
}

} // namespace

// ── The signature: the n·L·f0 image ladder matches the analytic ZOH model ───

TEST_CASE("OSC-WT lo-fi image ladder matches the analytic variable-clock model",
          "[signal][osc][wt][lofi]") {
    // A pure-sine table at full precision, so the ZOH images are the ONLY spuria
    // (no quantization grit to confuse the reading). f0 = 55 Hz places the first
    // two image pairs comfortably inside the reconstruction passband.
    const double f0 = 55.0;
    constexpr int kLength = 65536; // Long: fine fit resolution separates the pairs.

    LofiWtOscillator osc =
        make_lofi({sine_table(kTableLength)}, LofiWtOscillator::kFullPrecision,
                  /*reconstruction=*/true);
    const auto out = render(osc, f0, kLength);

    // Each rung of the ladder, measured vs the closed-form sinc model. The
    // reconstruction passband is flat here, so measured == analytic to a tight
    // tolerance, budgeted for the reconstruction filter's passband ripple.
    struct Rung { int n; int sign; };
    for (const Rung r : {Rung{1, -1}, Rung{1, +1}, Rung{2, -1}, Rung{2, +1}}) {
        const double hz = (static_cast<double>(r.n * static_cast<int>(kTableLength)) +
                           r.sign) * f0;
        const double predicted = zoh_image_db(r.n, r.sign, 1, kTableLength);
        const double measured = tone_db(out, hz, f0);
        INFO("image n=" << r.n << " sign=" << r.sign << " at " << hz << " Hz: measured "
             << measured << " dBc, predicted " << predicted << " dBc");
        CHECK_THAT(measured, WithinAbs(predicted, 0.25));
    }

    // Negative control: the MODERN tier, playing the SAME waveform (a sine),
    // produces NO image ladder — its interpolation + band-limiting remove it.
    WtOscillator modern;
    modern.set_wavetable_set({Wavetable64::make_sine(2048, kSampleRate)});
    modern.prepare(kSampleRate);
    modern.reset();
    std::vector<double> modern_out(static_cast<std::size_t>(kLength));
    const double inc = f0 / kSampleRate;
    for (int i = 0; i < kLength; ++i)
        modern_out[static_cast<std::size_t>(i)] = modern.next(inc);
    const double first_image_hz =
        (static_cast<double>(kTableLength) - 1.0) * f0; // (L-1)·f0.
    const double modern_image_db = tone_db(modern_out, first_image_hz, f0);
    const double lofi_image_db = zoh_image_db(1, -1, 1, kTableLength);
    INFO("modern tier at the (L-1)·f0 site: " << modern_image_db << " dBc (lo-fi is "
         << lofi_image_db << " dBc)");
    // The lo-fi image is ~-42 dBc; the modern tier has no image ladder there —
    // its residual sits far below (the reading is fundamental-leakage/fit-floor
    // limited, not a real image). Frame the gap relative to the lo-fi image so
    // the claim is "no comparable ladder," not an absolute floor.
    CHECK(modern_image_db < lofi_image_db - 30.0);
}

// ── The ladder tracks pitch — the whole reason it is a separate engine ──────

TEST_CASE("OSC-WT lo-fi image ladder tracks pitch", "[signal][osc][wt][lofi]") {
    constexpr int kLength = 65536;
    LofiWtOscillator osc =
        make_lofi({sine_table(kTableLength)}, LofiWtOscillator::kFullPrecision, true);

    const double f_lo = 55.0;
    const double f_hi = 82.5; // 1.5x — the image sites must scale by 1.5.
    const auto out_lo = render(osc, f_lo, kLength);
    const auto out_hi = render(osc, f_hi, kLength);

    const double L = static_cast<double>(kTableLength);
    const double site_lo = (L - 1.0) * f_lo; // 6985 Hz.
    const double site_hi = (L - 1.0) * f_hi; // 10477.5 Hz — the scaled site.

    // At f_hi the first image sits at (L-1)·f_hi, NOT at the f_lo site: the image
    // MOVED with the note. Measure the image at each render's own scaled site,
    // and confirm the high render has (almost) nothing at the low render's site.
    const double image_lo = tone_db(out_lo, site_lo, f_lo);
    const double image_hi = tone_db(out_hi, site_hi, f_hi);
    const double stale = tone_db(out_hi, site_lo, f_hi); // old site, wrong note.

    INFO("image tracks pitch: at f_lo the (L-1) image is " << image_lo
         << " dBc @ " << site_lo << " Hz; at f_hi it is " << image_hi << " dBc @ "
         << site_hi << " Hz; the f_lo site in the f_hi render is " << stale << " dBc");
    // Both renders show the image at THEIR scaled site, near the analytic level.
    const double predicted = zoh_image_db(1, -1, 1, kTableLength);
    CHECK_THAT(image_lo, WithinAbs(predicted, 0.25));
    CHECK_THAT(image_hi, WithinAbs(predicted, 0.25));
    // And the image is NOT at the old (unscaled) site once the note changes.
    CHECK(stale < image_hi - 30.0);
}

// ── 8-bit table quantization grit is ODD-harmonic ──────────────────────────

TEST_CASE("OSC-WT lo-fi 8-bit quantization grit is odd-harmonic",
          "[signal][osc][wt][lofi]") {
    const double f0 = 55.0;
    constexpr int kLength = 65536;

    // The aggregate quantization SNR is a property of the engine's STORED 8-bit
    // table: rms(table) / rms(quantization error). It is graded on the engine's
    // OWN stored data — read back via `stored_table` — not on a private
    // re-quantization in the test, so a drift in the engine's quantizer moves this
    // reading rather than passing vacuously against a stale expectation. This is
    // the ~49.9 dB ideal for a full-scale sine (6.02·8 + 1.76), the aggregate
    // figure spread across the odd harmonic lines — NOT any single line.
    LofiWtOscillator quantized = make_lofi({sine_table(kTableLength)}, 8, true);
    const auto stored = quantized.stored_table(0);
    REQUIRE(stored.size() == kTableLength);
    const auto reference = sine_table(kTableLength);
    std::vector<double> error(kTableLength);
    for (std::size_t k = 0; k < kTableLength; ++k)
        error[k] = stored[k] - reference[k];
    const double snr_db = 20.0 * std::log10(rms(reference) / rms(error));
    INFO("engine 8-bit stored-table SNR " << snr_db << " dB (ideal ~49.9)");
    CHECK_THAT(snr_db, WithinAbs(49.9, 2.0));

    // The grit's harmonic structure, measured as the DELTA between the quantized
    // render and the same wave at full precision. That difference is exactly the
    // quantization spuria — and it removes the 0 dB fundamental, so a projection at
    // a low harmonic is not swamped by the fundamental's own leakage. For a
    // symmetric table the quantizer preserves half-wave symmetry, so only ODD
    // harmonics appear: h3, h5 present; h2, h4 absent.
    LofiWtOscillator full =
        make_lofi({sine_table(kTableLength)}, LofiWtOscillator::kFullPrecision, true);
    const auto out_q = render(quantized, f0, kLength);
    const auto out_f = render(full, f0, kLength);
    std::vector<double> grit(static_cast<std::size_t>(kLength));
    for (std::size_t i = 0; i < grit.size(); ++i) grit[i] = out_q[i] - out_f[i];

    const double fund = tone_amp(out_f, f0);
    auto grit_db = [&](double hz) {
        return 20.0 * std::log10(tone_amp(grit, hz) / fund);
    };
    const double h2 = grit_db(2.0 * f0);
    const double h3 = grit_db(3.0 * f0);
    const double h4 = grit_db(4.0 * f0);
    const double h5 = grit_db(5.0 * f0);
    INFO("quantization spuria (delta vs full precision): h2=" << h2 << " h3=" << h3
         << " h4=" << h4 << " h5=" << h5 << " dBc");
    // Odd harmonics present (the grit) — around -70 dBc for an 8-bit table.
    CHECK(h3 > -90.0);
    CHECK(h3 < -55.0);
    CHECK(h5 > -90.0);
    CHECK(h5 < -55.0);
    // Even harmonics absent: a symmetric table cannot produce them.
    CHECK(h2 < h3 - 30.0);
    CHECK(h4 < h3 - 30.0);
}

// ── The reconstruction stage is real and load-bearing ──────────────────────

TEST_CASE("OSC-WT lo-fi reconstruction stage kills the naive in-band fold",
          "[signal][osc][wt][lofi]") {
    // Under naive point-sampling, the supra-Nyquist image (4L-1)·f0 folds back
    // in-band as an AUDIBLE spur — a modeling artifact, not the instrument's
    // sound. The oversample -> reconstruction-filter -> decimate reader removes
    // it. f0 = 55 Hz puts that fold near 19.9 kHz.
    const double f0 = 55.0;
    constexpr int kLength = 65536;
    const double image_hz = (4.0 * static_cast<double>(kTableLength) - 1.0) * f0;
    const double fold_hz = fold_frequency(image_hz, kSampleRate);

    LofiWtOscillator naive =
        make_lofi({sine_table(kTableLength)}, LofiWtOscillator::kFullPrecision,
                  /*reconstruction=*/false);
    LofiWtOscillator reconstructed =
        make_lofi({sine_table(kTableLength)}, LofiWtOscillator::kFullPrecision,
                  /*reconstruction=*/true);

    const double naive_db = tone_db(render(naive, f0, kLength), fold_hz, f0);
    const double recon_db = tone_db(render(reconstructed, f0, kLength), fold_hz, f0);

    INFO("(4L-1)·f0 = " << image_hz << " Hz folds to " << fold_hz
         << " Hz: naive " << naive_db << " dBc, reconstructed " << recon_db
         << " dBc (improvement " << naive_db - recon_db << " dB)");
    // Naive folds an audible in-band spur (~-54 dBc).
    CHECK(naive_db > -60.0);
    CHECK(naive_db < -45.0);
    // The reconstruction stage knocks it below -77 dBc — the proof it is real.
    CHECK(recon_db < -77.0);
    CHECK(naive_db - recon_db > 20.0);
}

// ── The wave-scan STEPS — the faithful lo-fi zipper ────────────────────────

TEST_CASE("OSC-WT lo-fi wave-scan steps between tables",
          "[signal][osc][wt][lofi]") {
    // A hard scan across two distinct raw tables. At the switch instant the
    // output steps from tableA[phase] to tableB[phase] with no crossfade and no
    // slew — the classic wavetable zipper, and a FEATURE here. The mode-aware
    // click gate must null this step against the intended stepped reference, not
    // flag it (contrast the modern tier, whose slewed scan is zipper-free).
    //
    // Measured on the raw (reconstruction-off) staircase so the step is a clean
    // sample-to-sample discontinuity; the reconstruction stage band-limits this
    // transient but does not remove it.
    const auto table_a = harmonic_table(kTableLength, 1); // sine.
    const auto table_b = harmonic_table(kTableLength, 3); // 3rd-harmonic wave.
    LofiWtOscillator osc =
        make_lofi({table_a, table_b}, LofiWtOscillator::kFullPrecision,
                  /*reconstruction=*/false);

    // Integer table-phase advance (increment·L = 1): each host sample steps the
    // ZOH by exactly one table index, so the switch lands on a known phase.
    const double f0 = kSampleRate / static_cast<double>(kTableLength); // 375 Hz.
    const double inc = f0 / kSampleRate;
    constexpr int kLength = 8192;
    // Switch at a host sample whose table index is L/4 (sine peak = +1, the
    // 3rd-harmonic wave = -1 there) so the inter-wave difference is maximal.
    const int kSwitch = 4096 + static_cast<int>(kTableLength) / 4;

    osc.reset();
    osc.set_scan(0.0);
    std::vector<double> out(static_cast<std::size_t>(kLength));
    for (int i = 0; i < kLength; ++i) {
        if (i == kSwitch) osc.set_scan(1.0);
        out[static_cast<std::size_t>(i)] = osc.next(inc);
    }

    // The bound: the largest sample-to-sample difference the scan step can
    // produce is the peak inter-wave difference. A faithful step is at or below
    // it; a step LARGER than it would be an inauthentic click.
    double inter_wave_bound = 0.0;
    for (std::size_t k = 0; k < kTableLength; ++k)
        inter_wave_bound = std::fmax(inter_wave_bound,
                                     std::fabs(table_b[k] - table_a[k]));

    const double scan_step = max_abs_step(out, kSwitch - 1, kSwitch + 2);
    const double steady_step = max_abs_step(out, 6000, kLength);
    INFO("scan step " << scan_step << " (bound " << inter_wave_bound
         << "), steady step " << steady_step);
    // The step is present and large — the OPPOSITE of the modern tier's
    // zipper-free assertion.
    CHECK(scan_step > steady_step * 5.0);
    // And bounded by the inter-wave difference — a genuine step, not a bigger
    // inauthentic click.
    CHECK(scan_step <= inter_wave_bound * 1.01);
    CHECK(scan_step > 1.5); // maximal-difference switch: ~2.
}

// ── A length-mismatched table set is rejected, never silently truncated ─────

TEST_CASE("OSC-WT lo-fi rejects a length-mismatched table set",
          "[signal][osc][wt][lofi]") {
    LofiWtOscillator osc;
    osc.prepare(kSampleRate);
    osc.set_reconstruction(false); // raw ZOH — the pitch reading is unfiltered.

    // A well-formed single-length set is accepted and installs cleanly.
    REQUIRE(osc.set_tables({sine_table(kTableLength)},
                           LofiWtOscillator::kFullPrecision));
    REQUIRE(osc.table_count() == 1);
    REQUIRE(osc.table_length() == kTableLength);

    // A set whose tables differ in length is REJECTED. Truncating the longer table
    // to the shorter length would play a fraction of its cycle as a full cycle — a
    // DC-laden, off-pitch waveform (a 2L sine truncated to L reads as a half-cycle:
    // DC ~0.64, half-amplitude fundamental). The call must be a no-op that returns
    // false and leaves the good set intact.
    const bool accepted =
        osc.set_tables({sine_table(kTableLength), sine_table(2 * kTableLength)},
                       LofiWtOscillator::kFullPrecision);
    CHECK_FALSE(accepted);
    CHECK(osc.table_count() == 1);            // the previous set survived untouched.
    CHECK(osc.table_length() == kTableLength);

    // And the surviving table still plays a clean single cycle: near-zero DC and a
    // unit fundamental — a full sine, not the corrupted half-cycle a silent
    // truncation would have installed.
    const double f0 = 200.0;
    constexpr int kLength = 16384;
    const auto out = render(osc, f0, kLength);
    double mean = 0.0;
    for (double v : out) mean += v;
    mean /= static_cast<double>(out.size());
    const double fundamental = tone_amp(out, f0, /*offset=*/0);
    INFO("surviving table: DC " << mean << ", fundamental amp " << fundamental
         << " (a silent truncation would read DC ~0.64, fundamental ~0.42)");
    CHECK(std::fabs(mean) < 0.05);
    CHECK_THAT(fundamental, WithinAbs(1.0, 0.1));
}

// ── Determinism ────────────────────────────────────────────────────────────

TEST_CASE("OSC-WT lo-fi is deterministic — same input twice is bit-identical",
          "[signal][osc][wt][lofi]") {
    auto build = []() {
        return make_lofi({sine_table(kTableLength), harmonic_table(kTableLength, 3)},
                         8, /*reconstruction=*/true);
    };
    auto run = [](LofiWtOscillator& osc) {
        osc.reset();
        std::vector<double> out(4096);
        const double inc = 220.0 / kSampleRate;
        for (int i = 0; i < 4096; ++i) {
            if (i == 1024) osc.set_scan(1.0);
            out[static_cast<std::size_t>(i)] = osc.next(inc);
        }
        return out;
    };
    LofiWtOscillator a = build();
    LofiWtOscillator b = build();
    const auto first = run(a);
    const auto second = run(b);
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i)
        REQUIRE(first[i] == second[i]);
}
