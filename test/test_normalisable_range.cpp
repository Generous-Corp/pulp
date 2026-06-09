#include <catch2/catch_test_macros.hpp>
#include <pulp/state/parameter.hpp>

#include <cmath>

using namespace pulp::state;

TEST_CASE("NormalisableRange linear behaves like ParamRange", "[state][normalisable]") {
    NormalisableRange<float> r{0.0f, 10.0f};
    REQUIRE(r.normalize(0.0f) == 0.0f);
    REQUIRE(r.normalize(5.0f) == 0.5f);
    REQUIRE(r.normalize(10.0f) == 1.0f);
    REQUIRE(r.denormalize(0.5f) == 5.0f);
}

TEST_CASE("NormalisableRange clamps out-of-range inputs", "[state][normalisable]") {
    NormalisableRange<float> r{-1.0f, 1.0f};
    REQUIRE(r.normalize(-5.0f) == 0.0f);
    REQUIRE(r.normalize(5.0f) == 1.0f);
    REQUIRE(r.denormalize(-1.0f) == -1.0f);
    REQUIRE(r.denormalize(2.0f) == 1.0f);
}

TEST_CASE("NormalisableRange skew weights low end", "[state][normalisable]") {
    // skew < 1 should pull the curve toward `min` — so denormalize(0.5)
    // returns a value below the linear midpoint.
    NormalisableRange<float> r{0.0f, 100.0f, /*step*/ 0.0f, /*skew*/ 0.3f};
    const float mid = r.denormalize(0.5f);
    REQUIRE(mid < 50.0f);
    REQUIRE(mid > 0.0f);
    // Round-trip: normalize(denormalize(n)) == n within tolerance.
    REQUIRE(std::abs(r.normalize(mid) - 0.5f) < 1e-5f);
}

TEST_CASE("NormalisableRange with_centre places midpoint at 0.5", "[state][normalisable]") {
    // Classic frequency knob: 20 Hz min, 20000 Hz max, 1000 Hz centre.
    auto r = NormalisableRange<float>::with_centre(20.0f, 20000.0f, 1000.0f);
    const float at_half = r.denormalize(0.5f);
    REQUIRE(std::abs(at_half - 1000.0f) < 0.5f);
    // Endpoints still snap.
    REQUIRE(r.denormalize(0.0f) == 20.0f);
    REQUIRE(r.denormalize(1.0f) == 20000.0f);
}

TEST_CASE("NormalisableRange step quantization", "[state][normalisable]") {
    NormalisableRange<float> r{0.0f, 10.0f, /*step*/ 0.5f};
    REQUIRE(r.denormalize(0.5f) == 5.0f);
    REQUIRE(r.denormalize(0.05f) == 0.5f);
    // 0.07 of 10 = 0.7, snaps to 0.5.
    REQUIRE(r.denormalize(0.07f) == 0.5f);
    REQUIRE(r.snap(2.3f) == 2.5f);
    REQUIRE(r.snap(0.2f) == 0.0f);
}

TEST_CASE("NormalisableRange symmetric skew is mirrored", "[state][normalisable]") {
    NormalisableRange<float> r{-1.0f, 1.0f, /*step*/ 0.0f, /*skew*/ 0.5f, /*symmetric*/ true};
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
