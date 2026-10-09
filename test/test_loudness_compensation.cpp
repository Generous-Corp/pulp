// The product-independent loudness-compensation unit
// (<pulp/signal/loudness_compensation.hpp>). Links Pulp::signal and
// Catch2 only, so it moves upstream with the header: pure-function vectors,
// the estimator's determinism across chunkings, gating, start-up, the target
// slew, and an allocation count over the realtime calls.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/fir_design.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/loudness_compensation.hpp>
#include <pulp/signal/multi_channel_meter.hpp>

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <random>
#include <span>
#include <vector>

// ── Allocation counter (this executable only) ──────────────────────────────
namespace {
std::atomic<bool> g_counting{false};
std::atomic<long> g_allocations{0};
void* counted(std::size_t n) {
    if (g_counting.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
} // namespace
void* operator new(std::size_t n) {
    return counted(n);
}
void* operator new[](std::size_t n) {
    return counted(n);
}
void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}

using namespace pulp::signal;
using Catch::Approx;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> noise(std::size_t n, unsigned seed, double amplitude = 0.1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> w(0.0, amplitude);
    std::vector<float> x(n);
    for (auto& v : x)
        v = static_cast<float>(w(rng));
    return x;
}

std::vector<float> tone(std::size_t n, double hz, double amplitude = 0.1) {
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i)
        x[i] = static_cast<float>(amplitude *
                                  std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate));
    return x;
}

// Push a mono signal (fed to both channels) in a repeating chunk pattern and
// record the absolute sample index after which each frame completed.
std::vector<std::size_t> feed(LongTermSpectrum& s, const std::vector<float>& x,
                              const std::vector<int>& chunks) {
    std::vector<std::size_t> frames;
    std::size_t pos = 0, c = 0;
    while (pos < x.size()) {
        const auto n = static_cast<int>(std::min<std::size_t>(
            static_cast<std::size_t>(chunks[c++ % chunks.size()]), x.size() - pos));
        const float* ch[] = {x.data() + pos, x.data() + pos};
        s.push(ch, 2, n,
               [&](int consumed) { frames.push_back(pos + static_cast<std::size_t>(consumed)); });
        pos += static_cast<std::size_t>(n);
    }
    return frames;
}

} // namespace

TEST_CASE("makeup_gain_db: pure vectors", "[loudness-compensation][pure]") {
    const MakeupLimits limits{24.0f, 24.0f};
    const std::vector<double> flat(8, 1.0), spectrum{0, 0, 0, 1, 0, 0, 0, 0};
    CHECK(makeup_gain_db(flat, flat, limits) == Approx(0.0f).margin(1e-6));
    // Everything at bin 3, boosted 6.02 dB there: cut 6.02 dB.
    std::vector<double> boost3 = flat;
    boost3[3] = 4.0;
    CHECK(makeup_gain_db(boost3, spectrum, limits) == Approx(-6.0206f).margin(1e-3));
    // The same boost where there is no material changes nothing.
    std::vector<double> boost6 = flat;
    boost6[6] = 4.0;
    CHECK(makeup_gain_db(boost6, spectrum, limits) == Approx(0.0f).margin(1e-6));
    // Half the material muted: +3.01 dB.
    const std::vector<double> two{1, 1, 0, 0}, half{1, 0, 1, 1};
    CHECK(makeup_gain_db(half, two, limits) == Approx(3.0103f).margin(1e-3));
    // Everything removed: the boost limit; nothing to measure: 0 dB.
    CHECK(makeup_gain_db(std::vector<double>(8, 0.0), flat, limits) == 24.0f);
    CHECK(makeup_gain_db(flat, std::vector<double>(8, 0.0), limits) == 0.0f);
    // Clamps.
    std::vector<double> huge = flat;
    for (auto& v : huge)
        v = 1.0e6;
    CHECK(makeup_gain_db(huge, flat, {12.0f, 6.0f}) == -12.0f);
    CHECK(makeup_gain_db(std::vector<double>(8, 1.0e-6), flat, {12.0f, 6.0f}) == 6.0f);
}

TEST_CASE("blend_makeup_gain_db: the two-leg model", "[loudness-compensation][pure]") {
    const MakeupLimits limits{24.0f, 24.0f};
    const std::vector<double> p{1, 1, 1, 1}, zero(4, 0.0);
    using C = std::complex<double>;
    // Coherent legs (an ordinary effect), 100 % wet: the single-leg answer.
    const std::vector<C> boost{{2, 0}, {1, 0}, {1, 0}, {1, 0}};
    const LegSpectra coherent{p, p, p, zero};
    const std::vector<double> boost_power{4, 1, 1, 1};
    CHECK(blend_makeup_gain_db(boost, coherent, 1.0, limits) ==
          Approx(makeup_gain_db(boost_power, p, limits)).margin(1e-6));
    // Coherent at 50 %: |0.5 H + 0.5|^2 per bin.
    const double half = (2.25 + 3.0) / 4.0;
    CHECK(blend_makeup_gain_db(boost, coherent, 0.5, limits) ==
          Approx(-10.0 * std::log10(half)).margin(1e-4));
    // A pure phase flip at 50 %: coherent legs cancel, so the boost limit...
    const std::vector<C> flip(4, C{-1, 0});
    CHECK(blend_makeup_gain_db(flip, coherent, 0.5, limits) == 24.0f);
    // ...uncorrelated legs (a hold while the input moves on) add as powers,
    // so the same flip changes nothing.
    const LegSpectra uncorrelated{p, p, zero, zero};
    CHECK(blend_makeup_gain_db(flip, uncorrelated, 0.5, limits) == Approx(0.0f).margin(1e-6));
    // Uncorrelated, 50 %, the wet leg muted: half the power, +3.01 dB.
    const std::vector<C> mute(4, C{0, 0});
    CHECK(blend_makeup_gain_db(mute, uncorrelated, 0.5, limits) == Approx(3.0103f).margin(1e-3));
    // Nothing to measure.
    const LegSpectra silent{zero, zero, zero, zero};
    CHECK(blend_makeup_gain_db(boost, silent, 1.0, limits) == 0.0f);
}

TEST_CASE("MinimumPhaseResponse keeps the magnitude and has minimum phase",
          "[loudness-compensation][pure]") {
    constexpr int n = 1024;
    MinimumPhaseResponse mp;
    mp.prepare(n);
    REQUIRE(mp.prepared());
    std::vector<std::complex<double>> out(n / 2 + 1);
    // Flat: zero phase.
    REQUIRE(mp.compute(std::vector<double>(n / 2 + 1, 1.0), 1e-6, out));
    double worst_phase = 0.0;
    for (const auto& h : out)
        worst_phase = std::max(worst_phase, std::abs(std::arg(h)));
    CHECK(worst_phase < 1e-5);
    // A one-pole low-pass's magnitude comes back as that one-pole's response
    // (a one-pole is minimum phase), phase and all.
    const double a = 0.9;
    std::vector<double> mag(n / 2 + 1);
    for (int k = 0; k <= n / 2; ++k) {
        const auto z = std::polar(1.0, -2.0 * kPi * k / n);
        mag[static_cast<std::size_t>(k)] = std::abs((1.0 - a) / (1.0 - a * z));
    }
    REQUIRE(mp.compute(mag, 1e-6, out));
    double worst = 0.0;
    for (int k = 0; k <= n / 2; ++k) {
        const auto z = std::polar(1.0, -2.0 * kPi * k / n);
        const auto want = (1.0 - a) / (1.0 - a * z);
        worst = std::max(worst, std::abs(out[static_cast<std::size_t>(k)] - want));
    }
    INFO("worst deviation from the one-pole response " << worst);
    CHECK(worst < 1e-3);
}

// The FIR designer allocates on the control thread; MinimumPhaseResponse keeps
// a prepared response workspace for real-time use. Compare their complex responses, not
// just magnitudes, so a cepstrum-folding or phase-sign mismatch is observable.
TEST_CASE("MinimumPhaseResponse agrees with the offline FIR designer",
          "[loudness-compensation][pure]") {
    constexpr int n = 1024;
    constexpr double floor = 1e-6;
    MinimumPhaseResponse prepared;
    prepared.prepare(n);
    REQUIRE(prepared.prepared());
    std::vector<double> magnitude(n / 2 + 1);
    std::vector<std::complex<double>> response(n / 2 + 1);
    for (int shape = 0; shape < 3; ++shape) {
        CAPTURE(shape);
        for (int bin = 0; bin <= n / 2; ++bin) {
            const double x = static_cast<double>(bin) / (n / 2);
            magnitude[bin] = shape == 0   ? 1.0
                             : shape == 1 ? 0.1 + 1.9 * x
                                          : (bin >= 150 && bin <= 240 ? 0.0 : 0.5 + x);
        }
        pulp::signal::MinimumPhaseFirOptions options;
        options.log_magnitude_floor = floor;
        const auto fir = pulp::signal::reconstruct_minimum_phase_fir(magnitude, options);
        REQUIRE(fir.status == pulp::signal::FirDesignStatus::success);
        REQUIRE(fir.coefficients.size() == n);
        REQUIRE(prepared.compute(magnitude, floor, response));
        std::vector<std::complex<double>> spectrum(n);
        for (int sample = 0; sample < n; ++sample)
            spectrum[sample] = {fir.coefficients[sample], 0.0};
        pulp::signal::FftT<double> fft(n);
        REQUIRE(fft.ready());
        fft.forward(spectrum.data());
        double worst = 0.0;
        for (int bin = 0; bin <= n / 2; ++bin)
            worst = std::max(worst, std::abs(response[bin] - spectrum[bin]));
        INFO("maximum complex response error " << worst);
        CHECK(worst < 5e-5);
    }
}

TEST_CASE("LongTermSpectrum measures how the legs combine", "[loudness-compensation][state]") {
    LoudnessCompensationConfig config;
    config.track_dry_leg = true;
    const auto x = noise(static_cast<std::size_t>(kRate * 6.0), 21u);
    const auto y = noise(x.size(), 22u);
    const auto sum = [](std::span<const double> v) {
        double t = 0;
        for (const double e : v)
            t += e;
        return t;
    };
    // The wet leg IS the dry leg: the cross-spectrum is the power spectrum.
    LongTermSpectrum same;
    same.prepare(kRate, 1, config);
    const float* xc[] = {x.data()};
    same.push(xc, xc, 1, static_cast<int>(x.size()), [](int) {});
    const auto s = same.legs();
    CHECK(sum(s.wd_re) == Approx(sum(s.ww)).epsilon(1e-6));
    CHECK(std::abs(sum(s.wd_im)) < 1e-6 * sum(s.ww));
    // Independent legs: the cross-spectrum is a small fraction of the power.
    LongTermSpectrum apart;
    apart.prepare(kRate, 1, config);
    const float* yc[] = {y.data()};
    apart.push(xc, yc, 1, static_cast<int>(x.size()), [](int) {});
    const auto t = apart.legs();
    const double coherence = std::abs(sum(t.wd_re)) / std::sqrt(sum(t.ww) * sum(t.dd));
    std::printf(
        "[loudness-compensation] independent legs: |sum Pwd| / sqrt(sum Pww sum Pdd) = %.4f\n",
        coherence);
    CHECK(coherence < 0.05);
}

TEST_CASE("band_makeup_gain_db over a SpectrumCdf", "[loudness-compensation][pure]") {
    // Bins of 100 Hz; all the energy in bins 2..3 (150..350 Hz).
    std::vector<double> power(10, 0.0);
    power[2] = power[3] = 1.0;
    SpectrumCdf cdf;
    cdf.prepare(power.size());
    cdf.assign(power, 100.0);
    CHECK(cdf.weight(0.0, 1.0e9) == Approx(1.0));
    CHECK(cdf.weight(150.0, 250.0) == Approx(0.5));
    CHECK(cdf.weight(200.0, 250.0) == Approx(0.25)); // linear inside a bin
    const auto weight = [&](double lo, double hi) { return cdf.weight(lo, hi); };
    const MakeupLimits limits{24.0f, 24.0f};
    const BandPowerGain cut_material[] = {
        {1.0, 150.0, 1.0}, {150.0, 350.0, 0.25}, {350.0, 1e9, 1.0}};
    CHECK(band_makeup_gain_db(cut_material, weight, limits) == Approx(6.0206f).margin(1e-3));
    const BandPowerGain boost_elsewhere[] = {
        {1.0, 150.0, 16.0}, {150.0, 350.0, 1.0}, {350.0, 1e9, 16.0}};
    CHECK(band_makeup_gain_db(boost_elsewhere, weight, limits) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("k_weighting_power follows BS.1770", "[loudness-compensation][pure]") {
    const auto k = pulp::signal::k_weighting_coefficients(kRate);
    REQUIRE(k.valid);
    const auto db = [&](double hz) { return 10.0 * std::log10(k_weighting_power(hz, k, kRate)); };
    // +0.69 dB at 1 kHz (what the -0.691 in the LUFS formula cancels), the
    // ~+4 dB shelf by 10 kHz, and the high-pass well down by 20 Hz.
    CHECK(db(997.0) == Approx(0.69).margin(0.05));
    CHECK(db(10000.0) == Approx(4.0).margin(0.3));
    CHECK(db(20.0) < -10.0);
}

TEST_CASE("LongTermSpectrum does not depend on how the stream is chopped",
          "[loudness-compensation][determinism]") {
    LoudnessCompensationConfig config;
    auto x = noise(static_cast<std::size_t>(kRate * 2.0), 3u);
    const auto t = tone(static_cast<std::size_t>(kRate * 2.0), 3000.0);
    x.insert(x.end(), t.begin(), t.end());
    LongTermSpectrum reference;
    reference.prepare(kRate, 2, config);
    const auto reference_frames = feed(reference, x, {512});
    REQUIRE(reference_frames.size() > 40);
    for (const auto& chunks : std::vector<std::vector<int>>{
             {1}, {37}, {64}, {4096}, {2048}, {17, 256, 3, 64, 101}, {10000}}) {
        LongTermSpectrum s;
        s.prepare(kRate, 2, config);
        const auto frames = feed(s, x, chunks);
        CHECK(frames == reference_frames);
        const auto a = reference.spectrum(), b = s.spectrum();
        REQUIRE(a.size() == b.size());
        std::size_t differ = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            differ += a[i] != b[i];
        CHECK(differ == 0);
        CHECK(s.weight(2000.0, 4000.0) == reference.weight(2000.0, 4000.0));
    }
    // Control: a different signal gives a different estimate.
    LongTermSpectrum other;
    other.prepare(kRate, 2, config);
    (void)feed(other, noise(x.size(), 4u), {512});
    REQUIRE(other.weight(2000.0, 4000.0) != reference.weight(2000.0, 4000.0));
}

TEST_CASE("LongTermSpectrum starts at the prior, converges, holds through silence, resets",
          "[loudness-compensation][state]") {
    LoudnessCompensationConfig config;
    const int bins = LongTermSpectrum::bins_for(kRate, config);
    // A prior with all its energy below 100 Hz.
    std::vector<double> prior(static_cast<std::size_t>(bins), 0.0);
    for (int k = 1; k < 8; ++k)
        prior[static_cast<std::size_t>(k)] = 1.0;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config, prior);
    CHECK(s.weight(0.0, 100.0) == Approx(1.0));
    // A 3 kHz tone for 12 s (4 time constants): the estimate moves to it.
    const auto x = tone(static_cast<std::size_t>(kRate * 12.0), 3000.0);
    const float* ch[] = {x.data()};
    s.push(ch, 1, static_cast<int>(x.size()));
    const double at_tone = s.weight(2900.0, 3100.0);
    std::printf("[loudness-compensation] after 12 s of 3 kHz: %.3f of the energy within "
                "100 Hz of it, %.4f still below 100 Hz\n",
                at_tone, s.weight(0.0, 100.0));
    CHECK(at_tone > 0.95);
    // Silence: once the analysis window has left the tone (one frame
    // length), every frame is gated and the estimate is held bit for bit.
    const std::vector<float> silence(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    const float* sc[] = {silence.data()};
    s.push(sc, 1, s.fft_size());
    std::vector<double> before(s.spectrum().begin(), s.spectrum().end());
    const auto gated_before = s.gated_frames();
    s.push(sc, 1, static_cast<int>(silence.size()));
    CHECK(s.gated_frames() > gated_before + 40);
    CHECK(std::equal(before.begin(), before.end(), s.spectrum().begin()));
    // Near-silence below the gate (-80 dBFS noise) is held too.
    const auto hiss = noise(static_cast<std::size_t>(kRate * 2.0), 9u, 1.0e-4);
    const float* hc[] = {hiss.data()};
    s.push(hc, 1, static_cast<int>(hiss.size()));
    CHECK(std::equal(before.begin(), before.end(), s.spectrum().begin()));
    // Reset: back to the prior.
    s.reset();
    CHECK(s.weight(0.0, 100.0) == Approx(1.0));
    CHECK(s.observed_frames() == 0);
}

TEST_CASE("MakeupTarget: an edit jumps, the material slews", "[loudness-compensation][state]") {
    MakeupTarget t;
    CHECK(t.retarget(-6.0f));
    CHECK(t.value_db() == -6.0f);
    // 6 dB/s over a 42.7 ms hop: at most 0.256 dB per step.
    CHECK(t.follow(0.0f, 2048.0 / kRate, 6.0));
    CHECK(t.value_db() == Approx(-6.0f + 0.256f).margin(1e-3));
    CHECK(t.retarget(3.0f));
    CHECK(t.value_db() == 3.0f);
    CHECK_FALSE(t.follow(3.0f, 0.1, 6.0));
    // No limit when the slew is off.
    CHECK(t.follow(-10.0f, 0.001, 0.0));
    CHECK(t.value_db() == -10.0f);
}

TEST_CASE("MakeupTarget: proportional slew far from the target, slow near it",
          "[loudness-compensation][state]") {
    MakeupTarget t;
    REQUIRE(t.retarget(0.0f));
    // 20 dB away at 10/s, capped at 120 dB/s: 120 x 0.0427 = 5.1 dB this hop.
    REQUIRE(t.follow(-20.0f, 2048.0 / kRate, 6.0, 10.0, 120.0));
    CHECK(t.value_db() == Approx(-5.12f).margin(0.02f));
    // 0.3 dB away: the 6 dB/s floor, all of it in one hop (0.256 dB max).
    MakeupTarget n;
    REQUIRE(n.retarget(0.0f));
    REQUIRE(n.follow(-0.3f, 2048.0 / kRate, 6.0, 10.0, 120.0));
    CHECK(n.value_db() == Approx(-0.256f).margin(0.002f));
}

TEST_CASE("MaterialChangeDetector: a gap must persist; a huge one less long",
          "[loudness-compensation][state]") {
    using E = MaterialChangeDetector::Event;
    MaterialChangeDetector d;
    d.threshold_db = 4.0;
    d.frames = 12;
    d.huge_db = 12.0;
    d.huge_frames = 6;
    d.holdoff_frames = 6;
    // A passing moment: a run that drains before 12 frames is forgotten
    // (leaky: one frame below the threshold takes two back).
    CHECK(d.observe(0.0f, 6.0f) == E::run_started);
    for (int i = 0; i < 3; ++i)
        CHECK(d.observe(0.0f, 6.0f) == E::none);
    CHECK(d.observe(0.0f, 1.0f) == E::none);       // 4 -> 2
    CHECK(d.observe(0.0f, 1.0f) == E::run_broken); // 2 -> 0
    // A sustained 6 dB gap: confirmed on the 12th frame.
    CHECK(d.observe(0.0f, 6.0f) == E::run_started);
    for (int i = 0; i < 10; ++i)
        CHECK(d.observe(0.0f, 6.0f) == E::none);
    CHECK(d.observe(0.0f, 6.0f) == E::confirmed);
    for (int i = 0; i < 6; ++i)
        CHECK(d.observe(0.0f, 20.0f) != E::confirmed); // hold-off
    // A huge gap: six frames.
    CHECK(d.observe(0.0f, 20.0f) == E::run_started);
    for (int i = 0; i < 4; ++i)
        CHECK(d.observe(0.0f, 20.0f) == E::none);
    CHECK(d.observe(0.0f, 20.0f) == E::confirmed);
}

TEST_CASE("LongTermSpectrum: a locate keeps the estimate, a saved one restores warm",
          "[loudness-compensation][state]") {
    LoudnessCompensationConfig config;
    config.track_dry_leg = true;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config);
    const auto x = tone(static_cast<std::size_t>(kRate * 4.0), 3000.0);
    const float* c[] = {x.data()};
    s.push(c, c, 1, static_cast<int>(x.size()), [](int) {});
    const double before = s.weight(2900.0, 3100.0);
    REQUIRE(before > 0.95);
    s.restart_grid();
    CHECK(s.weight(2900.0, 3100.0) == before);
    CHECK(s.samples_to_next_frame() == s.hop());
    // Band-compressed round trip, into another instance at another rate.
    SpectrumBands bands;
    s.export_bands(bands);
    REQUIRE(bands.valid);
    LongTermSpectrum t;
    t.prepare(96000.0, 1, config);
    CHECK(t.weight(2900.0, 3100.0) < 0.05); // the flat prior
    t.import_bands(bands);
    std::printf("[loudness-compensation] restored at 96 kHz: %.3f of the energy within "
                "100 Hz of 3 kHz (saved %.3f)\n",
                t.weight(2900.0, 3100.0), before);
    CHECK(t.weight(2900.0, 3100.0) > 0.9);
    CHECK(t.prior_weight() == 0.0);
}

TEST_CASE("MaterialChangeDetector: a smaller gap counts after a lasting loud part falls",
          "[loudness-compensation][state]") {
    using E = MaterialChangeDetector::Event;
    const auto make = [] {
        MaterialChangeDetector d;
        d.threshold_db = 4.0;
        d.frames = 8;
        d.huge_db = 12.0;
        d.huge_frames = 6;
        d.drop_threshold_db = 2.0;
        d.drop_level_db = 3.0;
        d.drop_min_loud_frames = 10;
        d.max_decay_db_per_frame = 0.5;
        d.level_fast_alpha = 0.5;
        d.level_slow_alpha = 0.05;
        return d;
    };
    // `loud` frames at -10 LUFS with no gap, then frames at `level(k)` with a
    // 3 dB gap (under the ordinary 4 dB threshold): confirmed within `limit`?
    const auto run = [](MaterialChangeDetector d, int loud, const std::function<double(int)>& level,
                        int limit) {
        for (int k = 0; k < loud; ++k)
            (void)d.observe(0.0f, 0.0f, -10.0);
        for (int k = 0; k < limit; ++k)
            if (d.observe(0.0f, 3.0f, level(k)) == E::confirmed)
                return k + 1;
        return -1;
    };
    const int steady = run(make(), 60, [](int) { return -17.0; }, 30);
    std::printf(
        "[loudness-compensation] 7 dB quieter, steady, 3 dB gap: confirmed after %d frames\n",
        steady);
    CHECK(steady > 0);
    CHECK(steady <= 12);
    // A decay (1 dB per frame) is not a change of material.
    CHECK(run(make(), 60, [](int k) { return -12.0 - 1.0 * k; }, 30) == -1);
    // A loud part shorter than drop_min_loud_frames (after the warm-up) does
    // not arm the path: quiet, a 4-frame loud burst, quiet again; a 12-frame
    // loud part does.
    for (const int burst : {4, 12}) {
        auto d = make();
        for (int k = 0; k < 11; ++k)
            (void)d.observe(0.0f, 0.0f, -17.0);
        for (int k = 0; k < burst; ++k)
            (void)d.observe(0.0f, 0.0f, 0.0);
        int confirmed = -1;
        for (int k = 0; k < 30 && confirmed < 0; ++k)
            if (d.observe(0.0f, 3.0f, -17.0) == E::confirmed)
                confirmed = k + 1;
        std::printf("[loudness-compensation] after a %d-frame loud part: confirmed after %d\n",
                    burst, confirmed);
        if (burst == 4)
            CHECK(confirmed == -1);
        else
            CHECK(confirmed > 0);
    }
    // Control: without the path the same steady drop is never confirmed.
    auto off = make();
    off.drop_threshold_db = 0.0;
    CHECK(run(off, 60, [](int) { return -17.0; }, 30) == -1);
    // The ordinary path is unchanged: a 6 dB gap at a steady level confirms.
    {
        auto d = make();
        int k = 0;
        for (; k < 30; ++k)
            if (d.observe(0.0f, 6.0f, -10.0) == E::confirmed)
                break;
        CHECK(k + 1 == 8);
        CHECK_FALSE(d.last_was_drop());
    }
}

TEST_CASE("LongTermSpectrum: a level drop rescales the level, never the spectrum",
          "[loudness-compensation][state]") {
    // Loud noise, then a 3 kHz tone 30 dB quieter for 6 s. Energy weighting
    // alone lets the noise outweigh the tone for a long time; the level-drop
    // rule (4 s window) brings the old level down, so the tone takes over --
    // without a restart (a decay must not restart the estimate).
    LoudnessCompensationConfig config;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config);
    auto x = noise(static_cast<std::size_t>(kRate * 5.0), 8u, 0.3);
    const auto quiet = tone(static_cast<std::size_t>(kRate * 6.0), 3000.0, 0.3 * 0.0316);
    x.insert(x.end(), quiet.begin(), quiet.end());
    const float* c[] = {x.data()};
    s.push(c, 1, static_cast<int>(x.size()));
    std::printf("[loudness-compensation] 6 s after a 30 dB drop to a tone: %.3f of the energy "
                "within 100 Hz of it (%llu level drops, %llu restarts)\n",
                s.weight(2900.0, 3100.0), static_cast<unsigned long long>(s.level_drops()),
                static_cast<unsigned long long>(s.restarts()));
    CHECK(s.level_drops() >= 1);
    CHECK(s.restarts() == 0);
    CHECK(s.weight(2900.0, 3100.0) > 0.4);
    // Control: without the rule the noise still dominates.
    config.level_drop_db = 0.0;
    LongTermSpectrum r;
    r.prepare(kRate, 1, config);
    r.push(c, 1, static_cast<int>(x.size()));
    CHECK(r.weight(2900.0, 3100.0) < 0.2);
}

TEST_CASE("LongTermSpectrum: a restart starts from the candidate and remembers the past",
          "[loudness-compensation][state]") {
    // Noise for 4 s, then a 3 kHz tone at the same level. A candidate begun
    // when the tone starts holds only the tone: restarting from it gives the
    // tone's estimate at once. Going back to noise and merging gives the mix.
    LoudnessCompensationConfig config;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config);
    const auto a = noise(static_cast<std::size_t>(kRate * 4.0), 3u, 0.1);
    const auto b = tone(static_cast<std::size_t>(kRate * 1.0), 3000.0, 0.14);
    const float* ca[] = {a.data()};
    s.push(ca, 1, static_cast<int>(a.size()));
    const double noise_share = s.weight(2900.0, 3100.0);
    // Skip one window's worth so the candidate's frames hold only the tone.
    const float* cb[] = {b.data()};
    s.push(cb, 1, s.fft_size());
    s.begin_candidate();
    s.push(cb, 1, static_cast<int>(b.size()) - s.fft_size());
    s.restart();
    std::printf("[loudness-compensation] restart from the candidate: %.3f of the energy near "
                "3 kHz (noise alone %.3f)\n",
                s.weight(2900.0, 3100.0), noise_share);
    CHECK(s.weight(2900.0, 3100.0) > 0.95);
    CHECK(s.has_previous());
    s.merge_with_previous();
    CHECK(s.alternating());
    const double mixed = s.weight(2900.0, 3100.0);
    std::printf("[loudness-compensation] merged with the noise before it: %.3f\n", mixed);
    CHECK(mixed > 0.3);
    CHECK(mixed < 0.7);
}

TEST_CASE("LongTermSpectrum: a restored estimate is a prior; a carried one is warm and armed",
          "[loudness-compensation][state]") {
    LoudnessCompensationConfig config;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config);
    const auto low = tone(static_cast<std::size_t>(kRate * 4.0), 100.0, 0.3);
    const float* cl[] = {low.data()};
    s.push(cl, 1, static_cast<int>(low.size()));
    SpectrumBands saved;
    s.export_bands(saved);
    REQUIRE(saved.valid);
    const auto high = tone(static_cast<std::size_t>(kRate * 1.0), 3000.0, 0.3);
    const float* ch[] = {high.data()};
    // As a prior: on different material it converges exactly as a cold start.
    LongTermSpectrum prior, cold;
    prior.prepare(kRate, 1, config);
    cold.prepare(kRate, 1, config);
    prior.import_bands(saved, LongTermSpectrum::Import::prior);
    CHECK(prior.weight(50.0, 150.0) > 0.8); // starts at the saved estimate
    prior.push(ch, 1, static_cast<int>(high.size()));
    cold.push(ch, 1, static_cast<int>(high.size()));
    std::printf("[loudness-compensation] 1 s of a new tone: restored-as-prior %.3f, cold %.3f "
                "of the energy near it\n",
                prior.weight(2900.0, 3100.0), cold.weight(2900.0, 3100.0));
    CHECK(prior.weight(2900.0, 3100.0) == Approx(cold.weight(2900.0, 3100.0)).margin(1e-6));
    // Warm (a carried estimate): the detector's slow shape is armed on the
    // carried material, so new material shows as a gap at once.
    LongTermSpectrum warm;
    warm.prepare(kRate, 1, config);
    warm.import_bands(saved, LongTermSpectrum::Import::warm);
    const float* ch2[] = {high.data()};
    warm.push(ch2, 1, static_cast<int>(kRate * 0.5));
    const auto fast = warm.fast_shape_legs(), slow = warm.slow_shape_legs();
    double fast_hi = 0.0, slow_hi = 0.0;
    for (std::size_t k = 0; k < fast.ww.size(); ++k) {
        if (static_cast<double>(k) * warm.bin_hz() < 2000.0)
            continue;
        fast_hi += fast.ww[k];
        slow_hi += slow.ww[k];
    }
    std::printf("[loudness-compensation] warm import, 0.5 s of new material: fast shape %.3f, "
                "slow shape %.3f above 2 kHz\n",
                fast_hi, slow_hi);
    CHECK(fast_hi > 0.5);
    CHECK(slow_hi < 0.4); // not overwritten by the first frame
}

TEST_CASE("The realtime calls do not allocate", "[loudness-compensation][rt-safety]") {
    // Both leg modes: a single-leg estimator takes its own branches (the dry
    // and cross spectra copied from the wet one on import).
    const bool track_dry_leg = GENERATE(true, false);
    INFO("track_dry_leg " << track_dry_leg);
    LoudnessCompensationConfig config;
    config.track_dry_leg = track_dry_leg;
    LongTermSpectrum s;
    s.prepare(kRate, 2, config);
    MinimumPhaseResponse mp;
    mp.prepare(s.fft_size());
    std::vector<std::complex<double>> phased(static_cast<std::size_t>(s.bins()));
    std::vector<double> magnitude(static_cast<std::size_t>(s.bins()), 0.5);
    MakeupTarget target;
    const auto x = noise(static_cast<std::size_t>(kRate * 1.0), 5u);
    const float* ch[] = {x.data(), x.data()};
    std::vector<BandPowerGain> bands{{1.0, 500.0, 4.0}, {500.0, 1e9, 1.0}};
    std::vector<double> response(static_cast<std::size_t>(s.bins()), 2.0);
    g_allocations = 0;
    g_counting = true;
    for (std::size_t pos = 0; pos + 256 <= x.size(); pos += 256) {
        const float* c[] = {ch[0] + pos, ch[1] + pos};
        s.push(c, c, 2, 256, [&](int) {
            (void)target.follow(
                band_makeup_gain_db(bands, [&](double lo, double hi) { return s.weight(lo, hi); },
                                    {config.max_cut_db, config.max_boost_db}),
                0.04, 6.0);
        });
    }
    (void)makeup_gain_db(response, s.spectrum(), {24.0f, 24.0f});
    SpectrumBands saved;
    s.export_bands(saved);
    s.restart();
    s.restart_grid();
    s.import_bands(saved);
    s.import_bands(saved, LongTermSpectrum::Import::prior);
    const float* c0[] = {x.data(), x.data()};
    s.wet_becomes_dry();
    s.legs_uncorrelated();
    MaterialChangeDetector detector;
    (void)detector.observe(1.0f, 9.0f);
    s.begin_candidate();
    s.push(c0, c0, 2, 256, [](int) {});
    s.restart();
    s.merge_with_previous();
    s.cancel_candidate();
    (void)target.follow(-10.0f, 0.04, 6.0, 10.0, 120.0);
    REQUIRE(mp.compute(magnitude, 1e-6, phased));
    (void)blend_makeup_gain_db(phased, s.legs(), 0.5, {24.0f, 24.0f});
    s.reset();
    g_counting = false;
    INFO("allocations on the realtime path: " << g_allocations.load());
    CHECK(g_allocations.load() == 0);
    // Control: the counter sees an allocation.
    g_counting = true;
    auto* probe = new std::vector<int>(16);
    g_counting = false;
    delete probe;
    REQUIRE(g_allocations.load() > 0);
}

// ── Closed loop: the make-up restores the measured loudness ────────────────
namespace {

// Seeded pink noise (Kellett's economy filter over Gaussian white noise): equal
// energy per octave, so on a linear bin grid nearly all of its energy sits in
// the few bins below a few hundred hertz. A flat-weighted make-up counts bins
// instead and all but ignores a bass boost's loudness.
std::vector<float> pink(std::size_t n, unsigned seed, double amplitude = 0.05) {
    const auto w = noise(n, seed, 1.0);
    std::vector<float> x(n);
    double b0 = 0.0, b1 = 0.0, b2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        b0 = 0.99765 * b0 + w[i] * 0.0990460;
        b1 = 0.96300 * b1 + w[i] * 0.2965164;
        b2 = 0.57000 * b2 + w[i] * 1.0526913;
        x[i] = static_cast<float>(amplitude * (b0 + b1 + b2 + w[i] * 0.1848));
    }
    return x;
}

// BS.1770 gated integrated loudness, by the time-domain meter (an
// implementation independent of the estimator's frequency-domain model).
double integrated_lufs(const std::vector<float>& x) {
    MultiChannelMeter meter;
    meter.prepare(kRate, 1);
    const float* ch[] = {x.data()};
    constexpr int block = 512;
    for (std::size_t at = 0; at < x.size(); at += block) {
        const float* chunk[] = {ch[0] + at};
        meter.process(chunk, 1, static_cast<int>(std::min<std::size_t>(block, x.size() - at)));
    }
    return meter.snapshot().lufs_integrated;
}

struct ClosedLoop {
    double dry_lufs;
    double shaped_lufs;
    double compensated_lufs;
    float makeup_db;
};

// Material through a +12 dB low shelf at 300 Hz, the make-up computed from
// the estimator's spectrum of that material and the shelf's response on its
// bin grid, then applied to the shaped signal.
ClosedLoop closed_loop(bool weight_by_material) {
    const auto dry = pink(static_cast<std::size_t>(12.0 * kRate), 1234);

    Biquad shelf;
    shelf.set_coefficients(Biquad::Type::low_shelf, 300.0f, 0.707f, static_cast<float>(kRate),
                           12.0f);
    auto shaped = dry;
    shelf.process(shaped.data(), static_cast<int>(shaped.size()));

    LoudnessCompensationConfig config;
    config.weight_by_material = weight_by_material;
    LongTermSpectrum s;
    s.prepare(kRate, 1, config);
    const float* ch[] = {dry.data()};
    s.push(ch, 1, static_cast<int>(dry.size()));

    std::vector<double> response(static_cast<std::size_t>(s.bins()));
    const double bin_hz = kRate / static_cast<double>(s.fft_size());
    for (std::size_t b = 0; b < response.size(); ++b) {
        const double m = section_magnitude(
            shelf.coefficients(), angular_frequency(static_cast<double>(b) * bin_hz, kRate));
        response[b] = m * m;
    }
    const float makeup = makeup_gain_db(response, s.spectrum(), {24.0f, 24.0f});

    auto compensated = shaped;
    const float g = std::pow(10.0f, makeup / 20.0f);
    for (auto& v : compensated)
        v *= g;
    return {integrated_lufs(dry), integrated_lufs(shaped), integrated_lufs(compensated), makeup};
}

} // namespace

// Tolerance 0.5 LU. Detection floor: the uncompensated shelf moves the
// loudness by more than 3 LU, so a make-up of 0 dB misses by far more than
// the tolerance, and the flat-weighted negative control below must miss by
// more than 2 LU on the same material.
TEST_CASE("The make-up restores the dry loudness of tilted material through a shelf",
          "[loudness-compensation][closed-loop]") {
    const auto r = closed_loop(true);
    INFO("dry " << r.dry_lufs << " LUFS, shaped " << r.shaped_lufs << " LUFS, compensated "
                << r.compensated_lufs << " LUFS, make-up " << r.makeup_db << " dB");
    REQUIRE(std::isfinite(r.dry_lufs));
    CHECK(std::abs(r.shaped_lufs - r.dry_lufs) > 3.0);
    CHECK(std::abs(r.compensated_lufs - r.dry_lufs) < 0.5);
}

TEST_CASE("Flat weighting misses the dry loudness on tilted material",
          "[loudness-compensation][closed-loop]") {
    const auto r = closed_loop(false);
    INFO("dry " << r.dry_lufs << " LUFS, compensated " << r.compensated_lufs << " LUFS, make-up "
                << r.makeup_db << " dB");
    CHECK(std::abs(r.compensated_lufs - r.dry_lufs) > 2.0);
}

// A call with fewer channels than prepared feeds the rest silence: the same
// estimate as passing explicit zeros, never the older audio still in the
// unused channels' history.
TEST_CASE("A narrower push feeds the unused channels silence", "[loudness-compensation][state]") {
    const auto loud_l = noise(static_cast<std::size_t>(2.0 * kRate), 11, 0.3);
    const auto loud_r = noise(static_cast<std::size_t>(2.0 * kRate), 12, 0.3);
    const auto quiet = tone(static_cast<std::size_t>(3.0 * kRate), 300.0, 0.05);
    const std::vector<float> zeros(quiet.size(), 0.0f);

    auto run = [&](bool narrow) {
        LoudnessCompensationConfig config;
        LongTermSpectrum s;
        s.prepare(kRate, 2, config);
        const float* both[] = {loud_l.data(), loud_r.data()};
        s.push(both, 2, static_cast<int>(loud_l.size()));
        if (narrow) {
            const float* one[] = {quiet.data()};
            s.push(one, 1, static_cast<int>(quiet.size()));
        } else {
            const float* padded[] = {quiet.data(), zeros.data()};
            s.push(padded, 2, static_cast<int>(quiet.size()));
        }
        return std::vector<double>(s.spectrum().begin(), s.spectrum().end());
    };
    const auto narrow = run(true);
    const auto padded = run(false);
    REQUIRE(narrow.size() == padded.size());
    double worst = 0.0;
    for (std::size_t b = 0; b < narrow.size(); ++b)
        worst = std::max(worst, std::abs(narrow[b] - padded[b]));
    INFO("worst per-bin difference " << worst);
    CHECK(worst == 0.0);
}
