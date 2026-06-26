// OfflineStretch tests.
//
// These tests pin the contract and quality bands for the implemented engine:
//   - exact output length = round(in_frames * time_ratio) (loop grid-lock);
//   - R=1, pitch 0 is a perfect null against the input;
//   - deterministic renders for independent runs;
//   - tempo/pitch/repitch quality bounds;
//   - the process() contract rejects misuse before writing output.
// Output quality is not yet compared against an external reference renderer.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/offline_stretch.hpp>
#include <pulp/signal/transient_phase_policy.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using pulp::signal::OfflineFormantMode;
using pulp::signal::OfflineStretch;
using pulp::signal::OfflineStretchOptions;
using pulp::signal::offline_stretch_output_frames;

namespace {

std::vector<float> ramp(long n, float start = -0.9f, float step = 0.0011f) {
    std::vector<float> v(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) v[static_cast<size_t>(i)] = start + step * static_cast<float>(i);
    return v;
}

} // namespace

TEST_CASE("offline_stretch_output_frames is exact round(N*R)", "[offline-stretch]") {
    CHECK(offline_stretch_output_frames(1000, 1.0) == 1000);
    CHECK(offline_stretch_output_frames(1000, 1.25) == 1250);
    CHECK(offline_stretch_output_frames(1000, 0.5) == 500);
    CHECK(offline_stretch_output_frames(2000, 2.0) == 4000);
    // Awkward primes / non-integer products round to nearest.
    CHECK(offline_stretch_output_frames(997, 1.5) == 1496);   // 1495.5 -> 1496
    CHECK(offline_stretch_output_frames(1000, 1.0 / 3.0) == 333); // 333.33 -> 333
    // Degenerate guards.
    CHECK(offline_stretch_output_frames(0, 1.5) == 0);
    CHECK(offline_stretch_output_frames(-5, 1.5) == 0);
    CHECK(offline_stretch_output_frames(100, 0.0) == 0);
}

TEST_CASE("R=1 pitch=0 is a perfect null (mono and stereo)", "[offline-stretch]") {
    OfflineStretch s;
    s.prepare(48000.0, 2);

    const long n = 1024;
    std::vector<float> l = ramp(n), r = ramp(n, 0.5f, -0.0013f);
    const float* in[2] = {l.data(), r.data()};

    std::vector<float> ol(n), orr(n);
    float* out[2] = {ol.data(), orr.data()};

    OfflineStretchOptions opts; // defaults: time_ratio 1, pitch 0
    REQUIRE(offline_stretch_output_frames(n, opts.time_ratio) == n);

    std::string err;
    REQUIRE(s.process(in, n, out, n, opts, &err));
    for (long i = 0; i < n; ++i) {
        REQUIRE(ol[static_cast<size_t>(i)] == l[static_cast<size_t>(i)]);
        REQUIRE(orr[static_cast<size_t>(i)] == r[static_cast<size_t>(i)]);
    }
}

TEST_CASE("process writes exactly the contracted output length", "[offline-stretch]") {
    OfflineStretch s;
    s.prepare(44100.0, 1);

    const long n = 991; // prime
    std::vector<float> in = ramp(n);
    const float* inp[1] = {in.data()};

    OfflineStretchOptions opts;
    opts.time_ratio = 1.25;
    const long expected = offline_stretch_output_frames(n, opts.time_ratio);
    CHECK(expected == 1239); // round(1238.75)

    std::vector<float> out(static_cast<size_t>(expected), 1234.0f);
    float* outp[1] = {out.data()};

    std::string err;
    REQUIRE(s.process(inp, n, outp, expected, opts, &err));
    // The render must have written every output sample (no stale fill).
    bool any_stale = false;
    for (float v : out) if (v == 1234.0f) { any_stale = true; break; }
    CHECK_FALSE(any_stale);
}

TEST_CASE("process rejects contract violations", "[offline-stretch]") {
    const long n = 256;
    std::vector<float> in = ramp(n), out(n);
    const float* inp[1] = {in.data()};
    float* outp[1] = {out.data()};
    OfflineStretchOptions opts;

    SECTION("unprepared") {
        OfflineStretch s;
        std::string err;
        CHECK_FALSE(s.process(inp, n, outp, n, opts, &err));
        CHECK_FALSE(err.empty());
    }
    SECTION("wrong out_frames") {
        OfflineStretch s;
        s.prepare(48000.0, 1);
        std::string err;
        CHECK_FALSE(s.process(inp, n, outp, n + 1, opts, &err)); // R=1 expects n
        CHECK_FALSE(err.empty());
    }
    SECTION("non-positive ratio") {
        OfflineStretch s;
        s.prepare(48000.0, 1);
        opts.time_ratio = 0.0;
        std::string err;
        CHECK_FALSE(s.process(inp, n, outp, 0, opts, &err));
    }
}

TEST_CASE("process rejects ratios/pitch outside the prepared range", "[offline-stretch]") {
    const long n = 256;
    std::vector<float> in = ramp(n);
    const float* inp[1] = {in.data()};

    SECTION("default range rejects > max_time_ratio, never silently clamps") {
        OfflineStretch s;
        s.prepare(48000.0, 1); // default sizing: max_time_ratio 4.0
        OfflineStretchOptions opts;
        opts.time_ratio = 5.0; // beyond 4×
        const long expected = offline_stretch_output_frames(n, opts.time_ratio);
        std::vector<float> out(static_cast<size_t>(expected));
        float* outp[1] = {out.data()};
        std::string err;
        CHECK_FALSE(s.process(inp, n, outp, expected, opts, &err));
        CHECK(err.find("time_ratio") != std::string::npos);
    }

    SECTION("widening max_time_ratio at prepare() admits the same ratio") {
        OfflineStretch s;
        OfflineStretchOptions sizing;
        sizing.max_time_ratio = 8.0;
        s.prepare(48000.0, 1, sizing);
        OfflineStretchOptions opts;
        opts.time_ratio = 5.0;
        const long expected = offline_stretch_output_frames(n, opts.time_ratio);
        std::vector<float> out(static_cast<size_t>(expected));
        float* outp[1] = {out.data()};
        std::string err;
        REQUIRE(s.process(inp, n, outp, expected, opts, &err));
    }

    SECTION("pitch beyond prepared max is rejected") {
        OfflineStretch s;
        s.prepare(48000.0, 1); // default max_pitch_semitones 24
        OfflineStretchOptions opts;
        opts.pitch_semitones = 36.0;
        std::vector<float> out(n);
        float* outp[1] = {out.data()};
        std::string err;
        CHECK_FALSE(s.process(inp, n, outp, n, opts, &err));
        CHECK(err.find("pitch") != std::string::npos);
    }
}

TEST_CASE("repitch_linked: exact length, R=1 identity, sine tracks i/ratio", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, f = 1000.0, w = 2.0 * pi * f / sr;
    const long n = 8192;
    std::vector<float> in(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    const float* inp[1] = {in.data()};

    OfflineStretch s;
    s.prepare(sr, 1);

    SECTION("R=1 is an exact identity") {
        OfflineStretchOptions o; o.repitch_linked = true;
        std::vector<float> out(static_cast<size_t>(n));
        float* outp[1] = {out.data()};
        std::string err;
        REQUIRE(s.process(inp, n, outp, n, o, &err));
        double e = 0; for (long i = 0; i < n; ++i) { double d = out[i] - in[i]; e += d * d; }
        CHECK(std::sqrt(e / n) < 1e-6);
    }

    SECTION("R=1.5 reads a continuous sine at position i/ratio") {
        OfflineStretchOptions o; o.repitch_linked = true; o.time_ratio = 1.5;
        const long m = offline_stretch_output_frames(n, 1.5);
        REQUIRE(m == 12288);
        std::vector<float> out(static_cast<size_t>(m));
        float* outp[1] = {out.data()};
        std::string err;
        REQUIRE(s.process(inp, n, outp, m, o, &err));
        // Interior only — edges read zero-padded taps and bias sinc6.
        double e = 0; long cnt = 0;
        for (long i = 64; i < m - 64; ++i) {
            const double ref = 0.5 * std::sin(w * (static_cast<double>(i) / 1.5));
            const double d = out[static_cast<size_t>(i)] - ref;
            e += d * d; ++cnt;
        }
        // sinc6 is a 6-tap windowed sinc: ~-48 dB passband accuracy on a 1 kHz
        // tone. Confirms repitch reads the correct positions; upgrading repitch
        // to the 96 dB Kaiser-sinc Resampler remains a quality improvement.
        CHECK(std::sqrt(e / cnt) < 2.5e-3);
    }
}

TEST_CASE("tempo-only: exact length, pitch preserved (sine stays ~1 kHz)", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, f = 1000.0, w = 2.0 * pi * f / sr;
    const long n = 48000; // 1 second
    std::vector<float> in(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    const float* inp[1] = {in.data()};

    OfflineStretch s;
    s.prepare(sr, 1);
    OfflineStretchOptions o; o.time_ratio = 1.5; // tempo only, pitch 0
    const long m = offline_stretch_output_frames(n, 1.5);
    REQUIRE(m == 72000);
    std::vector<float> out(static_cast<size_t>(m));
    float* outp[1] = {out.data()};
    std::string err;
    REQUIRE(s.process(inp, n, outp, m, o, &err));

    // Zero-crossing rate over the interior -> frequency. Tempo-stretch PRESERVES
    // pitch (~1 kHz); repitch would have dropped it to 1000/1.5 = 667 Hz.
    long zc = 0;
    const long lo = 4800, hi = m - 4800;
    for (long i = lo + 1; i < hi; ++i)
        if ((out[static_cast<size_t>(i - 1)] <= 0.0f) != (out[static_cast<size_t>(i)] <= 0.0f)) ++zc;
    const double dur = static_cast<double>(hi - lo - 1) / sr;
    const double freq = zc / (2.0 * dur);
    CHECK(std::abs(freq - 1000.0) < 40.0); // pitch preserved within ~4%
    CHECK(freq > 850.0);                    // definitely not the repitched 667 Hz
}

TEST_CASE("transient refractory gate collapses a hit's decay to one reset",
          "[offline-stretch][transient]") {
    using pulp::signal::TransientPhasePolicy;
    const int fft = 256, bins = fft / 2 + 1;
    // Feed steady warmup frames (no flux), then a "ringing hit": frames whose
    // magnitude keeps rising so the detector WOULD fire on every frame. Without a
    // refractory window it re-fires through the whole decay (the over-firing that
    // bleeds the vocoder's synthesis-phase lead and blows out deep hits); with the
    // gate it fires once at the onset and suppresses the rest.
    auto count_fires = [&](int refractory) {
        TransientPhasePolicy p;
        TransientPhasePolicy::Config c;
        c.fft_size = fft;
        c.refractory_frames = refractory;
        p.prepare(c);
        std::vector<std::complex<float>> buf(static_cast<size_t>(bins));
        const std::complex<float>* frames[1] = {buf.data()};
        for (int f = 0; f < 12; ++f) {                       // warmup: steady, flux -> 0
            std::fill(buf.begin(), buf.end(), std::complex<float>(1.0f, 0.0f));
            p.analyze(frames, 1, bins);
        }
        int fires = 0;
        float mag = 1.0f;
        for (int f = 0; f < 12; ++f) {                       // ringing hit: rising magnitude
            mag *= 1.7f;
            std::fill(buf.begin(), buf.end(), std::complex<float>(mag, 0.0f));
            if (p.analyze(frames, 1, bins) > 0.0f) ++fires;
        }
        return fires;
    };
    const int legacy = count_fires(0);  // no gate -> fires through the whole decay
    const int gated = count_fires(3);   // gate -> a fraction of that
    CHECK(legacy >= 8);                 // confirms the over-firing condition exists
    CHECK(gated >= 1);                  // still fires at the onset (transient preserved)
    CHECK(gated <= legacy / 2);         // refractory collapses the decay re-fires
}

TEST_CASE("verbatim relocation grafts only the high band (low end stays clean PV)",
          "[offline-stretch][transient]") {
    // The transient graft re-injects only the HIGH-frequency attack; the low end is
    // left to the continuous phase vocoder. (Grafting full-band low frequencies onto
    // the phase-mismatched PV body across the short seam blew out deep kicks at
    // stretch.) Guard it: relocate-on vs relocate-off must differ ONLY above the
    // crossover — their low-passed outputs should be nearly identical.
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0;
    const long n = 48000;
    // Low sustained tone (80 Hz, below the graft crossover) + periodic broadband
    // clicks (fire the onset detector so the graft runs).
    std::vector<float> in(static_cast<size_t>(n), 0.0f);
    for (long i = 0; i < n; ++i)
        in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * pi * 80.0 * i / sr));
    for (long c = 0; c < n; c += 8000) {                 // clicks every ~167 ms
        if (c < n) in[static_cast<size_t>(c)] += 0.9f;
        if (c + 1 < n) in[static_cast<size_t>(c + 1)] -= 0.8f;
    }
    const float* inp[1] = {in.data()};

    const long m = offline_stretch_output_frames(n, 1.5);
    auto render = [&](bool relocate) {
        OfflineStretch s; s.prepare(sr, 1);
        OfflineStretchOptions o; o.time_ratio = 1.5; o.relocate_transients = relocate;
        std::vector<float> out(static_cast<size_t>(m));
        float* outp[1] = {out.data()};
        std::string err;
        REQUIRE(s.process(inp, n, outp, m, o, &err));
        return out;
    };
    const std::vector<float> on = render(true);
    const std::vector<float> off = render(false);

    // Low-pass the difference at 150 Hz (below the 300 Hz graft crossover). A graft
    // that touched the low band would leave low-frequency residue here.
    std::vector<float> diff(static_cast<size_t>(m));
    for (long i = 0; i < m; ++i) diff[static_cast<size_t>(i)] = on[static_cast<size_t>(i)] - off[static_cast<size_t>(i)];
    const double f0 = 150.0, q = 0.7071, w0 = 2.0 * pi * f0 / sr;
    const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * q);
    const double b0 = (1.0 - cw) * 0.5, b1 = 1.0 - cw, b2 = (1.0 - cw) * 0.5;
    const double a0 = 1.0 + alpha, a1 = -2.0 * cw, a2 = 1.0 - alpha;
    const double nb0 = b0 / a0, nb1 = b1 / a0, nb2 = b2 / a0, na1 = a1 / a0, na2 = a2 / a0;
    double z1 = 0.0, z2 = 0.0, lo_diff = 0.0, tot = 0.0;
    for (long i = 0; i < m; ++i) {
        const double x = diff[static_cast<size_t>(i)];
        const double y = nb0 * x + z1; z1 = nb1 * x - na1 * y + z2; z2 = nb2 * x - na2 * y;
        lo_diff += y * y;
        tot += static_cast<double>(off[static_cast<size_t>(i)]) * off[static_cast<size_t>(i)];
    }
    // The low band of the graft's effect is negligible vs the signal energy.
    CHECK(std::sqrt(lo_diff / (tot + 1e-12)) < 0.02);
}

TEST_CASE("tempo-stretch makes up the PV energy loss without clipping",
          "[offline-stretch][energy]") {
    const double sr = 48000.0;
    const long n = 48000;
    auto rms = [](const float* x, long lo, long hi) {
        double s = 0.0; for (long i = lo; i < hi; ++i) s += static_cast<double>(x[i]) * x[i];
        return hi > lo ? std::sqrt(s / static_cast<double>(hi - lo)) : 0.0;
    };

    // Broadband material (deterministic LCG noise) — the phase vocoder's incoherent
    // overlap loses the most energy here, so the make-up has the most to restore.
    std::vector<float> noise(static_cast<size_t>(n));
    unsigned long st = 99991UL;
    for (long i = 0; i < n; ++i) {
        st = st * 1103515245UL + 12345UL;
        noise[static_cast<size_t>(i)] = (static_cast<float>((st >> 16) & 0x7fff) / 16384.0f - 1.0f) * 0.4f;
    }
    const float* inp[1] = {noise.data()};
    const double ri = rms(noise.data(), 8000, n - 8000);
    for (double R : {0.75, 1.5, 2.0}) {
        OfflineStretch s; s.prepare(sr, 1);
        OfflineStretchOptions o; o.time_ratio = R;
        const long m = offline_stretch_output_frames(n, R);
        std::vector<float> out(static_cast<size_t>(m));
        float* op[1] = {out.data()};
        std::string err;
        REQUIRE(s.process(inp, n, op, m, o, &err));
        const double ro = rms(out.data(), 8000, m - 8000);
        const double db = 20.0 * std::log10(ro / ri);
        INFO("ratio " << R << " RMS " << db << " dB vs source");
        CHECK(std::abs(db) < 1.5);                 // energy restored to ~source (was -3..-4 dB)
        float peak = 0.0f; for (float v : out) peak = std::max(peak, std::fabs(v));
        CHECK(peak <= 1.0f);                        // never clips (soft-clip ceiling)
    }

    // Coherent material (pure sine) reconstructs at full level already, so the make-up
    // must be ~unity — it must NOT inflate a tonal signal.
    std::vector<float> sine(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i)
        sine[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 * i / sr));
    const float* sp[1] = {sine.data()};
    OfflineStretch s; s.prepare(sr, 1);
    OfflineStretchOptions o; o.time_ratio = 1.5;
    const long m = offline_stretch_output_frames(n, 1.5);
    std::vector<float> out(static_cast<size_t>(m));
    float* op[1] = {out.data()};
    std::string err;
    REQUIRE(s.process(sp, n, op, m, o, &err));
    const double rs_in = rms(sine.data(), 8000, n - 8000);
    const double rs_out = rms(out.data(), 8000, m - 8000);
    CHECK(std::abs(20.0 * std::log10(rs_out / rs_in)) < 1.5);  // tonal level preserved, not inflated
}

TEST_CASE("tempo-only: stereo channel coherence (identical L/R stay identical)", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, w = 2.0 * pi * 1000.0 / sr;
    const long n = 24000;
    std::vector<float> a(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) a[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    std::vector<float> b = a; // identical L and R
    const float* inp[2] = {a.data(), b.data()};

    OfflineStretch s;
    s.prepare(sr, 2);
    OfflineStretchOptions o; o.time_ratio = 1.3;
    const long m = offline_stretch_output_frames(n, 1.3);
    std::vector<float> ol(static_cast<size_t>(m)), orr(static_cast<size_t>(m));
    float* outp[2] = {ol.data(), orr.data()};
    std::string err;
    REQUIRE(s.process(inp, n, outp, m, o, &err));

    // Coherent multichannel processing: identical inputs -> identical outputs.
    double e = 0; for (long i = 0; i < m; ++i) { double d = ol[i] - orr[i]; e += d * d; }
    CHECK(std::sqrt(e / m) < 1e-7);
}

TEST_CASE("tempo path is safe and deterministic", "[offline-stretch]") {
    const double sr = 48000.0;

    SECTION("silence in -> silence out") {
        OfflineStretch s; s.prepare(sr, 1);
        const long n = 20000;
        std::vector<float> in(static_cast<size_t>(n), 0.0f);
        const float* ip[1] = {in.data()};
        OfflineStretchOptions o; o.time_ratio = 1.4;
        const long m = offline_stretch_output_frames(n, 1.4);
        std::vector<float> out(static_cast<size_t>(m), 1.0f);
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, m, o, &e));
        float peak = 0.0f; for (float v : out) peak = std::max(peak, std::fabs(v));
        CHECK(peak < 1e-6f);
    }

    SECTION("full-scale input -> finite output (no NaN/Inf)") {
        OfflineStretch s; s.prepare(sr, 1);
        const long n = 16000;
        std::vector<float> in(static_cast<size_t>(n));
        for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = (i % 2) ? 1.0f : -1.0f;
        const float* ip[1] = {in.data()};
        OfflineStretchOptions o; o.time_ratio = 1.7;
        const long m = offline_stretch_output_frames(n, 1.7);
        std::vector<float> out(static_cast<size_t>(m));
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, m, o, &e));
        for (float v : out) REQUIRE(std::isfinite(v));
    }

    SECTION("deterministic: two independent runs are byte-identical") {
        const long n = 12000;
        std::vector<float> in(static_cast<size_t>(n));
        for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(0.05 * i));
        const float* ip[1] = {in.data()};
        OfflineStretchOptions o; o.time_ratio = 1.25;
        const long m = offline_stretch_output_frames(n, 1.25);
        std::vector<float> a(static_cast<size_t>(m)), b(static_cast<size_t>(m));
        float* pa[1] = {a.data()}; float* pb[1] = {b.data()};
        std::string e;
        OfflineStretch s1; s1.prepare(sr, 1); REQUIRE(s1.process(ip, n, pa, m, o, &e));
        OfflineStretch s2; s2.prepare(sr, 1); REQUIRE(s2.process(ip, n, pb, m, o, &e));
        CHECK(a == b);
    }

    SECTION("supported sample rates render at exact length") {
        for (double rate : {44100.0, 48000.0, 96000.0, 192000.0}) {
            OfflineStretch s; s.prepare(rate, 1);
            const long n = 8000;
            std::vector<float> in(static_cast<size_t>(n));
            for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.3 * std::sin(0.03 * i));
            const float* ip[1] = {in.data()};
            OfflineStretchOptions o; o.time_ratio = 1.5;
            const long m = offline_stretch_output_frames(n, 1.5);
            std::vector<float> out(static_cast<size_t>(m));
            float* op[1] = {out.data()};
            std::string e;
            REQUIRE(s.process(ip, n, op, m, o, &e));
            for (float v : out) REQUIRE(std::isfinite(v));
        }
    }
}

TEST_CASE("pitch-only: duration preserved, pitch shifts by semitones", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, f0 = 500.0, w = 2.0 * pi * f0 / sr;
    const long n = 48000;
    std::vector<float> in(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    const float* ip[1] = {in.data()};

    OfflineStretch s; s.prepare(sr, 1);
    auto dominant_freq = [&](const std::vector<float>& x) {
        long zc = 0; const long lo = 6000, hi = static_cast<long>(x.size()) - 6000;
        for (long i = lo + 1; i < hi; ++i)
            if ((x[static_cast<size_t>(i - 1)] <= 0.0f) != (x[static_cast<size_t>(i)] <= 0.0f)) ++zc;
        return zc / (2.0 * (static_cast<double>(hi - lo - 1) / sr));
    };

    SECTION("+12 semitones doubles the frequency, length unchanged") {
        OfflineStretchOptions o; o.pitch_semitones = 12.0; // time_ratio 1
        std::vector<float> out(static_cast<size_t>(n));
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, n, o, &e));
        CHECK(std::abs(dominant_freq(out) - 1000.0) < 40.0); // 500 -> 1000
    }
    SECTION("-12 semitones halves the frequency") {
        OfflineStretchOptions o; o.pitch_semitones = -12.0;
        std::vector<float> out(static_cast<size_t>(n));
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, n, o, &e));
        CHECK(std::abs(dominant_freq(out) - 250.0) < 20.0); // 500 -> 250
    }
}

TEST_CASE("independent R+S: exact length and shifted pitch", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, f0 = 500.0, w = 2.0 * pi * f0 / sr;
    const long n = 48000;
    std::vector<float> in(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    const float* ip[1] = {in.data()};

    OfflineStretch s; s.prepare(sr, 1);
    auto dom = [&](const std::vector<float>& x) {
        long zc = 0; const long lo = 8000, hi = static_cast<long>(x.size()) - 8000;
        for (long i = lo + 1; i < hi; ++i)
            if ((x[static_cast<size_t>(i - 1)] <= 0.0f) != (x[static_cast<size_t>(i)] <= 0.0f)) ++zc;
        return zc / (2.0 * (static_cast<double>(hi - lo - 1) / sr));
    };

    SECTION("follow formant: R=1.5, +12 st (single-pass)") {
        OfflineStretchOptions o; o.time_ratio = 1.5; o.pitch_semitones = 12.0;
        o.formant_mode = OfflineFormantMode::follow_pitch;
        const long m = offline_stretch_output_frames(n, 1.5);
        REQUIRE(m == 72000);
        std::vector<float> out(static_cast<size_t>(m));
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, m, o, &e));
        CHECK(std::abs(dom(out) - 1000.0) < 40.0); // pitch 500 -> 1000
    }

    SECTION("preserve formant: R=0.75, +7 st (cascade)") {
        OfflineStretchOptions o; o.time_ratio = 0.75; o.pitch_semitones = 7.0;
        o.formant_mode = OfflineFormantMode::preserve_original;
        const long m = offline_stretch_output_frames(n, 0.75);
        REQUIRE(m == 36000);
        std::vector<float> out(static_cast<size_t>(m));
        float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(ip, n, op, m, o, &e));
        const double target = 500.0 * std::exp2(7.0 / 12.0); // ~749 Hz
        CHECK(std::abs(dom(out) - target) < 40.0);
    }
}

TEST_CASE("STN noise routing: noisy input stretches finite + deterministic", "[offline-stretch]") {
    const long n = 24000;
    std::vector<float> in(static_cast<size_t>(n));
    unsigned long st = 12345UL; // deterministic LCG pseudo-noise
    auto rnd = [&]() { st = st * 1103515245UL + 12345UL; return (static_cast<double>((st >> 16) & 0x7fff) / 16384.0) - 1.0; };
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = 0.3f * static_cast<float>(rnd());
    const float* ip[1] = {in.data()};

    const double sr = 48000.0;
    OfflineStretchOptions o; o.time_ratio = 1.4; o.route_noise_stn = true; // STN path on
    const long m = offline_stretch_output_frames(n, 1.4);
    std::vector<float> a(static_cast<size_t>(m)), b(static_cast<size_t>(m));
    float* pa[1] = {a.data()}; float* pb[1] = {b.data()};
    std::string e;
    OfflineStretch s1; s1.prepare(sr, 1, o); REQUIRE(s1.process(ip, n, pa, m, o, &e));
    OfflineStretch s2; s2.prepare(sr, 1, o); REQUIRE(s2.process(ip, n, pb, m, o, &e));
    for (float v : a) REQUIRE(std::isfinite(v));
    CHECK(a == b); // STN morphing stays deterministic
}

TEST_CASE("draft quality=0: fast OLA tempo, exact length, pitch preserved", "[offline-stretch]") {
    constexpr double pi = 3.14159265358979323846;
    const double sr = 48000.0, f = 1000.0, w = 2.0 * pi * f / sr;
    const long n = 48000;
    std::vector<float> in(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i) in[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(w * i));
    const float* ip[1] = {in.data()};

    OfflineStretch s; s.prepare(sr, 1);
    OfflineStretchOptions o; o.time_ratio = 1.5; o.quality = 0; // draft
    const long m = offline_stretch_output_frames(n, 1.5);
    REQUIRE(m == 72000);
    std::vector<float> out(static_cast<size_t>(m));
    float* op[1] = {out.data()};
    std::string e; REQUIRE(s.process(ip, n, op, m, o, &e));
    for (float v : out) REQUIRE(std::isfinite(v));
    long zc = 0; const long lo = 6000, hi = m - 6000;
    for (long i = lo + 1; i < hi; ++i)
        if ((out[static_cast<size_t>(i - 1)] <= 0.0f) != (out[static_cast<size_t>(i)] <= 0.0f)) ++zc;
    const double fr = zc / (2.0 * (static_cast<double>(hi - lo - 1) / sr));
    // Plain OLA is phase-incoherent: ZCR jitters a few % from boundary
    // discontinuities. The point is pitch is PRESERVED (near 1 kHz), not
    // repitched down to 667 Hz — a generous band confirms that for a draft.
    CHECK(fr > 880.0);
    CHECK(fr < 1120.0);
}

// ── Adaptive STFT window selection (material-aware) ───────────────────────────
// The phase core is fixed at FFT 4096/512; that window is too small to resolve
// closely-spaced low partials (bass wobbles) and too large for percussive time
// resolution (drum attacks soften). recommend_window() picks geometry from the
// input, and prepare() honors an explicit override. These pin the ear-validated
// selection so a future refactor can't silently regress it.
namespace {
constexpr double kPi2 = 6.28318530717958647692;
std::vector<float> sine(long n, double sr, double hz, float amp = 0.7f) {
    std::vector<float> v(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i)
        v[static_cast<size_t>(i)] = amp * static_cast<float>(std::sin(kPi2 * hz * static_cast<double>(i) / sr));
    return v;
}
} // namespace

TEST_CASE("recommend_window picks material-adaptive STFT geometry", "[offline-stretch]") {
    const double sr = 48000.0;
    const long n = static_cast<long>(sr); // 1 s

    SECTION("percussive (high crest) -> 1024: time resolution (matches Python ref)") {
        std::vector<float> perc(static_cast<size_t>(n), 0.0f);
        for (long i = 0; i < n; i += 12000) perc[static_cast<size_t>(i)] = 1.0f; // sparse clicks
        const float* p[1] = {perc.data()};
        const auto w = OfflineStretch::recommend_window(p, n, 1, sr);
        CHECK(w.fft_size == 1024);
        CHECK(w.analysis_hop == 128);
    }
    SECTION("bass / low-fundamental -> large window + 16x overlap") {
        const auto b = sine(n, sr, 60.0, 0.8f);
        const float* p[1] = {b.data()};
        const auto w = OfflineStretch::recommend_window(p, n, 1, sr);
        CHECK(w.fft_size == 8192);
        CHECK(w.analysis_hop == 512);
    }
    SECTION("mid sustained tone -> engine default (no override)") {
        const auto m = sine(n, sr, 2000.0, 0.5f);
        const float* p[1] = {m.data()};
        const auto w = OfflineStretch::recommend_window(p, n, 1, sr);
        CHECK(w.fft_size == 0);
        CHECK(w.analysis_hop == 0);
    }
    SECTION("degenerate input -> default, never crashes") {
        const auto w = OfflineStretch::recommend_window(nullptr, 0, 1, sr);
        CHECK(w.fft_size == 0);
        CHECK(w.analysis_hop == 0);
    }
}

TEST_CASE("prepare honors an explicit fft_size override and still renders", "[offline-stretch]") {
    const double sr = 48000.0;
    OfflineStretch s;
    OfflineStretchOptions sizing;
    sizing.fft_size = 8192;      // force the bass window
    sizing.analysis_hop = 512;
    s.prepare(sr, 1, sizing);
    CHECK(s.fft_size() == 8192);

    const long n = 24000;
    const auto in = sine(n, sr, 80.0, 0.6f);
    const float* inp[1] = {in.data()};
    OfflineStretchOptions o; o.time_ratio = 2.0;
    const long m = offline_stretch_output_frames(n, o.time_ratio);
    std::vector<float> out(static_cast<size_t>(m));
    float* outp[1] = {out.data()};
    std::string err;
    REQUIRE(s.process(inp, n, outp, m, o, &err));
    for (float v : out) REQUIRE(std::isfinite(v));
    // exact length contract holds at the larger window too
    REQUIRE(static_cast<long>(out.size()) == offline_stretch_output_frames(n, o.time_ratio));
}

TEST_CASE("invalid fft override is ignored (falls back to default geometry)", "[offline-stretch]") {
    OfflineStretch s;
    OfflineStretchOptions sizing;
    sizing.fft_size = 3000;       // not a power of two -> must be ignored
    sizing.analysis_hop = 500;
    s.prepare(48000.0, 1, sizing);
    // fft_size() reflects the requested override value; the engine internally
    // rejects the invalid geometry and uses its 4096 default — the render must
    // still succeed and stay finite.
    const long n = 8000;
    const auto in = sine(n, 48000.0, 200.0, 0.5f);
    const float* inp[1] = {in.data()};
    OfflineStretchOptions o; o.time_ratio = 1.5;
    const long m = offline_stretch_output_frames(n, o.time_ratio);
    std::vector<float> out(static_cast<size_t>(m));
    float* outp[1] = {out.data()};
    std::string err;
    REQUIRE(s.process(inp, n, outp, m, o, &err));
    for (float v : out) REQUIRE(std::isfinite(v));
}

// ── Character modes (StretchCharacter) ────────────────────────────────────────
// clean (default) = spectral peak-lock; varispeed = pitch+time-linked resample +
// speed-scaled tape head EQ; phase_vocoder/granular are reserved modes that
// currently render as clean. These pin the API contract + the varispeed tape
// behaviour.
TEST_CASE("varispeed: identity at ratio 1, exact length, tape EQ direction", "[offline-stretch]") {
    using pulp::signal::StretchCharacter;
    const double sr = 48000.0;
    const long n = 24000;
    const auto in = sine(n, sr, 220.0, 0.6f);
    const float* inp[1] = {in.data()};
    OfflineStretch s; s.prepare(sr, 1);

    SECTION("ratio 1.0 is a (near) identity — head EQ bypasses at unity") {
        OfflineStretchOptions o; o.character = StretchCharacter::varispeed; o.time_ratio = 1.0;
        std::vector<float> out(static_cast<size_t>(n)); float* op[1] = {out.data()};
        std::string e; REQUIRE(s.process(inp, n, op, n, o, &e));
        double maxd = 0.0;
        for (long i = 0; i < n; ++i) maxd = std::max(maxd, std::abs((double)out[(size_t)i]-(double)in[(size_t)i]));
        CHECK(maxd < 1e-3); // pure resample at integer positions
    }
    SECTION("ratio != 1: exact length, finite, and slow=darker than fast") {
        // HF brightness proxy: mean |first difference| / mean |sample|. Brighter
        // (more HF) -> higher slew. Robust and FFT-free.
        auto brightness = [&](const std::vector<float>& y) {
            double slew=0, amp=0;
            for (size_t i=1;i<y.size();++i){ slew+=std::abs((double)y[i]-(double)y[i-1]); amp+=std::abs((double)y[i]); }
            return amp>0 ? slew/amp : 0.0;
        };
        OfflineStretchOptions slow; slow.character=StretchCharacter::varispeed; slow.time_ratio=2.0;
        const long ms = offline_stretch_output_frames(n, 2.0);
        std::vector<float> os(static_cast<size_t>(ms)); float* sp[1]={os.data()};
        std::string e; REQUIRE(s.process(inp, n, sp, ms, slow, &e));
        REQUIRE(static_cast<long>(os.size()) == ms);
        for (float v: os) REQUIRE(std::isfinite(v));

        OfflineStretchOptions fast; fast.character=StretchCharacter::varispeed; fast.time_ratio=0.5;
        const long mf = offline_stretch_output_frames(n, 0.5);
        std::vector<float> of(static_cast<size_t>(mf)); float* fp[1]={of.data()};
        REQUIRE(s.process(inp, n, fp, mf, fast, &e));
        // tape slow is duller than tape fast (head-gap HF loss scales with speed)
        CHECK(brightness(os) < brightness(of));
    }
}

TEST_CASE("reserved characters render valid output (clean fallback)", "[offline-stretch]") {
    using pulp::signal::StretchCharacter;
    const double sr = 48000.0; const long n = 12000;
    const auto in = sine(n, sr, 440.0, 0.5f);
    const float* inp[1] = {in.data()};
    OfflineStretch s; s.prepare(sr, 1);
    for (auto ch : {StretchCharacter::phase_vocoder, StretchCharacter::granular}) {
        OfflineStretchOptions o; o.character = ch; o.time_ratio = 1.5; o.relocate_transients = true;
        const long m = offline_stretch_output_frames(n, 1.5);
        std::vector<float> out(static_cast<size_t>(m)); float* op[1]={out.data()};
        std::string e; REQUIRE(s.process(inp, n, op, m, o, &e));
        REQUIRE(static_cast<long>(out.size()) == m);
        for (float v: out) REQUIRE(std::isfinite(v));
    }
}

// ── Fine-tune preset layer (StretchPreset) ────────────────────────────────────
#include <pulp/signal/stretch_preset.hpp>
using pulp::signal::StretchPreset;
using pulp::signal::StretchCharacter;

TEST_CASE("StretchPreset round-trips through text", "[offline-stretch][preset]") {
    StretchPreset p;
    p.name = "My Tape";
    p.character = StretchCharacter::varispeed;
    p.fft_size = 8192; p.analysis_hop = 512;
    p.transient_sensitivity = 1.8f;
    p.route_noise_stn = true;
    p.relocate_transients = true;
    const std::string txt = pulp::signal::preset_to_text(p);
    StretchPreset q; std::string err;
    REQUIRE(pulp::signal::preset_from_text(txt, q, &err));
    CHECK(q.name == "My Tape");
    CHECK(q.character == StretchCharacter::varispeed);
    CHECK(q.fft_size == 8192);
    CHECK(q.analysis_hop == 512);
    CHECK(q.transient_sensitivity == 1.8f);
    CHECK(q.route_noise_stn == true);
    CHECK(q.relocate_transients == true);
}

TEST_CASE("StretchPreset applies to options and captures back", "[offline-stretch][preset]") {
    StretchPreset p; p.character = StretchCharacter::varispeed; p.fft_size = 1024; p.transient_sensitivity = 2.0f;
    OfflineStretchOptions o; o.time_ratio = 2.0; // caller's ratio is NOT a preset field
    pulp::signal::apply_preset(o, p);
    CHECK(o.character == StretchCharacter::varispeed);
    CHECK(o.fft_size == 1024);
    CHECK(o.transient_sensitivity == 2.0f);
    CHECK(o.time_ratio == 2.0); // untouched by the preset
    const StretchPreset back = pulp::signal::capture_preset(o, "roundtrip");
    CHECK(back.character == StretchCharacter::varispeed);
    CHECK(back.fft_size == 1024);
}

TEST_CASE("StretchPreset parsing is tolerant", "[offline-stretch][preset]") {
    StretchPreset p; std::string err;
    const std::string txt =
        "# a comment\n\n  character = varispeed  \n"
        "future_key = whatever\n"   // unknown key ignored
        "route_noise_stn = yes\n";
    REQUIRE(pulp::signal::preset_from_text(txt, p, &err));
    CHECK(p.character == StretchCharacter::varispeed);
    CHECK(p.route_noise_stn == true);
    // malformed character value is a hard error
    StretchPreset bad;
    CHECK_FALSE(pulp::signal::preset_from_text("character = bogus\n", bad, &err));
}

// #110: verbatim transient relocation grafts the original attack back onto the
// phase-vocoder output, restoring the peak the PV smears (the "compressed" sound),
// while leaving tonal material (no detected onsets) bit-identical.
TEST_CASE("verbatim transient relocation restores attack peaks; tonal is a no-op",
          "[offline-stretch][issue-110]") {
    const long sr = 48000, n = sr; // 1 s
    auto peak = [](const std::vector<float>& v) {
        float m = 0.0f; for (float x : v) m = std::max(m, std::abs(x)); return m;
    };

    // Percussive: 6 sharp exponentially-decaying clicks.
    std::vector<float> perc(static_cast<size_t>(n), 0.0f);
    for (int k = 0; k < 6; ++k) {
        const long t0 = static_cast<long>(k) * (n / 6);
        for (long j = 0; j < 2000 && t0 + j < n; ++j) {
            const float env = std::exp(-static_cast<float>(j) / 480.0f);
            perc[static_cast<size_t>(t0 + j)] =
                env * std::sin(2.0f * 3.14159265f * 150.0f * static_cast<float>(j) / static_cast<float>(sr));
        }
    }

    OfflineStretch s; s.prepare(static_cast<double>(sr), 1);
    const double R = 2.0;
    const long out_n = offline_stretch_output_frames(n, R);
    const float* ip[1] = {perc.data()};
    std::string err;

    OfflineStretchOptions base; base.time_ratio = R; base.quality = 2;
    std::vector<float> clean(static_cast<size_t>(out_n));
    float* cp[1] = {clean.data()};
    REQUIRE(s.process(ip, n, cp, out_n, base, &err));

    OfflineStretchOptions ro = base;
    ro.transient_mode = pulp::signal::StretchTransientMode::verbatim_relocate;
    std::vector<float> relo(static_cast<size_t>(out_n));
    float* rp[1] = {relo.data()};
    REQUIRE(s.process(ip, n, rp, out_n, ro, &err));

    // The plain PV smears the attacks; relocation restores them materially higher,
    // approaching the original peak.
    CHECK(peak(relo) > peak(clean) * 1.15f);
    CHECK(peak(relo) >= peak(perc) * 0.9f);

    // Tonal material: no onsets detected -> relocation is a perfect no-op.
    std::vector<float> sine(static_cast<size_t>(n));
    for (long i = 0; i < n; ++i)
        sine[static_cast<size_t>(i)] =
            0.6f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(i) / static_cast<float>(sr));
    const float* sip[1] = {sine.data()};
    std::vector<float> sclean(static_cast<size_t>(out_n)), srelo(static_cast<size_t>(out_n));
    float* scp[1] = {sclean.data()}; float* srp[1] = {srelo.data()};
    REQUIRE(s.process(sip, n, scp, out_n, base, &err));
    REQUIRE(s.process(sip, n, srp, out_n, ro, &err));
    float maxd = 0.0f;
    for (long i = 0; i < out_n; ++i) maxd = std::max(maxd, std::abs(sclean[static_cast<size_t>(i)] - srelo[static_cast<size_t>(i)]));
    CHECK(maxd < 1e-6f);

    // Identity (R=1) must ignore relocation entirely.
    std::vector<float> id(static_cast<size_t>(n));
    OfflineStretchOptions io = ro; io.time_ratio = 1.0;
    float* idp[1] = {id.data()};
    REQUIRE(s.process(ip, n, idp, n, io, &err));
    float idd = 0.0f; for (long i = 0; i < n; ++i) idd = std::max(idd, std::abs(id[static_cast<size_t>(i)] - perc[static_cast<size_t>(i)]));
    CHECK(idd < 1e-6f);
}
