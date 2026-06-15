// OfflineStretch — Phase 0 scaffold tests.
//
// At this phase the engine is a length-correct pass-through: exact identity at
// time_ratio == 1, and an honest placeholder (copy + zero-pad) otherwise. These
// tests pin the parts of the contract that must hold for EVERY future phase:
//   - exact output length = round(in_frames * time_ratio) (loop grid-lock);
//   - R=1, pitch 0 is a perfect null against the input;
//   - the process() contract rejects misuse (unprepared, wrong out length).
// The stretch-quality assertions arrive with the real engine in Phase 1+.

#include <catch2/catch_test_macros.hpp>

#include <pulp/signal/offline_stretch.hpp>

#include <algorithm>
#include <cmath>
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
    // Placeholder body must have written every output sample (no stale fill).
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
        // tone. Confirms repitch reads the correct positions; a Kaiser-sinc
        // (Resampler, 96 dB) upgrade for repitch is a P6 quality item.
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

TEST_CASE("Phase 1 safety + determinism (tempo path)", "[offline-stretch]") {
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
