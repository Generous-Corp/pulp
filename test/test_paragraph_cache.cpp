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

#include <pulp/canvas/text_font_context.hpp>

#include "skia_paragraph_cache.hpp"
#include "skia_unicode.hpp"

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
#include <thread>
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

// Key fields SkiaCanvas holds constant today, so no draw can vary them; the
// key must still keep them apart, because the build reads them.
TEST_CASE("Paragraph cache keys the paragraph style switches the build reads",
          "[canvas][paragraph-cache]") {
    CacheScope scope;
    ParagraphKey base;
    base.text = "Bold heading";
    base.families = {"Inter"};
    base.size = 16.0f;
    base.weight = 700;
    base.font_generation = font_registration_generation();
    int builds = 0;
    const auto build = [&builds](const ParagraphKey&) {
        ++builds;
        // Any laid-out paragraph makes the entry storable.
        PreparedParagraph prepared;
        auto collection = TextFontContext::shared()->font_collection();
        auto builder = skia::textlayout::ParagraphBuilder::make(skia::textlayout::ParagraphStyle(),
                                                                collection, shared_sk_unicode());
        builder->addText("x");
        prepared.paragraph = builder->Build();
        prepared.paragraph->layout(100.0f);
        return prepared;
    };
    const auto lookup = [&](const ParagraphKey& key) {
        return static_cast<bool>(find_or_build_paragraph(key, build));
    };
    SECTION("fake missing font styles") {
        ParagraphKey other = base;
        other.fake_missing_font_styles = !base.fake_missing_font_styles;
        REQUIRE(lookup(base));
        REQUIRE(lookup(base));
        REQUIRE(builds == 1);
        REQUIRE(lookup(other));
        CHECK(builds == 2);
    }
    SECTION("hinting") {
        ParagraphKey other = base;
        other.hinting = !base.hinting;
        REQUIRE(lookup(base));
        REQUIRE(lookup(other));
        CHECK(builds == 2);
    }
}

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
    SECTION("direction") {
        const auto with_direction = [](Canvas::TextDirection direction) {
            return [direction](SkiaCanvas& c) {
                c.set_font_full("Inter", 16.0f, 400, 0, 0.0f);
                c.set_direction(direction);
                c.fill_text("Hello 1001!", 8.0f, kBaseline);
            };
        };
        expect_second_matches_uncached(with_direction(Canvas::TextDirection::ltr),
                                       with_direction(Canvas::TextDirection::rtl));
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

    // An entry looked up again is protected and probation is evicted first:
    // re-touching "Preset 4" keeps it when "Preset 6" arrives.
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 4"));
    const auto hits = cache.stats().hits;
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 6"));
    render(text_with("Inter", 14.0f, 400, 0, 0.0f, "Preset 4"));
    CHECK(cache.stats().hits == hits + 1);

    // Shrinking evicts at once.
    cache.set_limits(1, two);
    CHECK(cache.stats().entries == 1);
}

TEST_CASE("Paragraph cache canonicalises non-finite and signed-zero spacing",
          "[canvas][paragraph-cache]") {
    CacheScope scope;
    auto& cache = ParagraphCache::instance();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // NaN and infinite spacing draw unspaced, cached or not, and a repaint
    // finds the entry rather than adding one per paint.
    const Draw spaced_nan = text_with("Inter", 16.0f, 400, 0, nan, "Gain -3 dB");
    const Draw sized_nan = text_with("Inter", nan, 400, 0, 0.0f, "Gain -3 dB");
    const Pixels want = render_uncached(text_with("Inter", 16.0f, 400, 0, 0.0f, "Gain -3 dB"));
    const Pixels want_sized = render_uncached(sized_nan);
    CHECK(render_uncached(spaced_nan) == want);
    cache.clear();
    const auto before = paragraph_builds();
    for (int frame = 0; frame < 4; ++frame)
        CHECK(render(spaced_nan) == want);
    CHECK(render(text_with("Inter", 16.0f, 400, 0, inf, "Gain -3 dB")) == want);
    CHECK(render(sized_nan) == want_sized);
    CHECK(render(sized_nan) == want_sized);
    CHECK(paragraph_builds() - before == 2); // the NaN-size run is its own key

    // -0 and +0 are one key.
    cache.clear();
    const auto zeros = paragraph_builds();
    render(text_with("Inter", 16.0f, 400, 0, -0.0f, "Mix"));
    render(text_with("Inter", 16.0f, 400, 0, 0.0f, "Mix"));
    CHECK(paragraph_builds() - zeros == 1);
    CHECK(cache.stats().entries == 1);

    // A NaN run reaching the eviction end of the cache does not crash it.
    cache.clear();
    cache.set_limits(1, ParagraphCache::kDefaultMaxBytes);
    render(spaced_nan);
    for (int i = 0; i < 3; ++i)
        render(text_with("Inter", 16.0f, 400, 0, nan, "Run " + std::to_string(i)));
    CHECK(cache.stats().entries == 1);
}

TEST_CASE("Paragraph cache keeps a static label through text that changes every frame",
          "[canvas][paragraph-cache]") {
    CacheScope scope;
    auto& cache = ParagraphCache::instance();
    cache.set_limits(64, ParagraphCache::kDefaultMaxBytes);
    const Draw label = text_with("Inter", 14.0f, 400, 0, 0.0f, "Output");
    const Pixels want = render_uncached(label);
    render(label);
    render(label); // looked up again: protected

    // 1000 frames of a meter readout, each a string never seen before, while
    // the label is not repainted.
    for (int frame = 0; frame < 1000; ++frame)
        render(text_with("Inter", 14.0f, 400, 0, 0.0f,
                         std::to_string(-60 + frame % 61) + "." + std::to_string(frame) + " dB"));
    CHECK(cache.stats().entries <= 64);

    const auto before = paragraph_builds();
    CHECK(render(label) == want);
    CHECK(paragraph_builds() - before == 0);
}

TEST_CASE("Paragraph cache stays correct and bounded under concurrent painting",
          "[canvas][paragraph-cache][threading]") {
    CacheScope scope;
    auto& cache = ParagraphCache::instance();
    constexpr int kThreads = 4;
    constexpr int kFrames = 60;
    std::vector<std::string> texts;
    for (int i = 0; i < 12; ++i)
        texts.push_back("Shared " + std::to_string(i));
    for (int t = 0; t < kThreads; ++t)
        for (int i = 0; i < 6; ++i)
            texts.push_back("Thread " + std::to_string(t) + " run " + std::to_string(i));
    std::vector<Pixels> want;
    for (const auto& text : texts)
        want.push_back(render_uncached(text_with("Inter", 15.0f, 400, 0, 0.5f, text)));

    const std::size_t small = 8 * ParagraphCache::estimated_entry_bytes(16);
    std::atomic<bool> painting{true};
    std::atomic<int> mismatches{0};
    std::vector<std::thread> painters;
    for (int t = 0; t < kThreads; ++t) {
        painters.emplace_back([&, t] {
            for (int frame = 0; frame < kFrames; ++frame) {
                const std::size_t shared = static_cast<std::size_t>((frame + t) % 12);
                const std::size_t own = 12 + static_cast<std::size_t>(t * 6 + frame % 6);
                for (const std::size_t i : {shared, own})
                    if (!(render(text_with("Inter", 15.0f, 400, 0, 0.5f, texts[i])) == want[i]))
                        ++mismatches;
            }
        });
    }
    // Meanwhile font registrations invalidate everything and the bounds move.
    std::thread churn([&] {
        int round = 0;
        while (painting.load()) {
            bump_font_registration_generation();
            cache.set_limits(round % 2 ? 4 : 64,
                             round % 2 ? small : ParagraphCache::kDefaultMaxBytes);
            ++round;
            std::this_thread::yield();
        }
    });
    for (auto& p : painters)
        p.join();
    painting = false;
    churn.join();

    CHECK(mismatches.load() == 0);
    const auto stats = cache.stats();
    CHECK(stats.entries <= stats.max_entries);
    CHECK(stats.bytes <= stats.max_bytes);
    // Byte accounting did not drift: every live entry holds 8 to 17 text bytes.
    CHECK(stats.bytes >= stats.entries * ParagraphCache::estimated_entry_bytes(8));
    CHECK(stats.bytes <= stats.entries * ParagraphCache::estimated_entry_bytes(17));
}

#if defined(__APPLE__)
namespace {

// Heap growth from caching @p texts, after an uncached pass has warmed Skia's
// glyph and shaping caches so the growth is what the entries themselves hold.
std::size_t held_by_entries(const std::vector<std::string>& texts, float spacing) {
    auto& cache = ParagraphCache::instance();
    const auto draw_all = [&] {
        for (const auto& t : texts)
            render(text_with("Inter", 13.0f, 400, 0, spacing, t));
    };
    cache.set_enabled(false);
    draw_all();
    cache.set_enabled(true);
    cache.clear();
    malloc_statistics_t before{};
    malloc_zone_statistics(nullptr, &before);
    draw_all();
    malloc_statistics_t after{};
    malloc_zone_statistics(nullptr, &after);
    REQUIRE(cache.stats().entries == texts.size());
    return after.size_in_use > before.size_in_use ? after.size_in_use - before.size_in_use : 0;
}

std::vector<std::string> numbered(const std::string& stem, int count) {
    std::vector<std::string> texts;
    for (int i = 0; i < count; ++i)
        texts.push_back(stem + " " + std::to_string(i));
    return texts;
}

} // namespace

TEST_CASE("Paragraph cache byte estimate tracks what an entry holds",
          "[canvas][paragraph-cache][memory]") {
    CacheScope scope;
    // Text the primary face cannot cover takes the per-run fallback shaper
    // unless it is spaced, so the CJK group is spaced to reach SkParagraph.
    struct Group {
        const char* name;
        std::vector<std::string> texts;
        float spacing = 0.0f;
    };
    const std::vector<Group> groups = {
        {"short latin", numbered("Band gain -12 dB", 96)},
        {"long latin",
         numbered("The quick brown fox jumps over the lazy dog while the low shelf rises "
                  "and the high cut falls; attack, release, ratio, knee and makeup gain "
                  "all sit on one line here",
                  48)},
        {"cjk",
         numbered("\xE9\x9F\xB3\xE9\x87\x8F\xE8\xB0\x83\xE6\x95\xB4"
                  "\xE9\xA2\x91\xE7\x8E\x87\xE5\x93\x8D\xE5\xBA\x94",
                  64),
         0.5f},
        {"emoji", numbered("Mix \xF0\x9F\x8E\x9B\xEF\xB8\x8F level \xF0\x9F\x94\x8A", 64)},
    };
    for (const auto& group : groups) {
        const std::size_t held = held_by_entries(group.texts, group.spacing);
        std::size_t estimate = 0;
        std::size_t text_bytes = 0;
        for (const auto& t : group.texts) {
            estimate += ParagraphCache::estimated_entry_bytes(t.size());
            text_bytes += t.size();
        }
        INFO(group.name << ": held " << held << " bytes for " << group.texts.size() << " entries ("
                        << text_bytes << " text bytes); estimate " << estimate);
        CHECK(ParagraphCache::instance().stats().bytes == estimate);
        // An upper bound with headroom, not a multiple of the real footprint:
        // a far-off estimate would make the byte bound meaningless.
        CHECK(held <= estimate);
        CHECK(held * 3 >= estimate);
    }
}
#endif

#endif // PULP_HAS_SKIA
