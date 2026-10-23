// ParagraphCache — the process-wide laid-out paragraph cache behind
// SkiaCanvas text.
//
// Every cached render is compared byte for byte with the same draw made with
// the cache disabled. The variant cases draw one run, then a run that differs
// in exactly one key input, so a key that dropped that input would hand back
// the first paragraph and paint the wrong glyphs or layout.

#ifdef PULP_HAS_SKIA

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/paragraph_cache.hpp>
#include <pulp/canvas/resource_counters.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace pulp::canvas;

namespace {

constexpr int kWidth = 240;
constexpr int kHeight = 48;
constexpr float kBaseline = 32.0f;

using Draw = std::function<void(SkiaCanvas&)>;

struct Pixels {
    std::vector<std::uint8_t> bytes;
    bool operator==(const Pixels&) const = default;
};

// One frame: a fresh SkiaCanvas over a fresh white raster, the way a host
// builds a canvas per frame.
Pixels render(const Draw& draw) {
    SkBitmap bm;
    bm.allocPixels(SkImageInfo::Make(kWidth, kHeight, kRGBA_8888_SkColorType, kPremul_SkAlphaType,
                                     SkColorSpace::MakeSRGB()));
    bm.eraseColor(SK_ColorWHITE);
    SkCanvas sk(bm);
    SkiaCanvas canvas(&sk);
    canvas.set_fill_color(Color::rgba8(0, 0, 0));
    draw(canvas);
    Pixels out;
    const auto* base = static_cast<const std::uint8_t*>(bm.getPixels());
    out.bytes.assign(base, base + bm.computeByteSize());
    return out;
}

bool has_ink(const Pixels& p) {
    for (std::size_t i = 0; i < p.bytes.size(); i += 4)
        if (p.bytes[i] < 250)
            return true;
    return false;
}

std::uint64_t paragraph_builds() {
    return resource_counters().paragraph_builds;
}

// Restores the shared cache to its defaults around every case.
struct CacheScope {
    CacheScope() {
        reset();
    }
    ~CacheScope() {
        reset();
    }
    static void reset() {
        auto& cache = ParagraphCache::instance();
        cache.set_enabled(true);
        cache.set_limits(ParagraphCache::kDefaultMaxEntries, ParagraphCache::kDefaultMaxBytes);
        cache.clear();
        cache.reset_stats();
    }
};

Pixels render_uncached(const Draw& draw) {
    auto& cache = ParagraphCache::instance();
    cache.set_enabled(false);
    auto out = render(draw);
    cache.set_enabled(true);
    return out;
}

// Paints `first` into a warm cache, then `second`, and requires `second` to
// match its uncached render. The two uncached renders must differ, or the case
// could not see a key that confused them.
void expect_second_matches_uncached(const Draw& first, const Draw& second) {
    const Pixels want_first = render_uncached(first);
    const Pixels want_second = render_uncached(second);
    REQUIRE(has_ink(want_second));
    REQUIRE_FALSE(want_first == want_second);

    ParagraphCache::instance().clear();
    CHECK(render(first) == want_first);
    CHECK(render(second) == want_second);
    // And again, now that both are cached.
    CHECK(render(first) == want_first);
    CHECK(render(second) == want_second);
}

Draw text_with(std::string family, float size, int weight, int slant, float spacing,
               std::string text, std::vector<Canvas::FontFeature> features = {}) {
    return [=](SkiaCanvas& c) {
        c.set_font_full(family, size, weight, slant, spacing);
        if (!features.empty())
            c.set_font_features(features);
        c.fill_text(text, 8.0f, kBaseline);
    };
}

} // namespace

TEST_CASE("Paragraph cache paints the uncached pixels and builds once",
          "[canvas][paragraph-cache]") {
    CacheScope scope;
    const Draw draw = text_with("Inter", 16.0f, 400, 0, 0.0f, "Output level");
    const Pixels want = render_uncached(draw);
    REQUIRE(has_ink(want));

    ParagraphCache::instance().clear();
    const auto before = paragraph_builds();
    for (int frame = 0; frame < 5; ++frame)
        CHECK(render(draw) == want);
    CHECK(paragraph_builds() - before == 1);
    CHECK(ParagraphCache::instance().stats().hits == 4);
}

TEST_CASE("Paragraph cache disabled rebuilds on every paint", "[canvas][paragraph-cache]") {
    CacheScope scope;
    const Draw draw = text_with("Inter", 16.0f, 400, 0, 0.0f, "Output level");
    ParagraphCache::instance().set_enabled(false);
    const auto before = paragraph_builds();
    for (int frame = 0; frame < 5; ++frame)
        render(draw);
    CHECK(paragraph_builds() - before == 5);
    CHECK(ParagraphCache::instance().stats().entries == 0);
}

TEST_CASE("Paragraph cache keys every shaping input", "[canvas][paragraph-cache]") {
    CacheScope scope;
    const auto base = [](std::string text = "Hello 1001") {
        return text_with("Inter", 16.0f, 400, 0, 0.0f, std::move(text));
    };
    SECTION("text") {
        expect_second_matches_uncached(base("Level 1"), base("Level 2"));
    }
    SECTION("size") {
        expect_second_matches_uncached(base(),
                                       text_with("Inter", 18.0f, 400, 0, 0.0f, "Hello 1001"));
    }
    SECTION("family") {
        expect_second_matches_uncached(
            base(), text_with("JetBrains Mono", 16.0f, 400, 0, 0.0f, "Hello 1001"));
    }
    SECTION("weight") {
        expect_second_matches_uncached(base(),
                                       text_with("Inter", 16.0f, 700, 0, 0.0f, "Hello 1001"));
    }
    SECTION("slant") {
        expect_second_matches_uncached(base(),
                                       text_with("Inter", 16.0f, 400, 1, 0.0f, "Hello 1001"));
    }
    SECTION("letter spacing") {
        expect_second_matches_uncached(base(),
                                       text_with("Inter", 16.0f, 400, 0, 2.5f, "Hello 1001"));
    }
    SECTION("OpenType features") {
        const Canvas::FontFeature zero{Canvas::make_font_feature_tag("zero"), 1};
        expect_second_matches_uncached(text_with("Inter", 16.0f, 400, 0, 0.0f, "1001", {}),
                                       text_with("Inter", 16.0f, 400, 0, 0.0f, "1001", {zero}));
    }
}

TEST_CASE("Paragraph cache applies each draw's own paint", "[canvas][paragraph-cache]") {
    CacheScope scope;
    const auto spaced = [](SkiaCanvas& c) { c.set_font_full("Inter", 18.0f, 400, 0, 1.0f); };
    SECTION("colour") {
        expect_second_matches_uncached(
            [&](SkiaCanvas& c) {
                spaced(c);
                c.set_fill_color(Color::rgba8(220, 30, 30));
                c.fill_text("Mix 42%", 8.0f, kBaseline);
            },
            [&](SkiaCanvas& c) {
                spaced(c);
                c.set_fill_color(Color::rgba8(30, 60, 220));
                c.fill_text("Mix 42%", 8.0f, kBaseline);
            });
    }
    SECTION("gradient") {
        expect_second_matches_uncached(
            [&](SkiaCanvas& c) {
                spaced(c);
                c.fill_text("Mix 42%", 8.0f, kBaseline);
            },
            [&](SkiaCanvas& c) {
                spaced(c);
                const Color colors[] = {Color::rgba8(255, 0, 0), Color::rgba8(0, 0, 255)};
                const float stops[] = {0.0f, 1.0f};
                c.set_fill_gradient_linear(8.0f, 0.0f, 100.0f, 0.0f, colors, stops, 2);
                c.fill_text("Mix 42%", 8.0f, kBaseline);
            });
    }
    SECTION("stroke after fill") {
        expect_second_matches_uncached(
            [&](SkiaCanvas& c) {
                spaced(c);
                c.fill_text("Mix 42%", 8.0f, kBaseline);
            },
            [&](SkiaCanvas& c) {
                spaced(c);
                c.set_stroke_color(Color::rgba8(0, 120, 0));
                c.set_line_width(1.0f);
                c.stroke_text("Mix 42%", 8.0f, kBaseline, 0.0f);
            });
    }
}

TEST_CASE("Paragraph cache follows a width and alignment change", "[canvas][paragraph-cache]") {
    CacheScope scope;
    const auto font = [](SkiaCanvas& c) { c.set_font_full("Inter", 16.0f, 400, 0, 0.5f); };
    SECTION("max width") {
        expect_second_matches_uncached(
            [&](SkiaCanvas& c) {
                font(c);
                c.fill_text_with_max_width("Frequency response", 8.0f, kBaseline, 220.0f);
            },
            [&](SkiaCanvas& c) {
                font(c);
                c.fill_text_with_max_width("Frequency response", 8.0f, kBaseline, 90.0f);
            });
    }
    SECTION("alignment") {
        expect_second_matches_uncached(
            [&](SkiaCanvas& c) {
                font(c);
                c.fill_text("Frequency", 120.0f, kBaseline);
            },
            [&](SkiaCanvas& c) {
                font(c);
                c.set_text_align(TextAlign::center);
                c.fill_text("Frequency", 120.0f, kBaseline);
            });
    }
}

TEST_CASE("Paragraph cache drops its entries when a font registers",
          "[canvas][paragraph-cache][fonts]") {
    CacheScope scope;
    // JetBrains Mono has no U+0132; Jost does. Registering Jost under the
    // stack's second family changes the fallback for that glyph while the
    // primary face, and so every other key input, stays the same. Letter
    // spacing keeps the run on SkParagraph: unspaced text the primary face
    // cannot cover takes the per-run fallback shaper instead.
    const std::string family = "JetBrains Mono, PulpParagraphCacheJost";
    const Draw draw = text_with(family, 20.0f, 400, 0, 0.5f,
                                "A\xC4\xB2"
                                "B");

    ParagraphCache::instance().clear();
    const Pixels before = render(draw);
    REQUIRE(render(draw) == before); // now served from the cache
    REQUIRE(ParagraphCache::instance().stats().entries == 1);

    if (!register_font_file(PULP_TEST_JOST_FONT_PATH, "PulpParagraphCacheJost"))
        SKIP("platform font manager unavailable");

    const Pixels after = render(draw);
    CHECK(ParagraphCache::instance().stats().invalidations >= 1);
    CHECK(after == render_uncached(draw));
    CHECK_FALSE(after == before);
}

TEST_CASE("Paragraph cache stays within its entry and byte bounds", "[canvas][paragraph-cache]") {
    CacheScope scope;
    auto& cache = ParagraphCache::instance();
    const auto draw_n = [](int n) {
        for (int i = 0; i < n; ++i)
            render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset " + std::to_string(i)));
    };

    cache.set_limits(3, ParagraphCache::kDefaultMaxBytes);
    draw_n(5);
    CHECK(cache.stats().entries == 3);
    CHECK(cache.stats().evictions == 2);

    cache.clear();
    cache.reset_stats();
    const std::size_t two = 2 * ParagraphCache::estimated_entry_bytes(std::strlen("Preset 0"));
    cache.set_limits(ParagraphCache::kDefaultMaxEntries, two);
    draw_n(6);
    CHECK(cache.stats().entries == 2);
    CHECK(cache.stats().bytes <= two);

    // The least recently used entry goes first: re-touching "Preset 4" keeps
    // it when "Preset 6" arrives.
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 4"));
    const auto hits = cache.stats().hits;
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 6"));
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 4"));
    CHECK(cache.stats().hits == hits + 1);

    // Shrinking evicts at once.
    cache.set_limits(1, two);
    CHECK(cache.stats().entries == 1);
}

#if defined(__APPLE__)
TEST_CASE("Paragraph cache byte estimate bounds what an entry holds",
          "[canvas][paragraph-cache][memory]") {
    CacheScope scope;
    auto& cache = ParagraphCache::instance();
    constexpr int kCount = 96;
    std::vector<std::string> texts;
    std::size_t text_bytes = 0;
    for (int i = 0; i < kCount; ++i) {
        texts.push_back("Band " + std::to_string(i) + " gain -" + std::to_string(i % 24) + " dB");
        text_bytes += texts.back().size();
    }
    const auto draw_all = [&] {
        for (const auto& t : texts)
            render(text_with("Inter", 13.0f, 400, 0, 0.0f, t));
    };
    // Warm Skia's glyph and shaping caches with nothing retained, so the
    // measured growth is what the entries themselves hold.
    cache.set_enabled(false);
    draw_all();
    cache.set_enabled(true);
    cache.clear();

    malloc_statistics_t before{};
    malloc_zone_statistics(nullptr, &before);
    draw_all();
    malloc_statistics_t after{};
    malloc_zone_statistics(nullptr, &after);

    REQUIRE(cache.stats().entries == kCount);
    const std::size_t held =
        after.size_in_use > before.size_in_use ? after.size_in_use - before.size_in_use : 0;
    std::size_t estimate = 0;
    for (const auto& t : texts)
        estimate += ParagraphCache::estimated_entry_bytes(t.size());
    INFO("held " << held << " bytes for " << kCount << " entries (" << text_bytes
                 << " text bytes); estimate " << estimate);
    CHECK(held > 0);
    CHECK(held <= estimate);
    CHECK(cache.stats().bytes == estimate);
}
#endif

#endif // PULP_HAS_SKIA
