#include <catch2/catch_test_macros.hpp>

#include "support/modal_analysis.hpp"

#include <pulp/signal/modal_bank.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <vector>

// Calibration of the modal analyzers against synthetic signals whose ground
// truth is prescribed, not fitted. An analyzer that has only ever been run on
// real data and agreed with expectations has not been calibrated — it has been
// confirmed. These tests construct signals where the right answer is known in
// closed form and assert the analyzer recovers it, including the controls that
// prove it does NOT recover an answer that is not there.

using namespace pulp::test::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

/// Sum of prescribed exponentially-decaying sinusoids, sampled exactly as
/// ModalBankT renders them: y[n] = sum_m gain_m * r_m^n * sin((n+1) * w_m).
std::vector<float> synth_modes(const std::vector<pulp::signal::ModalMode>& modes,
                               std::size_t n, double fs) {
    std::vector<float> x(n, 0.0f);
    for (const auto& m : modes) {
        const double r = std::pow(10.0, -3.0 / (m.t60_s * fs));
        const double w = 2.0 * kPi * m.freq_hz / fs;
        double rn = 1.0;
        for (std::size_t i = 0; i < n; ++i) {
            x[i] += static_cast<float>(m.gain * rn *
                                       std::sin((static_cast<double>(i) + 1.0) * w));
            rn *= r;
        }
    }
    return x;
}

/// A decaying sinusoid whose instantaneous frequency follows `freq_at(t)` and
/// whose envelope decays at a rate given by `sigma_at(t)`, padded with silence
/// so onset detection has something to find. Phase is integrated so the
/// prescribed frequency is the true instantaneous frequency, not an
/// approximation.
template <typename FreqFn, typename SigmaFn>
std::vector<float> synth_glide(FreqFn freq_at, SigmaFn sigma_at, double seconds,
                               double fs, double pad_s = 0.010) {
    const auto pad = static_cast<std::size_t>(pad_s * fs);
    const auto n = static_cast<std::size_t>(seconds * fs);
    std::vector<float> x(pad + n, 0.0f);
    double phase = 0.0;
    double log_env = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        x[pad + i] = static_cast<float>(std::exp(log_env) * std::sin(phase));
        phase += 2.0 * kPi * freq_at(t) / fs;
        log_env -= sigma_at(t) / fs;
    }
    return x;
}

std::vector<float> render_modal_bank_ir(
    const std::vector<pulp::signal::ModalMode>& modes, std::size_t n, double fs) {
    pulp::signal::ModalBank bank;
    bank.prepare(fs, static_cast<int>(modes.size()));
    bank.set_modes(modes);
    std::vector<float> in(n, 0.0f), out(n, 0.0f);
    in[0] = 1.0f;
    constexpr int block = 512;
    for (std::size_t i = 0; i < n; i += block) {
        const int b = static_cast<int>(std::min<std::size_t>(block, n - i));
        bank.process_add(in.data() + i, out.data() + i, b);
    }
    return out;
}

double cents(double measured, double reference) {
    return 1200.0 * std::log2(measured / reference);
}

/// Find the analyzed mode nearest `hz`, or nullptr.
const MeasuredMode* nearest(const ModeAnalysis& a, double hz) {
    const MeasuredMode* best = nullptr;
    double best_d = 1e30;
    for (const auto& m : a.modes) {
        const double d = std::abs(m.freq_hz - hz);
        if (d < best_d) {
            best_d = d;
            best = &m;
        }
    }
    return best;
}

} // namespace

// ── Calibration: known modes in, known modes out ──────────────────────────

TEST_CASE("analyze_modes recovers prescribed frequency, T60 and amplitude",
          "[modal-analysis][calibration]") {
    // Inharmonic, well-separated, spanning the band. Ground truth is the
    // closed-form synthesis above — no processor in the loop, so a failure
    // here is the analyzer's and nothing else's.
    const std::vector<pulp::signal::ModalMode> truth = {
        {110.0f, 2.0f, 1.0f},
        {251.3f, 1.4f, 0.7f},
        {829.7f, 0.9f, 0.5f},
        {2113.1f, 0.55f, 0.4f},
        {5527.9f, 0.3f, 0.3f},
    };
    const auto x = synth_modes(truth, static_cast<std::size_t>(2.0 * kFs), kFs);

    const auto analysis = analyze_modes(x, kFs);
    INFO(summarize(analysis));
    REQUIRE(analysis.ok);
    REQUIRE(analysis.modes.size() == truth.size());

    for (const auto& t : truth) {
        const auto* m = nearest(analysis, t.freq_hz);
        REQUIRE(m != nullptr);
        const double dc = cents(m->freq_hz, t.freq_hz);
        const double t60_err = (m->t60_s - t.t60_s) / t.t60_s;
        const double amp_err = (m->amplitude - t.gain) / t.gain;
        INFO("mode " << t.freq_hz << " Hz: " << summarize(*m) << " | "
                     << dc << " cents, T60 " << t60_err * 100.0 << "%, amp "
                     << amp_err * 100.0 << "%");
        CHECK(std::abs(dc) < 5.0);
        CHECK(std::abs(t60_err) < 0.05);
        CHECK(std::abs(amp_err) < 0.15);
        CHECK(m->confidence > 0.95);
    }
}

TEST_CASE("measure_mode reads ModalBank gain back as amplitude",
          "[modal-analysis][calibration]") {
    // The property worth protecting: ModalMode::gain is the mode's amplitude
    // in the rendered impulse response, so the number you set and the number
    // the harness measures are the same number. This test is what keeps that
    // true across changes to either side.
    const std::vector<pulp::signal::ModalMode> truth = {
        {440.0f, 1.2f, 0.25f},
        {1237.0f, 0.8f, 0.9f},
    };
    const auto ir = render_modal_bank_ir(truth, static_cast<std::size_t>(1.5 * kFs), kFs);

    for (const auto& t : truth) {
        const auto m = measure_mode(ir, kFs, t.freq_hz * 1.002); // deliberately off
        INFO("prescribed " << t.freq_hz << " Hz gain " << t.gain << " -> "
                           << summarize(m));
        CHECK(std::abs(cents(m.freq_hz, t.freq_hz)) < 5.0);
        CHECK(std::abs((m.t60_s - t.t60_s) / t.t60_s) < 0.05);
        CHECK(std::abs((m.amplitude - t.gain) / t.gain) < 0.15);
    }
}

TEST_CASE("T60 fit stays in its dB window and does not chase the noise floor",
          "[modal-analysis][calibration]") {
    // The failure this guards: a detector that hunts for the -60 dB crossing
    // latches onto whatever is at the floor and reports a confident, wrong,
    // and typically identical T60 for every mode. Here a fast mode is buried
    // under broadband noise well before it would reach -60 dB. The fit window
    // ends at -30 dB, above the floor, so the answer must still be right.
    const std::vector<pulp::signal::ModalMode> truth = {{500.0f, 0.4f, 1.0f}};
    auto x = synth_modes(truth, static_cast<std::size_t>(2.0 * kFs), kFs);

    std::mt19937 rng(0xC0FFEE);
    std::normal_distribution<float> noise(0.0f, 1.0e-3f); // ~-60 dBFS floor
    for (auto& v : x) v += noise(rng);

    const auto m = measure_mode(x, kFs, 500.0);
    INFO(summarize(m));
    CHECK(std::abs((m.t60_s - 0.4) / 0.4) < 0.08);
    CHECK(m.confidence > 0.99);

    // Control: push the fit window down until it genuinely reaches the floor,
    // and the documented failure must appear. Measured margin, by sweeping
    // fit_end_db on this exact signal: the fit is accurate to <1% down to
    // -70 dB, ~3% at -80/-90 dB, and only breaks at -100 dB (+132%) and
    // -110 dB (+406%). The narrowband probe's processing gain over a
    // broadband floor is what buys that margin, so the default -30 dB window
    // sits ~70 dB clear of the cliff.
    //
    // The tell is not merely a wrong number: at -110 dB and below the fit
    // returns the SAME T60 regardless of the window, because it is fitting the
    // slope of the noise floor rather than the mode. That is the "confident,
    // wrong, and identical for every mode" signature the dB window exists to
    // prevent.
    ModeAnalysisOptions chase;
    chase.fit_start_db = -3.0;
    chase.fit_end_db = -110.0;
    const auto chased = measure_mode(x, kFs, 500.0, chase);
    INFO("windowed: " << summarize(m) << "\nchased:   " << summarize(chased));
    CHECK(std::abs((chased.t60_s - 0.4) / 0.4) > 1.0);
    // ...and the analyzer must SAY so rather than report it as a measurement.
    CHECK(chased.confidence < 0.7);
    CHECK_FALSE(chased.note.empty());

    ModeAnalysisOptions deeper = chase;
    deeper.fit_end_db = -140.0;
    const auto chased_deeper = measure_mode(x, kFs, 500.0, deeper);
    INFO("chased deeper: " << summarize(chased_deeper));
    CHECK(chased_deeper.t60_s == chased.t60_s); // floor-latched, window-independent
}

TEST_CASE("analyze_modes reports its resolution limit and merges closer modes",
          "[modal-analysis][calibration]") {
    // Honesty about what cannot be resolved. Two modes inside one Hann main
    // lobe are one peak; the analyzer must report one mode and say what its
    // separation limit was, not invent two.
    ModeAnalysisOptions o;
    o.fft_length = 8192; // bin 5.86 Hz, main lobe ~23.4 Hz
    const double limit = 4.0 * kFs / o.fft_length;

    const std::vector<pulp::signal::ModalMode> close = {
        {400.0f, 1.5f, 1.0f},
        {static_cast<float>(400.0 + 0.3 * limit), 1.5f, 1.0f},
    };
    const auto x = synth_modes(close, static_cast<std::size_t>(2.0 * kFs), kFs);
    const auto a = analyze_modes(x, kFs, o);
    INFO(summarize(a));
    CHECK(a.resolution_limit_hz == limit);
    CHECK(a.modes.size() == 1);

    const std::vector<pulp::signal::ModalMode> apart = {
        {400.0f, 1.5f, 1.0f},
        {static_cast<float>(400.0 + 4.0 * limit), 1.5f, 1.0f},
    };
    const auto x2 = synth_modes(apart, static_cast<std::size_t>(2.0 * kFs), kFs);
    const auto a2 = analyze_modes(x2, kFs, o);
    INFO(summarize(a2));
    CHECK(a2.modes.size() == 2);
}

TEST_CASE("analyze_modes refuses unusable buffers instead of returning a number",
          "[modal-analysis][calibration]") {
    const std::vector<float> zeros(1024, 0.0f);
    const auto z = analyze_modes(zeros, kFs);
    INFO(z.message);
    CHECK_FALSE(z.ok);
    CHECK(z.modes.empty());
    CHECK_FALSE(z.message.empty());

    std::vector<float> nans(1024, 0.0f);
    nans[10] = std::numeric_limits<float>::quiet_NaN();
    const auto n = analyze_modes(nans, kFs);
    INFO(n.message);
    CHECK_FALSE(n.ok);

    const auto e = analyze_modes({}, kFs);
    CHECK_FALSE(e.ok);
}

// ── Calibration: inharmonicity ────────────────────────────────────────────

TEST_CASE("measure_inharmonicity recovers a prescribed stiff-string B",
          "[modal-analysis][calibration]") {
    // Ground truth: f_n = n*f0*sqrt(1 + B n^2) with B prescribed. A piano-like
    // B of 4e-4 pushes partial 12 about 60 cents sharp — far enough that a
    // tracker predicting n*f0 would miss it, which is why the search follows
    // the running B fit.
    const double f0 = 130.81;
    const double b_true = 4.0e-4;
    const int num = 12;

    std::vector<pulp::signal::ModalMode> modes;
    for (int n = 1; n <= num; ++n) {
        const double f = n * f0 * std::sqrt(1.0 + b_true * n * n);
        modes.push_back({static_cast<float>(f), 1.5f, static_cast<float>(1.0 / n)});
    }
    const auto x = synth_modes(modes, static_cast<std::size_t>(2.0 * kFs), kFs);

    InharmonicityOptions o;
    o.num_partials = num;
    const auto r = measure_inharmonicity(x, kFs, f0 * 1.003, o);
    INFO(summarize(r));
    REQUIRE(r.ok);
    CHECK(r.found_partials == num);
    CHECK(std::abs(cents(r.f0_hz, f0)) < 5.0);
    CHECK(std::abs((r.b_coefficient - b_true) / b_true) < 0.10);
    // The stiff-string model explains this signal; the pure harmonic series
    // does not. If that ordering ever flips, B is not measuring what it claims.
    CHECK(r.rms_deviation_cents < 3.0);
    CHECK(r.rms_harmonic_deviation_cents > 10.0 * r.rms_deviation_cents);
}

TEST_CASE("measure_inharmonicity reports B near zero for an ideal harmonic series",
          "[modal-analysis][calibration]") {
    // The control for the test above: given a perfectly harmonic signal the
    // analyzer must NOT find inharmonicity. An estimator that always returns a
    // small positive B would have passed the stiff-string test on bias alone.
    const double f0 = 220.0;
    std::vector<pulp::signal::ModalMode> modes;
    for (int n = 1; n <= 10; ++n)
        modes.push_back({static_cast<float>(n * f0), 1.5f,
                         static_cast<float>(1.0 / n)});
    const auto x = synth_modes(modes, static_cast<std::size_t>(2.0 * kFs), kFs);

    InharmonicityOptions o;
    o.num_partials = 10;
    const auto r = measure_inharmonicity(x, kFs, f0, o);
    INFO(summarize(r));
    REQUIRE(r.ok);
    CHECK(std::abs(r.b_coefficient) < 1.0e-5);
    CHECK(r.rms_harmonic_deviation_cents < 3.0);
}

// ── Calibration: the control that makes sigma(t)/Q(t) usable ──────────────

TEST_CASE("track_cycles does not invent sigma variation from a frequency glide",
          "[modal-analysis][calibration]") {
    // THE load-bearing control. A resonator whose decay rate co-varies with
    // its own state is a real and interesting finding; an estimator that
    // manufactures that co-variation out of a frequency glide would produce
    // the finding whether or not it is there. Prescribe a deep, slow sigh
    // (59.7 -> 47.65 Hz) with sigma held EXACTLY constant, and require the
    // recovered sigma to be flat.
    //
    // The glide is deliberately slower than a real 808 sigh: sigma only
    // becomes valid three cycles into the track (the sliding fit needs a full
    // window), so a fast sigh is nearly over before the estimator can be
    // watched doing the wrong thing. A control has to be observable in the
    // region where the estimator actually reports.
    const double sigma_true = 7.0;
    const auto x = synth_glide(
        [](double t) { return 47.65 + 12.0 * std::exp(-t / 0.150); },
        [sigma_true](double) { return sigma_true; }, 1.2, kFs);

    CycleTrackOptions o;
    o.start_offset_s = 0.005; // synthetic: no excitation transient to clear
    const auto track = track_cycles(x, kFs, o);
    INFO(summarize(track));
    REQUIRE(track.ok);
    CHECK(track.monocomponent_confidence > 0.95);

    const double t0 = track.cycles.front().time_s;
    const double mean_sigma = track.mean_sigma(t0, t0 + 0.30);
    const double sigma_span = track.sigma_span_percent(t0, t0 + 0.30);
    const double f0_span = track.f0_span_percent(t0, t0 + 0.30);

    INFO("prescribed sigma " << sigma_true << " -> recovered mean " << mean_sigma
                             << " Np/s, span " << sigma_span << "% while f0 spans "
                             << f0_span << "%");
    // The glide must actually be present in the reported region, or this
    // control proves nothing at all.
    CHECK(f0_span > 5.0);
    // ...and sigma must be flat through it. The ratio of these two spans is
    // the calibration result: the estimator sees the frequency move by more
    // than an order of magnitude more than it moves sigma.
    CHECK(std::abs(mean_sigma - sigma_true) / sigma_true < 0.02);
    CHECK(sigma_span < 1.0);
    CHECK(f0_span > 20.0 * sigma_span);
}

TEST_CASE("track_cycles recovers a prescribed sigma ramp at constant f0",
          "[modal-analysis][calibration]") {
    // The other half of the calibration: having proved the estimator does not
    // invent sigma variation, prove it can still see it. Without this, "sigma
    // came out flat" would be indistinguishable from a broken estimator.
    const auto sigma_at = [](double t) {
        return 7.2 - (7.2 - 5.5) * std::exp(-t / 0.100);
    };
    const auto x = synth_glide([](double) { return 47.65; }, sigma_at, 1.2, kFs);

    CycleTrackOptions o;
    o.start_offset_s = 0.005; // synthetic: no excitation transient to clear
    const auto track = track_cycles(x, kFs, o);
    INFO(summarize(track));
    REQUIRE(track.ok);

    const double t0 = track.cycles.front().time_s;
    CHECK(track.f0_span_percent(t0, t0 + 0.30) < 0.5); // f0 really is constant

    // Sample the recovered sigma against the prescribed ramp at three points.
    // The tolerance budgets the 7-cycle (~150 ms) fit window smearing a ramp
    // whose time constant is 100 ms.
    for (const double probe : {0.10, 0.30, 0.60}) {
        const double got = track.mean_sigma(probe - 0.02, probe + 0.02);
        const double want = sigma_at(probe);
        if (got == 0.0) continue; // outside the tracked region
        INFO("t=" << probe << " s: prescribed sigma " << want << ", recovered " << got);
        CHECK(std::abs(got - want) / want < 0.08);
    }

    // And the ramp must be visible as a span, not smoothed into a constant.
    // The prescribed ramp spans 27% of its mean, but sigma only becomes valid
    // three cycles (~70 ms) in, by which point the 100 ms exponential has
    // already covered half its travel, and the 7-cycle fit window smears the
    // rest. The threshold below is the measured recovery with margin — the
    // load-bearing comparison is against the constant-sigma control, which
    // recovers a span two orders of magnitude smaller from the same estimator.
    const double span = track.sigma_span_percent(t0, t0 + 0.60);
    INFO("sigma span over the ramp: " << span << "%");
    CHECK(span > 6.0);
}

TEST_CASE("track_cycles Q follows the pi*f0/sigma convention",
          "[modal-analysis][calibration]") {
    // A Q quoted without its envelope convention is how factor-of-two
    // arguments start. Pin it: envelope e^{-sigma t} => Q = pi f0 / sigma.
    const double f0 = 100.0, sigma = 5.0;
    const auto x = synth_glide([f0](double) { return f0; },
                               [sigma](double) { return sigma; }, 1.5, kFs);
    const auto track = track_cycles(x, kFs);
    REQUIRE(track.ok);

    const double t0 = track.cycles.front().time_s;
    const double q = track.mean_q(t0 + 0.05, t0 + 0.40);
    const double expected = kPi * f0 / sigma;
    INFO(summarize(track));
    INFO("expected Q " << expected << ", measured " << q);
    CHECK(std::abs(q - expected) / expected < 0.03);
}

TEST_CASE("track_cycles flags a signal it cannot track",
          "[modal-analysis][calibration]") {
    // Two comparable partials produce zero crossings that belong to neither.
    // The estimator has no way to know which component it is following, so it
    // must say so rather than return a confident number for a fiction.
    const std::vector<pulp::signal::ModalMode> two = {
        {100.0f, 2.0f, 1.0f},
        {143.0f, 2.0f, 1.0f},
    };
    const auto x = synth_modes(two, static_cast<std::size_t>(1.5 * kFs), kFs);
    const auto track = track_cycles(x, kFs);
    INFO(summarize(track));
    CHECK(track.monocomponent_confidence < 0.8);

    // A clean single mode, by contrast, must score near 1.0 — otherwise the
    // flag above is just noise and carries no information.
    const std::vector<pulp::signal::ModalMode> one = {{100.0f, 2.0f, 1.0f}};
    const auto clean = track_cycles(
        synth_modes(one, static_cast<std::size_t>(1.5 * kFs), kFs), kFs);
    INFO(summarize(clean));
    CHECK(clean.monocomponent_confidence > 0.95);
}

TEST_CASE("track_cycles refuses unusable buffers", "[modal-analysis][calibration]") {
    const std::vector<float> zeros(48000, 0.0f);
    const auto z = track_cycles(zeros, kFs);
    INFO(z.message);
    CHECK_FALSE(z.ok);
    CHECK_FALSE(z.message.empty());

    // Too short to hold the default 21 ms start offset plus cycles.
    const std::vector<float> tiny(64, 0.5f);
    const auto t = track_cycles(tiny, kFs);
    INFO(t.message);
    CHECK_FALSE(t.ok);
}

// ── Calibration: the independent cross-check ──────────────────────────────

TEST_CASE("track_ar2 agrees with track_cycles on a known decaying sinusoid",
          "[modal-analysis][calibration]") {
    // The AR(2) tracker shares no machinery with the zero-crossing tracker —
    // no crossings, no envelope, no log. Agreement between them on a signal
    // whose answer is known is corroboration; agreement between two estimators
    // that share a bias would not be.
    const double f0 = 250.0, sigma = 6.0;
    const auto x = synth_glide([f0](double) { return f0; },
                               [sigma](double) { return sigma; }, 1.5, kFs);

    const auto ar2 = track_ar2(x, kFs);
    INFO(summarize(ar2));
    REQUIRE(ar2.ok);

    for (const auto& o : ar2.windows) {
        if (o.time_s > 0.6) break;
        INFO("t=" << o.time_s << " f0=" << o.freq_hz << " sigma=" << o.sigma_np_s
                  << " resid=" << o.residual_ratio);
        CHECK(std::abs(cents(o.freq_hz, f0)) < 5.0);
        CHECK(std::abs(o.sigma_np_s - sigma) / sigma < 0.05);
        // A two-pole model fits one decaying sinusoid to near-nothing.
        CHECK(o.residual_ratio < 1.0e-6);
    }

    const auto cyc = track_cycles(x, kFs);
    REQUIRE(cyc.ok);
    const double cyc_sigma = cyc.mean_sigma(0.10, 0.50);
    double ar2_sigma = 0.0;
    int n = 0;
    for (const auto& o : ar2.windows)
        if (o.time_s >= 0.10 && o.time_s < 0.50) {
            ar2_sigma += o.sigma_np_s;
            ++n;
        }
    REQUIRE(n > 0);
    ar2_sigma /= n;
    INFO("zero-crossing sigma " << cyc_sigma << " vs AR(2) sigma " << ar2_sigma);
    CHECK(std::abs(cyc_sigma - ar2_sigma) / ar2_sigma < 0.05);
}

TEST_CASE("track_ar2 reports no pole for a signal that is not a resonance",
          "[modal-analysis][calibration]") {
    // White noise has no complex pole pair to find. An estimator that returns
    // one anyway would report a frequency and Q for anything at all.
    std::mt19937 rng(0xBEEF);
    std::normal_distribution<float> g(0.0f, 0.2f);
    std::vector<float> noise(static_cast<std::size_t>(0.5 * kFs));
    for (auto& v : noise) v = g(rng);

    const auto ar2 = track_ar2(noise, kFs);
    INFO(summarize(ar2));
    // Noise may still yield a nominal pole, but the residual must expose it as
    // meaningless — a real resonance fits to <1e-6 (asserted above).
    for (const auto& o : ar2.windows) {
        INFO("noise window resid " << o.residual_ratio);
        CHECK(o.residual_ratio > 0.5);
    }
}

// ── Ground truth through the real renderer ────────────────────────────────

TEST_CASE("analyze_modes recovers a ModalBank spec from its rendered IR",
          "[modal-analysis][calibration]") {
    // End to end: prescribe modes, render through the actual ModalBank, and
    // recover the spec from the audio. This is the loop the physical-modelling
    // lanes rely on — "did the resonator do what I asked" answered in numbers.
    const std::vector<pulp::signal::ModalMode> spec = {
        {87.31f, 2.4f, 1.0f},
        {233.08f, 1.6f, 0.55f},
        {698.46f, 1.0f, 0.30f},
        {1864.66f, 0.6f, 0.18f},
    };
    const auto ir = render_modal_bank_ir(spec, static_cast<std::size_t>(2.5 * kFs), kFs);

    const auto a = analyze_modes(ir, kFs);
    INFO(summarize(a));
    REQUIRE(a.ok);
    REQUIRE(a.modes.size() == spec.size());

    std::printf("\nmodal spec recovery from rendered IR\n");
    std::printf("%12s %12s %10s %10s %10s %10s %8s\n", "spec f (Hz)", "meas f (Hz)",
                "cents", "spec T60", "meas T60", "T60 err%", "amp err%");
    for (const auto& s : spec) {
        const auto* m = nearest(a, s.freq_hz);
        REQUIRE(m != nullptr);
        const double dc = cents(m->freq_hz, s.freq_hz);
        const double te = (m->t60_s - s.t60_s) / s.t60_s * 100.0;
        const double ae = (m->amplitude - s.gain) / s.gain * 100.0;
        std::printf("%12.2f %12.2f %10.3f %10.3f %10.3f %10.2f %8.2f\n", s.freq_hz,
                    m->freq_hz, dc, s.t60_s, m->t60_s, te, ae);
        CHECK(std::abs(dc) < 5.0);
        CHECK(std::abs(te) < 5.0);
        CHECK(std::abs(ae) < 15.0);
    }
}
