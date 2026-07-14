#include <catch2/catch_test_macros.hpp>
#include <pulp/state/parameter.hpp>

#include <cmath>
#include <vector>

using namespace pulp::state;

TEST_CASE("SkewedRange linear behaves like ParamRange", "[state][skewed-range]") {
    SkewedRange<float> r{0.0f, 10.0f};
    REQUIRE(r.normalize(0.0f) == 0.0f);
    REQUIRE(r.normalize(5.0f) == 0.5f);
    REQUIRE(r.normalize(10.0f) == 1.0f);
    REQUIRE(r.denormalize(0.5f) == 5.0f);
}

TEST_CASE("SkewedRange clamps out-of-range inputs", "[state][skewed-range]") {
    SkewedRange<float> r{-1.0f, 1.0f};
    REQUIRE(r.normalize(-5.0f) == 0.0f);
    REQUIRE(r.normalize(5.0f) == 1.0f);
    REQUIRE(r.denormalize(-1.0f) == -1.0f);
    REQUIRE(r.denormalize(2.0f) == 1.0f);
}

TEST_CASE("SkewedRange skew weights low end", "[state][skewed-range]") {
    // skew < 1 should pull the curve toward `min` — so denormalize(0.5)
    // returns a value below the linear midpoint.
    SkewedRange<float> r{0.0f, 100.0f, /*step*/ 0.0f, /*skew*/ 0.3f};
    const float mid = r.denormalize(0.5f);
    REQUIRE(mid < 50.0f);
    REQUIRE(mid > 0.0f);
    // Round-trip: normalize(denormalize(n)) == n within tolerance.
    REQUIRE(std::abs(r.normalize(mid) - 0.5f) < 1e-5f);
}

TEST_CASE("SkewedRange with_centre places midpoint at 0.5", "[state][skewed-range]") {
    // Classic frequency knob: 20 Hz min, 20000 Hz max, 1000 Hz centre.
    auto r = SkewedRange<float>::with_centre(20.0f, 20000.0f, 1000.0f);
    const float at_half = r.denormalize(0.5f);
    REQUIRE(std::abs(at_half - 1000.0f) < 0.5f);
    // Endpoints still snap.
    REQUIRE(r.denormalize(0.0f) == 20.0f);
    REQUIRE(r.denormalize(1.0f) == 20000.0f);
}

TEST_CASE("SkewedRange step quantization", "[state][skewed-range]") {
    SkewedRange<float> r{0.0f, 10.0f, /*step*/ 0.5f};
    REQUIRE(r.denormalize(0.5f) == 5.0f);
    REQUIRE(r.denormalize(0.05f) == 0.5f);
    // 0.07 of 10 = 0.7, snaps to 0.5.
    REQUIRE(r.denormalize(0.07f) == 0.5f);
    REQUIRE(r.snap(2.3f) == 2.5f);
    REQUIRE(r.snap(0.2f) == 0.0f);
}

TEST_CASE("SkewedRange symmetric skew is mirrored", "[state][skewed-range]") {
    SkewedRange<float> r{-1.0f, 1.0f, /*step*/ 0.0f, /*skew*/ 0.5f, /*symmetric*/ true};
    // 0.0 in normalized space maps to -1, 1.0 maps to 1.
    REQUIRE(std::abs(r.denormalize(0.0f) - (-1.0f)) < 1e-5f);
    REQUIRE(std::abs(r.denormalize(1.0f) - 1.0f) < 1e-5f);
    // 0.5 in normalized space maps near the midpoint (0).
    REQUIRE(std::abs(r.denormalize(0.5f)) < 1e-5f);
    // Mirror property: denormalize(0.5 + d) == -denormalize(0.5 - d) within tolerance.
    const float plus = r.denormalize(0.7f);
    const float minus = r.denormalize(0.3f);
    REQUIRE(std::abs(plus + minus) < 1e-5f);
}

// ---------------------------------------------------------------------------
// Shaped ParamRange — the curve now lives on ParamRange itself so every format
// adapter (which converts through ParamRange::normalize/denormalize) and the
// UI normalized seam pick it up. These tests pin the two load-bearing
// invariants: linear ranges stay bit-identical to the legacy affine map, and
// skewed ranges round-trip monotonically with exact endpoints.
// ---------------------------------------------------------------------------

namespace {

// The legacy linear math, replicated verbatim, so we can assert the shaped
// ParamRange takes a *bit-identical* fast path when skew == 1.
float legacy_normalize(float min, float max, float value) {
    if (max == min) return 0.0f;
    return std::clamp((value - min) / (max - min), 0.0f, 1.0f);
}
float legacy_denormalize(float min, float max, float step, float normalized) {
    float value = min + normalized * (max - min);
    if (step > 0.0f) {
        value = min + std::round((value - min) / step) * step;
    }
    return std::clamp(value, min, max);
}

} // namespace

TEST_CASE("ParamRange linear is bit-identical to the legacy affine map",
          "[state][range][shaped]") {
    // Default-constructed range and an explicit linear range must reproduce the
    // exact bits of the pre-skew implementation across the full domain.
    const ParamRange a{-60.0f, 12.0f, -6.0f, 0.0f};
    const ParamRange b = ParamRange::linear(0.0f, 100.0f, 50.0f, 0.5f);
    REQUIRE(a.is_linear());
    REQUIRE(b.is_linear());

    for (int i = 0; i <= 1000; ++i) {
        const float n = static_cast<float>(i) / 1000.0f;
        const float v = a.min + n * (a.max - a.min);
        // Bit-exact equality (==), not tolerance — guards the fast path.
        REQUIRE(a.normalize(v) == legacy_normalize(a.min, a.max, v));
        REQUIRE(a.denormalize(n) == legacy_denormalize(a.min, a.max, a.step, n));

        const float vb = b.min + n * (b.max - b.min);
        REQUIRE(b.normalize(vb) == legacy_normalize(b.min, b.max, vb));
        REQUIRE(b.denormalize(n) ==
                legacy_denormalize(b.min, b.max, b.step, n));
    }
}

TEST_CASE("ParamRange skew round-trips monotonically with exact endpoints",
          "[state][range][shaped]") {
    // Frequency-style knob: low end gets more resolution (skew < 1).
    auto range = ParamRange::with_centre(20.0f, 20000.0f, 1000.0f);
    REQUIRE_FALSE(range.is_linear());

    // Endpoints are exact regardless of the curve.
    REQUIRE(range.denormalize(0.0f) == 20.0f);
    REQUIRE(range.denormalize(1.0f) == 20000.0f);
    REQUIRE(range.normalize(20.0f) == 0.0f);
    REQUIRE(range.normalize(20000.0f) == 1.0f);

    // Known midpoint: 0.5 normalized lands at the chosen 1 kHz centre.
    REQUIRE(std::abs(range.denormalize(0.5f) - 1000.0f) < 0.5f);

    // Monotonic across the whole normalized domain, with a coarse round-trip
    // bound. A 1000:1 skewed float curve loses precision near the steep top end
    // (pow(pow(n, 1/skew), skew) in float32), so 5e-3 is the honest ceiling
    // there — still 0.5% of the normalized range.
    float prev = range.denormalize(0.0f);
    for (int i = 1; i <= 1000; ++i) {
        const float n = static_cast<float>(i) / 1000.0f;
        const float plain = range.denormalize(n);
        REQUIRE(plain >= prev);                          // monotone non-decreasing
        REQUIRE(std::abs(range.normalize(plain) - n) < 5e-3f);
        prev = plain;
    }

    // A moderate skew round-trips tightly in float32: assert sub-1e-4 there so a
    // future regression in the curve math (not just the float ULP wall on the
    // extreme curve) is caught.
    ParamRange gentle{0.0f, 100.0f, 0.0f, 0.0f, 0.5f, false};
    REQUIRE_FALSE(gentle.is_linear());
    for (int i = 0; i <= 1000; ++i) {
        const float n = static_cast<float>(i) / 1000.0f;
        REQUIRE(std::abs(gentle.normalize(gentle.denormalize(n)) - n) < 1e-4f);
    }
}

TEST_CASE("ParamRange symmetric skew is bipolar and mirrored",
          "[state][range][shaped]") {
    // Pan-style bipolar control: [-1, 1] with a symmetric curve.
    ParamRange range{-1.0f, 1.0f, 0.0f, 0.0f, 0.5f, /*symmetric*/ true};
    REQUIRE_FALSE(range.is_linear());

    REQUIRE(std::abs(range.denormalize(0.0f) - (-1.0f)) < 1e-5f);
    REQUIRE(std::abs(range.denormalize(1.0f) - 1.0f) < 1e-5f);
    REQUIRE(std::abs(range.denormalize(0.5f)) < 1e-5f); // centre at midpoint

    // Mirror symmetry about the centre.
    const float plus = range.denormalize(0.7f);
    const float minus = range.denormalize(0.3f);
    REQUIRE(std::abs(plus + minus) < 1e-5f);

    // Round-trip holds for the symmetric branch too.
    for (int i = 0; i <= 100; ++i) {
        const float n = static_cast<float>(i) / 100.0f;
        REQUIRE(std::abs(range.normalize(range.denormalize(n)) - n) < 1e-4f);
    }
}

TEST_CASE("ParamRange with_centre falls back to linear for degenerate centre",
          "[state][range][shaped]") {
    // Centre outside the open interval ⇒ no skew (stays linear, exact).
    auto on_edge = ParamRange::with_centre(0.0f, 10.0f, 0.0f);
    REQUIRE(on_edge.is_linear());
    REQUIRE(on_edge.denormalize(0.5f) == 5.0f);

    auto inverted = ParamRange::with_centre(10.0f, 0.0f, 5.0f);
    REQUIRE(inverted.is_linear());
}

TEST_CASE("ParamRange skew honors step quantization on denormalize",
          "[state][range][shaped]") {
    // Skewed range with a coarse step: denormalize must still snap to the grid.
    ParamRange range{0.0f, 100.0f, 0.0f, 10.0f, 0.4f, false};
    REQUIRE_FALSE(range.is_linear());
    for (int i = 0; i <= 100; ++i) {
        const float v = range.denormalize(static_cast<float>(i) / 100.0f);
        const float snapped = std::round(v / 10.0f) * 10.0f;
        REQUIRE(std::abs(v - snapped) < 1e-3f);
    }
}

// ---------------------------------------------------------------------------
// Skew-curve reference cross-check — "prove the skew".
//
// SkewedRange's curve is the standard power-law parameter mapping used
// throughout plugin automation:
//
//     normalized  = p^skew                    (p = (v - min) / (max - min))
//     denormalized = min + (max - min) * p^(1/skew)
//
// and, for `symmetric_skew`, the same power law applied to the signed distance
// from the midpoint, d = 2p - 1:
//
//     normalized  = (1 + sign(d) * |d|^skew) / 2
//
// This block asserts SkewedRange against a SECOND, independently written
// evaluation of those same formulas, so a typo in either implementation shows
// up as a mismatch. The reference deliberately takes a different numerical
// route through the inverse — exp(log(p) / skew) instead of pow(p, 1/skew) —
// so agreement is evidence about the *math*, not a copy of the same
// expression tree. Everything runs in double, which pushes the only residual
// difference down to exp/log-vs-pow rounding (a handful of ULP) rather than
// the float32 precision wall the shaped tests above already document.
//
// Why this matters beyond arithmetic hygiene: this power law is the curve an
// automation lane authored in any other plugin ecosystem was recorded against.
// Pinning it at 1e-9 across skew in [0.05, 20] is what lets a project migrated
// into Pulp keep the *same knob feel* instead of silently re-shaping every
// swept parameter.
// ---------------------------------------------------------------------------

namespace skew_ref {

// Second evaluation of the power-law mapping written straight from the
// formulas above. `skew == 1` short-circuits to the affine map (the power law
// degenerates to identity there, and skipping pow() keeps it bit-exact).
double convert_to_0to1(double start, double end, double skew, bool symmetric,
                       double v) {
    double proportion = (v - start) / (end - start);
    proportion = std::clamp(proportion, 0.0, 1.0);
    if (skew == 1.0) return proportion;
    if (!symmetric) return std::pow(proportion, skew);
    const double dfm = 2.0 * proportion - 1.0;
    return (1.0 + std::pow(std::abs(dfm), skew) * (dfm < 0.0 ? -1.0 : 1.0)) / 2.0;
}

double convert_from_0to1(double start, double end, double skew, bool symmetric,
                         double proportion) {
    proportion = std::clamp(proportion, 0.0, 1.0);
    if (!symmetric) {
        if (skew != 1.0 && proportion > 0.0)
            proportion = std::exp(std::log(proportion) / skew);
        return start + (end - start) * proportion;
    }
    double dfm = 2.0 * proportion - 1.0;
    if (skew != 1.0 && dfm != 0.0)
        dfm = std::exp(std::log(std::abs(dfm)) / skew) * (dfm < 0.0 ? -1.0 : 1.0);
    return start + (end - start) / 2.0 * (1.0 + dfm);
}

}  // namespace skew_ref

TEST_CASE("SkewedRange matches the power-law reference across skew [0.05,20]",
          "[state][skewed-range][skew-ref]") {
    struct Case { double lo, hi; bool symmetric; };
    const std::vector<Case> ranges = {
        {0.0, 100.0, false},       // gain-style
        {20.0, 20000.0, false},    // frequency-style
        {-1.0, 1.0, true},         // bipolar pan/detune (symmetric)
        {-24.0, 24.0, true},       // bipolar transpose (symmetric)
    };
    const std::vector<double> skews = {0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0};

    for (const auto& rc : ranges) {
        for (double skew : skews) {
            SkewedRange<double> r{rc.lo, rc.hi, /*step*/ 0.0, skew, rc.symmetric};
            const double span = rc.hi - rc.lo;
            for (int i = 0; i <= 100; ++i) {
                const double n = static_cast<double>(i) / 100.0;

                // denormalize == reference normalized -> real mapping.
                const double plain = r.denormalize(n);
                const double ref_plain =
                    skew_ref::convert_from_0to1(rc.lo, rc.hi, skew, rc.symmetric, n);
                REQUIRE(std::abs(plain - ref_plain) <= 1e-9 * span + 1e-12);

                // normalize == reference real -> normalized mapping
                // (sample across the real domain).
                const double v = rc.lo + n * span;
                const double norm = r.normalize(v);
                const double ref_norm =
                    skew_ref::convert_to_0to1(rc.lo, rc.hi, skew, rc.symmetric, v);
                REQUIRE(std::abs(norm - ref_norm) <= 1e-9 + 1e-12);
            }
        }
    }
}

TEST_CASE("SkewedRange plain->norm->plain round-trips tightly in double",
          "[state][skewed-range][skew-ref]") {
    // Double precision removes the float32 wall, so the inverse round-trip is
    // tight across the WELL-CONDITIONED domain. Non-symmetric skew is stable to
    // the extreme (0.05..20); the symmetric branch composes pow(·,1/skew) then
    // pow(·,skew) about the midpoint and becomes ill-conditioned there for very
    // large skew (skew=20 maps a wide band to near the center, so the inverse
    // loses ~1e-4 absolute). That pathological corner — not a real bipolar
    // parameter — is still proven FORWARD-exact against the reference at 1e-9
    // by the match test above; here we bound symmetric skew to a realistic
    // range so the round-trip assertion stays tight and meaningful.
    const double span = 32.0;
    const double tol = 1e-7 * span + 1e-12;  // ~7 orders tighter than float32
    const auto round_trip_ok = [&](double skew, bool symmetric) {
        SkewedRange<double> r{-8.0, 24.0, /*step*/ 0.0, skew, symmetric};
        for (int i = 0; i <= 200; ++i) {
            const double v = -8.0 + (span * i) / 200.0;
            const double rt = r.denormalize(r.normalize(v));
            REQUIRE(std::abs(rt - v) <= tol);
        }
    };
    for (double skew : {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 10.0, 20.0})
        round_trip_ok(skew, /*symmetric*/ false);
    for (double skew : {0.2, 0.5, 1.0, 2.0, 5.0})
        round_trip_ok(skew, /*symmetric*/ true);
}

TEST_CASE("SkewedRange step snapping lands every value on the interval grid",
          "[state][skewed-range][skew-ref]") {
    // A stepped range must only ever emit values that sit on the interval
    // grid, no matter how the curve bends the way out of 0..1. denormalize()
    // snaps on the way out; snap() is the same rounding exposed standalone.
    // Prove both on a skewed, stepped range.
    SkewedRange<double> r{0.0, 120.0, /*interval*/ 12.0, /*skew*/ 0.3, false};
    for (int i = 0; i <= 100; ++i) {
        const double v = r.denormalize(static_cast<double>(i) / 100.0);
        // Every emitted value sits exactly on a 12-unit grid line.
        const double grid = std::round(v / 12.0) * 12.0;
        REQUIRE(std::abs(v - grid) < 1e-9);
        REQUIRE(v >= 0.0);
        REQUIRE(v <= 120.0);
    }
    // snap() is the standalone rounding primitive.
    REQUIRE(r.snap(17.0) == 12.0);
    REQUIRE(r.snap(19.0) == 24.0);
    REQUIRE(r.snap(-5.0) == 0.0);
    REQUIRE(r.snap(999.0) == 120.0);
}
