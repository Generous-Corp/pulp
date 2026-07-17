// Tests for the metallic PulpKit voices: closed hat, open hat, cymbal (the
// shared six-oscillator cluster) and the two-oscillator cowbell.
//
// Every claim these voices make is a measured number, not a smoke test:
//   * The three hi-hat/cymbal voices share one tone circuit and differ only in
//     decay, so the test renders each and measures its T60 off the energy
//     envelope -- the whole design rests on closed < open < cymbal.
//   * The same voices darken as they ring, because the bright noise "chiff" is
//     transient while the metallic cluster sustains; the test measures the
//     spectral centroid of each and asserts the monotonic closed > open >
//     cymbal ordering that a uniform VCA could not produce.
//   * The closed hat chokes the open hat; the test renders an open hat, chokes
//     it mid-ring, and measures that its tail collapses.
//   * The cowbell is tonal, so the test reads its two oscillator partials back
//     out of the spectrum.
//   * Every voice's render stays inside its declared peak bound, and the
//     audio-thread path (trigger / choke / process) allocates nothing.
//
// Centroid is measured with a small self-contained radix-2 FFT so the test
// depends only on pulp::signal and the voice headers -- no analysis library.
// The cowbell partials are read with an exact-frequency Goertzel, which needs
// no FFT bin to land on the partial.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include "pulp-kit/voices/cowbell_voice.hpp"
#include "pulp-kit/voices/metallic_voice.hpp"

#include <cmath>
#include <complex>
#include <vector>

namespace {

using pulp::examples::CowbellConfig;
using pulp::examples::CowbellVoice;
using pulp::examples::MetallicConfig;
using pulp::examples::MetallicVoice;

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// ── Render helpers ─────────────────────────────────────────────────────────
// Each voice's oscillator cluster free-runs, so the render lets it settle for
// a moment before the strike, exactly as a live host would leave the voice
// running between notes.

template <typename Voice, typename Config>
std::vector<float> render(const Config& cfg, double seconds, float velocity = 1.0f) {
    Voice v;
    v.prepare(kFs);
    v.set_config(cfg);
    v.reset();
    for (int i = 0; i < 2000; ++i) (void)v.process();
    v.trigger(velocity);
    const std::size_t n = static_cast<std::size_t>(seconds * kFs);
    std::vector<float> y(n);
    for (std::size_t i = 0; i < n; ++i) y[i] = v.process();
    return y;
}

// ── Spectral centroid via a small radix-2 FFT ──────────────────────────────

void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Hann-windowed power-of-two FFT of the render, capped so a long cymbal tail
// does not blow the transform up; returns the linear spectral centroid in Hz.
double spectral_centroid(const std::vector<float>& x) {
    std::size_t N = 1;
    while (N < x.size()) N <<= 1;
    if (N > (1u << 18)) N = 1u << 18;
    std::vector<std::complex<double>> a(N, {0.0, 0.0});
    for (std::size_t n = 0; n < N && n < x.size(); ++n) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (N - 1));
        a[n] = {x[n] * win, 0.0};
    }
    fft(a);
    const double bin_hz = kFs / static_cast<double>(N);
    double num = 0.0, den = 0.0;
    for (std::size_t k = 1; k < N / 2; ++k) {
        const double f = k * bin_hz;
        if (f < 50.0 || f > 20000.0) continue;
        const double m = std::abs(a[k]);
        num += f * m;
        den += m;
    }
    return den > 0.0 ? num / den : 0.0;
}

// ── Exact-frequency Goertzel (for the cowbell partials) ────────────────────

double goertzel(const std::vector<float>& x, double f) {
    const std::size_t N = x.size();
    const double w = 2.0 * kPi * f / kFs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t n = 0; n < N; ++n) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * n / (N - 1));
        const double s0 = coeff * s1 - s2 + win * static_cast<double>(x[n]);
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (N * 0.5);
}

double db(double num, double den) { return 20.0 * std::log10(num / (den + 1e-20)); }

// ── T60 off the energy envelope ────────────────────────────────────────────
// The noise voices have no single sinusoid to track, so decay is measured from
// the broadband RMS envelope: fit a line to the log-RMS over the -3..-30 dB
// window below the peak and extrapolate the slope to -60 dB.

double energy_t60(const std::vector<float>& x) {
    const int win = 480, hop = 240;
    std::vector<double> t, rms;
    double peak = 0.0;
    for (std::size_t i = 0; i + win < x.size(); i += hop) {
        double e = 0.0;
        for (int n = 0; n < win; ++n) e += static_cast<double>(x[i + n]) * x[i + n];
        const double r = std::sqrt(e / win);
        peak = std::max(peak, r);
        t.push_back((i + win / 2.0) / kFs);
        rms.push_back(r);
    }
    const double peak_db = 20.0 * std::log10(peak + 1e-20);
    std::vector<double> ft, fy;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const double d = 20.0 * std::log10(rms[i] + 1e-20) - peak_db;
        if (d <= -3.0 && d >= -30.0) {
            ft.push_back(t[i]);
            fy.push_back(20.0 * std::log10(rms[i] + 1e-20));
        }
    }
    if (ft.size() < 3) return -1.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const int m = static_cast<int>(ft.size());
    for (int i = 0; i < m; ++i) {
        sx += ft[i];
        sy += fy[i];
        sxx += ft[i] * ft[i];
        sxy += ft[i] * fy[i];
    }
    const double slope = (m * sxy - sx * sy) / (m * sxx - sx * sx);
    if (slope >= 0.0) return -1.0;
    return -60.0 / slope;
}

double rms_window(const std::vector<float>& x, std::size_t a, std::size_t b) {
    double e = 0.0;
    for (std::size_t i = a; i < b && i < x.size(); ++i) e += static_cast<double>(x[i]) * x[i];
    return std::sqrt(e / (b - a));
}

double peak_abs(const std::vector<float>& x) {
    double p = 0.0;
    for (float s : x) p = std::max<double>(p, std::fabs(s));
    return p;
}

}  // namespace

TEST_CASE("Metallic voices decay closed < open < cymbal", "[kit][metallic]") {
    const double t_closed = energy_t60(render<MetallicVoice>(pulp::examples::closed_hat_config(), 0.6));
    const double t_open = energy_t60(render<MetallicVoice>(pulp::examples::open_hat_config(), 1.2));
    const double t_cymbal = energy_t60(render<MetallicVoice>(pulp::examples::cymbal_config(), 3.5));

    INFO("T60 closed=" << t_closed << " open=" << t_open << " cymbal=" << t_cymbal);
    REQUIRE(t_closed > 0.0);
    REQUIRE(t_open > 0.0);
    REQUIRE(t_cymbal > 0.0);

    // The strict ordering is the whole point: one tone circuit, three decays.
    REQUIRE(t_closed < t_open);
    REQUIRE(t_open < t_cymbal);

    // And they land near the reference's measured decay times, not merely in
    // order: closed ~0.13 s, open ~0.43 s, cymbal ~2.6 s.
    REQUIRE(t_closed == Catch::Approx(0.130).margin(0.05));
    REQUIRE(t_open == Catch::Approx(0.433).margin(0.08));
    REQUIRE(t_cymbal == Catch::Approx(2.606).margin(0.3));
}

TEST_CASE("Metallic voices darken as they ring (centroid closed > open > cymbal)",
          "[kit][metallic]") {
    const double c_closed = spectral_centroid(render<MetallicVoice>(pulp::examples::closed_hat_config(), 0.6));
    const double c_open = spectral_centroid(render<MetallicVoice>(pulp::examples::open_hat_config(), 1.2));
    const double c_cymbal = spectral_centroid(render<MetallicVoice>(pulp::examples::cymbal_config(), 3.5));

    INFO("centroid closed=" << c_closed << " open=" << c_open << " cymbal=" << c_cymbal);

    // The transient noise chiff makes a short note read brighter than a long
    // one even though the tone source is identical -- a monotonic darkening a
    // uniform VCA cannot reproduce.
    REQUIRE(c_closed > c_open);
    REQUIRE(c_open > c_cymbal);
    // The cymbal is clearly the darkest, by more than a kilohertz.
    REQUIRE(c_open - c_cymbal > 1000.0);

    // In the reference's ballpark: hats are bright (~10-12 kHz), cymbal ~8 kHz.
    REQUIRE(c_closed == Catch::Approx(11700.0).margin(1500.0));
    REQUIRE(c_cymbal == Catch::Approx(7750.0).margin(1200.0));
}

TEST_CASE("Closed hat chokes a ringing open hat", "[kit][metallic]") {
    MetallicVoice free_ring, choked;
    free_ring.prepare(kFs);
    free_ring.set_config(pulp::examples::open_hat_config());
    free_ring.reset();
    choked.prepare(kFs);
    choked.set_config(pulp::examples::open_hat_config());
    choked.reset();
    for (int i = 0; i < 2000; ++i) {
        (void)free_ring.process();
        (void)choked.process();
    }
    free_ring.trigger(1.0f);
    choked.trigger(1.0f);

    const int choke_at = static_cast<int>(0.08 * kFs);
    const int N = static_cast<int>(0.4 * kFs);
    std::vector<float> a(N), b(N);
    for (int i = 0; i < N; ++i) {
        if (i == choke_at) choked.choke();  // the closed hat fires
        a[i] = free_ring.process();
        b[i] = choked.process();
    }

    // In a window well after the choke, the free-ringing open hat is still
    // audible while the choked one has collapsed to near silence.
    const std::size_t w0 = static_cast<std::size_t>(0.11 * kFs);
    const std::size_t w1 = static_cast<std::size_t>(0.16 * kFs);
    const double free_rms = rms_window(a, w0, w1);
    const double choked_rms = rms_window(b, w0, w1);
    INFO("post-choke RMS free=" << free_rms << " choked=" << choked_rms);
    REQUIRE(free_rms > 1e-3);
    REQUIRE(choked_rms < free_rms * 0.05);
}

TEST_CASE("Cowbell decays to the reference time and shows its two partials",
          "[kit][cowbell]") {
    const CowbellConfig cfg = pulp::examples::cowbell_config();
    const std::vector<float> y = render<CowbellVoice>(cfg, 1.5);

    // Decay near the reference's ~0.98 s.
    const double t = energy_t60(y);
    INFO("cowbell T60=" << t);
    REQUIRE(t == Catch::Approx(0.981).margin(0.12));

    // Both oscillator fundamentals are present and tower over the floor between
    // them -- the cowbell is a clear two-tone, not a noise burst.
    const double floor = goertzel(y, (cfg.low_hz + cfg.high_hz) * 0.5 + 120.0);
    const double low = goertzel(y, cfg.low_hz);
    const double high = goertzel(y, cfg.high_hz);
    INFO("cowbell low " << cfg.low_hz << "Hz " << db(low, floor)
                        << "dB, high " << cfg.high_hz << "Hz " << db(high, floor) << "dB");
    REQUIRE(db(low, floor) > 20.0);
    REQUIRE(db(high, floor) > 20.0);
    // The high oscillator is voiced above the low, as the reference measures.
    REQUIRE(high > low);
}

TEST_CASE("Metallic and cowbell renders stay inside their peak bound",
          "[kit][metallic][cowbell]") {
    struct Case {
        const char* name;
        std::vector<float> render;
        float bound;
    };

    auto metallic_bound = [](const MetallicConfig& cfg) {
        MetallicVoice v;
        v.prepare(kFs);
        v.set_config(cfg);
        return v.peak_bound();
    };
    auto cowbell_bound = [](const CowbellConfig& cfg) {
        CowbellVoice v;
        v.prepare(kFs);
        v.set_config(cfg);
        return v.peak_bound();
    };

    const std::vector<Case> cases = {
        {"closed hat", render<MetallicVoice>(pulp::examples::closed_hat_config(), 0.6),
         metallic_bound(pulp::examples::closed_hat_config())},
        {"open hat", render<MetallicVoice>(pulp::examples::open_hat_config(), 1.2),
         metallic_bound(pulp::examples::open_hat_config())},
        {"cymbal", render<MetallicVoice>(pulp::examples::cymbal_config(), 3.5),
         metallic_bound(pulp::examples::cymbal_config())},
        {"cowbell", render<CowbellVoice>(pulp::examples::cowbell_config(), 1.5),
         cowbell_bound(pulp::examples::cowbell_config())},
    };

    for (const auto& c : cases) {
        const double peak = peak_abs(c.render);
        INFO(c.name << " peak=" << peak << " bound=" << c.bound);
        REQUIRE(peak <= c.bound + 1e-4);
        // The bound is a real bound, not a vacuously large one.
        REQUIRE(peak > 0.2 * c.bound);
    }
}

TEST_CASE("Metallic voice audio path allocates nothing", "[kit][metallic][rt-safety]") {
    MetallicVoice v;
    v.prepare(kFs);
    v.set_config(pulp::examples::open_hat_config());
    v.reset();

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            if (block % 40 == 0) v.trigger(0.9f);
            if (block % 55 == 0) v.choke();
            v.set_t60(0.3f + 0.001f * (block % 100));
            v.set_gain(0.15f);
            for (int n = 0; n < 64; ++n) (void)v.process();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Cowbell voice audio path allocates nothing", "[kit][cowbell][rt-safety]") {
    CowbellVoice v;
    v.prepare(kFs);
    v.set_config(pulp::examples::cowbell_config());
    v.reset();

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            if (block % 40 == 0) v.trigger(0.9f);
            v.set_t60(0.8f + 0.001f * (block % 100));
            v.set_gain(0.18f);
            for (int n = 0; n < 64; ++n) (void)v.process();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}
