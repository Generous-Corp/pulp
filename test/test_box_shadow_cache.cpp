// test_box_shadow_cache.cpp — outset box-shadow coverage cache: visual parity
// with the direct path, plus the dual-flag behavior (re-blit on move, re-tint
// on color change, re-blur only on geometry change).

#include <catch2/catch_test_macros.hpp>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/box_shadow_cache.hpp>
#include <pulp/canvas/canvas.hpp>

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

using pulp::canvas::BoxShadowCache;
using pulp::canvas::Color;
using pulp::canvas::render_box_shadow_to_rgba;

namespace {

double mean_abs_diff(const std::vector<std::uint8_t>& a,
                     const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1e9;
    long long acc = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        acc += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    return static_cast<double>(acc) / static_cast<double>(a.size());
}

int max_abs_diff(const std::vector<std::uint8_t>& a,
                 const std::vector<std::uint8_t>& b) {
    int m = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
        m = std::max(m, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    return m;
}

}  // namespace

TEST_CASE("box shadow cache matches the direct path within tolerance",
          "[canvas][shadow][cache]") {
    auto& cache = BoxShadowCache::instance();
    const Color col = Color::rgba(0.0f, 0.0f, 0.0f, 0.5f);

    cache.clear();
    cache.set_enabled(false);
    const auto reference = render_box_shadow_to_rgba(
        220, 220, 1.0f, 60, 60, 90, 70, 0, 6, 18, 2, col, 14, false);

    cache.clear();
    cache.set_enabled(true);
    const auto cached = render_box_shadow_to_rgba(
        220, 220, 1.0f, 60, 60, 90, 70, 0, 6, 18, 2, col, 14, false);

    REQUIRE(reference.size() == cached.size());
    const double mean = mean_abs_diff(reference, cached);
    const int worst = max_abs_diff(reference, cached);
    CAPTURE(mean, worst);
    REQUIRE(mean < 2.5);
    // Also bound the worst single-pixel deviation: a mean-only check hides edge
    // resampling error from the cached path's linear blit. At scale 1.0 the
    // cached coverage is blitted 1:1 (observed worst is ~1/255); the margin
    // tolerates cross-platform AA rounding while still catching a gross
    // divergence (a squished oversized render or a wrong-key collision would
    // push this into the hundreds).
    REQUIRE(worst < 16);
}

TEST_CASE("moving shadow re-blits without re-blurring", "[canvas][shadow][cache]") {
    auto& cache = BoxShadowCache::instance();
    cache.clear();
    cache.set_enabled(true);
    cache.reset_stats();

    const Color col = Color::rgba(0.0f, 0.0f, 0.0f, 0.6f);
    for (int i = 0; i < 3; ++i) {
        render_box_shadow_to_rgba(256, 256, 1.0f, 10.0f + i * 20.0f,
                                  10.0f + i * 20.0f, 80, 60, 0, 4, 20, 0, col, 10,
                                  false);
    }
    const auto s = cache.stats();
    CAPTURE(s.renders, s.hits);
    REQUIRE(s.renders == 1);  // blurred once
    REQUIRE(s.hits == 2);     // re-blitted twice
}

TEST_CASE("color/opacity change re-tints without re-blurring",
          "[canvas][shadow][cache]") {
    auto& cache = BoxShadowCache::instance();
    cache.clear();
    cache.set_enabled(true);
    cache.reset_stats();

    const auto red = render_box_shadow_to_rgba(
        256, 256, 1.0f, 40, 40, 80, 60, 0, 4, 20, 0,
        Color::rgba(1.0f, 0.0f, 0.0f, 0.7f), 10, false);
    const auto blue = render_box_shadow_to_rgba(
        256, 256, 1.0f, 40, 40, 80, 60, 0, 4, 20, 0,
        Color::rgba(0.0f, 0.0f, 1.0f, 0.4f), 10, false);

    const auto s = cache.stats();
    CAPTURE(s.renders, s.hits);
    REQUIRE(s.renders == 1);  // blur reused
    REQUIRE(s.hits == 1);
    // ...but the tint actually changed the pixels.
    REQUIRE(max_abs_diff(red, blue) > 10);
}

TEST_CASE("geometry change triggers a re-blur", "[canvas][shadow][cache]") {
    auto& cache = BoxShadowCache::instance();
    cache.clear();
    cache.set_enabled(true);
    cache.reset_stats();

    const Color col = Color::rgba(0.0f, 0.0f, 0.0f, 0.5f);
    render_box_shadow_to_rgba(256, 256, 1.0f, 40, 40, 80, 60, 0, 4, 20, 0, col, 10,
                              false);
    render_box_shadow_to_rgba(256, 256, 1.0f, 40, 40, 80, 60, 0, 4, 30, 0, col, 10,
                              false);  // different blur radius
    const auto s = cache.stats();
    CAPTURE(s.renders, s.hits);
    REQUIRE(s.renders == 2);
}

TEST_CASE("box shadow cache skips an oversized shadow (falls through to direct path)",
          "[canvas][shadow][cache]") {
    // Regression: a shadow whose device extent exceeds Skia's 16384px max must NOT
    // be cached. The old code clamped the coverage surface to the max and stretched
    // it across the destination (squished shadow); the fix returns nullptr from the
    // render lambda so the caller falls through to the direct (uncached) path.
    // Assert nothing gets cached (the lambda returned null → no entry stored).
    auto& cache = BoxShadowCache::instance();
    cache.clear();
    cache.set_enabled(true);
    cache.reset_stats();

    const Color col = Color::rgba(0.0f, 0.0f, 0.0f, 0.5f);
    // scale 240 * ~100px local extent ≈ 24000 device px > 16384 → oversized.
    render_box_shadow_to_rgba(256, 256, 240.0f, 40, 40, 90, 70, 0, 6, 18, 2, col, 14,
                              false);
    REQUIRE(cache.stats().size == 0);  // oversized → not cached, no squished entry
}

TEST_CASE("BoxShadowKey::q handles non-finite and out-of-range inputs",
          "[canvas][shadow][cache]") {
    using pulp::canvas::BoxShadowKey;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Non-finite → a fixed bucket (0), never UB in the int cast.
    REQUIRE(BoxShadowKey::q(nan) == 0);
    REQUIRE(BoxShadowKey::q(inf) == 0);
    REQUIRE(BoxShadowKey::q(-inf) == 0);

    // Huge finite values are clamped to ±kLimit before the *4 and cast (no int32
    // overflow / UB) — they map to the same bucket as the clamp boundary.
    REQUIRE(BoxShadowKey::q(1.0e30f) == BoxShadowKey::q(4.0e8f));
    REQUIRE(BoxShadowKey::q(-1.0e30f) == BoxShadowKey::q(-4.0e8f));

    // Normal quantization is unaffected (1/4-unit buckets).
    REQUIRE(BoxShadowKey::q(0.0f) == 0);
    REQUIRE(BoxShadowKey::q(1.0f) == 4);
    REQUIRE(BoxShadowKey::q(-1.0f) == -4);
}

#endif  // PULP_HAS_SKIA
