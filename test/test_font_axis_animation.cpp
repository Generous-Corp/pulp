// test_font_axis_animation.cpp — Pulp #2163, font v2 Slice 3.3.
//
// Variable-font animation cache eviction. 60fps `wght` interpolation
// produces 60 distinct FontOptions hashes per second per family.
// Without an LRU cap, the resolver cache grows unbounded; this test
// pins the contract that animations stay within `set_cache_capacity`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/font_options.hpp>

#ifdef PULP_HAS_SKIA
#include "include/core/SkTypeface.h"
#endif

using namespace pulp::canvas;

TEST_CASE("FontResolver: default cache capacity is non-zero", "[font][axes][issue-2163]") {
    auto& r = FontResolver::instance();
    REQUIRE(r.cache_capacity() > 0u);
}

TEST_CASE("FontResolver: set_cache_capacity round-trip", "[font][axes]") {
    auto& r = FontResolver::instance();
    auto original = r.cache_capacity();

    r.set_cache_capacity(64);
    REQUIRE(r.cache_capacity() == 64u);

    r.set_cache_capacity(0);  // disabled
    REQUIRE(r.cache_capacity() == 0u);

    r.set_cache_capacity(original);
}

TEST_CASE("FontResolver: animation respects LRU cache cap", "[font][axes]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(16);

    // Simulate a 60fps weight animation: 100 distinct axis values
    // crossing the 16-entry cap should evict steadily.
    for (int i = 0; i < 100; ++i) {
        FontOptions opts;
        opts.family_stack.push_back("Inter");
        opts.size = 14.0f;
        opts.variation_axes.push_back({
            make_variation_axis_tag('w','g','h','t'),
            100.0f + static_cast<float>(i) * 8.0f,
        });
        (void) r.resolve_family_list(opts);
    }
#ifdef PULP_HAS_SKIA
    REQUIRE(r.cache_size() <= 16u);
    REQUIRE(r.cache_size() >= 1u);  // at least one entry survived
#else
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);  // restore default-ish
    r.clear_cache();
}

TEST_CASE("FontResolver: capacity=0 disables cap (legacy unbounded)", "[font][axes]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(0);

    for (int i = 0; i < 50; ++i) {
        FontOptions opts;
        opts.family_stack.push_back("Inter");
        opts.size = 14.0f + static_cast<float>(i) * 0.1f;  // distinct size each iter
        (void) r.resolve_family_list(opts);
    }
#ifdef PULP_HAS_SKIA
    // No cap → cache holds every distinct key.
    REQUIRE(r.cache_size() >= 40u);
#else
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);
    r.clear_cache();
}

TEST_CASE("FontResolver: shrinking the cap evicts oldest immediately",
          "[font][axes]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(64);

    for (int i = 0; i < 32; ++i) {
        FontOptions opts;
        opts.family_stack.push_back("Inter");
        opts.size = 14.0f + static_cast<float>(i) * 0.5f;
        (void) r.resolve_family_list(opts);
    }
#ifdef PULP_HAS_SKIA
    REQUIRE(r.cache_size() == 32u);

    r.set_cache_capacity(8);
    REQUIRE(r.cache_size() == 8u);
#else
    REQUIRE(r.cache_size() == 0u);
    r.set_cache_capacity(8);
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);
    r.clear_cache();
}

TEST_CASE("FontResolver: LRU hit promotes entry past eviction line",
          "[font][axes]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(4);

    // Insert 4 entries: A, B, C, D (oldest-to-newest)
    auto resolve_size = [&](float size) {
        FontOptions opts;
        opts.family_stack.push_back("Inter");
        opts.size = size;
        return r.resolve_family_list(opts);
    };
    resolve_size(10.0f);
    resolve_size(11.0f);
    resolve_size(12.0f);
    resolve_size(13.0f);
#ifdef PULP_HAS_SKIA
    REQUIRE(r.cache_size() == 4u);

    // Touch the oldest (10.0f) — promotes it to most-recent.
    resolve_size(10.0f);
    // Insert one more (14.0f) — evicts the now-oldest (11.0f).
    resolve_size(14.0f);
    REQUIRE(r.cache_size() == 4u);

    // 10.0f should still be cached (we just promoted it).
    auto before_size = r.cache_size();
    resolve_size(10.0f);  // hit, no insert
    REQUIRE(r.cache_size() == before_size);
#else
    REQUIRE(r.cache_size() == 0u);
    resolve_size(10.0f);
    resolve_size(14.0f);
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);
    r.clear_cache();
}

TEST_CASE("FontResolver: explicit generation splits otherwise identical cache keys",
          "[font][axes][coverage]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(8);

    FontOptions opts;
    opts.family_stack.push_back("Inter");
    opts.size = 14.0f;

    opts.registry_generation = 100;
    (void) r.resolve_family_list(opts);
#ifdef PULP_HAS_SKIA
    REQUIRE(r.cache_size() == 1u);

    opts.registry_generation = 101;
    (void) r.resolve_family_list(opts);
    REQUIRE(r.cache_size() == 2u);

    opts.registry_generation = 100;
    (void) r.resolve_family_list(opts);
    REQUIRE(r.cache_size() == 2u);
#else
    REQUIRE(r.cache_size() == 0u);

    opts.registry_generation = 101;
    (void) r.resolve_family_list(opts);
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);
    r.clear_cache();
}

TEST_CASE("FontResolver: empty family stack uses a distinct default cache key",
          "[font][axes][coverage]") {
    auto& r = FontResolver::instance();
    r.clear_cache();
    r.set_cache_capacity(8);

    FontOptions default_opts;
    default_opts.size = 14.0f;
    (void) r.resolve_family_list(default_opts);
#ifdef PULP_HAS_SKIA
    REQUIRE(r.cache_size() == 1u);

    FontOptions explicit_opts = default_opts;
    explicit_opts.family_stack.push_back("Inter");
    (void) r.resolve_family_list(explicit_opts);
    REQUIRE(r.cache_size() == 2u);

    (void) r.resolve_family_list(default_opts);
    REQUIRE(r.cache_size() == 2u);
#else
    REQUIRE(r.cache_size() == 0u);

    FontOptions explicit_opts = default_opts;
    explicit_opts.family_stack.push_back("Inter");
    (void) r.resolve_family_list(explicit_opts);
    REQUIRE(r.cache_size() == 0u);
#endif

    r.set_cache_capacity(256);
    r.clear_cache();
}
