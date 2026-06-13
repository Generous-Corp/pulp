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

#include <cmath>
#include <vector>

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
