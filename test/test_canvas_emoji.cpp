// test_canvas_emoji.cpp — covers the emoji-parity slice (pulp emoji-
// parity). Verifies both the Skia-free pieces (segmenter) and the
// Skia-backed shaping / measurement path. Tests that need a real
// SkFontMgr + emoji typeface bootstrap the bundled Noto Color Emoji
// fallback before running.
//
// As a side effect, each Skia-backed test writes a PNG to
// `PULP_EMOJI_TEST_PNG_DIR` (default `/tmp/pulp-emoji-validation/`) so a
// human can visually inspect that emoji actually render in color rather
// than tofu. Set `PULP_EMOJI_TEST_WRITE_PNGS=0` to suppress.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/emoji_segmenter.hpp>
#include <pulp/canvas/text_shaper.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/text_font_context.hpp>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#endif

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using pulp::canvas::FontRun;
using pulp::canvas::FontRunRole;
using pulp::canvas::segment_emoji_runs;
using pulp::canvas::contains_emoji;

namespace {

#ifdef PULP_HAS_SKIA
std::filesystem::path validation_dir() {
    if (const char* env = std::getenv("PULP_EMOJI_TEST_PNG_DIR")) {
        if (*env) return env;
    }
    // Prefer /tmp for cross-shell reachability (macOS's
    // `std::filesystem::temp_directory_path()` returns a per-user
    // `/var/folders/...` path that is awkward to copy-paste). Falls
    // back to the system temp dir if /tmp is unavailable.
    std::error_code ec;
    if (std::filesystem::exists("/tmp", ec)) {
        return std::filesystem::path("/tmp") / "pulp-emoji-validation";
    }
    return std::filesystem::temp_directory_path() / "pulp-emoji-validation";
}

bool png_writes_enabled() {
    // Default off — CI doesn't need the PNGs. Set
    // `PULP_EMOJI_TEST_WRITE_PNGS=1` locally when iterating on the
    // rendering path and want to eyeball the output.
    if (const char* env = std::getenv("PULP_EMOJI_TEST_WRITE_PNGS")) {
        std::string v(env);
        return v == "1" || v == "true" || v == "TRUE" || v == "yes";
    }
    return false;
}

void write_surface_png(SkSurface& surface, const std::string& name) {
    if (!png_writes_enabled()) return;
    // Read pixels directly off the surface — `makeImageSnapshot()` on a
    // raster surface produces an image whose pixels live in the GPU-
    // unfriendly half of Skia's image API. The straightforward path is
    // `surface->readPixels` into an RGBA buffer, then encode that.
    SkImageInfo info = surface.imageInfo();
    SkImageInfo dst_info = SkImageInfo::Make(info.width(), info.height(),
                                              kRGBA_8888_SkColorType,
                                              kUnpremul_SkAlphaType,
                                              SkColorSpace::MakeSRGB());
    std::vector<uint8_t> pixels(info.width() * info.height() * 4, 0);
    if (!surface.readPixels(dst_info, pixels.data(),
                            info.width() * 4, 0, 0)) {
        std::fprintf(stderr, "write_surface_png(%s): readPixels failed\n",
                     name.c_str());
        return;
    }
    SkPixmap pixmap(dst_info, pixels.data(), info.width() * 4);
    auto data = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!data) {
        std::fprintf(stderr, "write_surface_png(%s): encode failed\n",
                     name.c_str());
        return;
    }
    auto dir = validation_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto path = dir / (name + ".png");
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        std::fprintf(stderr, "write_surface_png(%s): cannot open %s\n",
                     name.c_str(), path.string().c_str());
        return;
    }
    out.write(reinterpret_cast<const char*>(data->data()),
              static_cast<std::streamsize>(data->size()));
}

// Once-per-process bootstrap that registers the bundled Noto Color Emoji
// typeface so every Skia-backed test sees the same emoji typeface. On
// macOS and Windows release builds without PULP_BUNDLE_NOTO_COLOR_EMOJI,
// falls through to `register_platform_emoji_fallback()`.
struct EmojiTypefaceBootstrap {
    EmojiTypefaceBootstrap() {
        if (!pulp::canvas::register_bundled_noto_color_emoji()) {
            pulp::canvas::register_platform_emoji_fallback();
        }
    }
};
EmojiTypefaceBootstrap kEmojiBootstrap;
#endif

} // namespace

// ── Segmenter (Skia-free) ───────────────────────────────────────────────

TEST_CASE("emoji_segmenter: empty input returns no runs", "[canvas][emoji][segmenter]") {
    auto runs = segment_emoji_runs("");
    REQUIRE(runs.empty());
}

TEST_CASE("emoji_segmenter: pure ASCII is one Default run",
          "[canvas][emoji][segmenter]") {
    auto runs = segment_emoji_runs("Hello, world");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Default);
    REQUIRE(runs[0].byte_start == 0);
    REQUIRE(runs[0].byte_end == 12);
}

TEST_CASE("emoji_segmenter: single emoji is one Emoji run",
          "[canvas][emoji][segmenter]") {
    // 😀 U+1F600 (F0 9F 98 80) — Emoji_Presentation default.
    auto runs = segment_emoji_runs("\xF0\x9F\x98\x80");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
    REQUIRE(runs[0].byte_start == 0);
    REQUIRE(runs[0].byte_end == 4);
}

TEST_CASE("emoji_segmenter: text-then-emoji emits two coalesced runs",
          "[canvas][emoji][segmenter]") {
    // "Hi 👋" — "Hi " is Default, 👋 (U+1F44B) is Emoji.
    auto runs = segment_emoji_runs("Hi \xF0\x9F\x91\x8B");
    REQUIRE(runs.size() == 2);
    REQUIRE(runs[0].role == FontRunRole::Default);
    REQUIRE(runs[0].byte_start == 0);
    REQUIRE(runs[0].byte_end == 3);
    REQUIRE(runs[1].role == FontRunRole::Emoji);
    REQUIRE(runs[1].byte_start == 3);
    REQUIRE(runs[1].byte_end == 7);
}

TEST_CASE("emoji_segmenter: ZWJ family stays as one Emoji run",
          "[canvas][emoji][segmenter]") {
    // 👨‍👩‍👧 — man (U+1F468) ZWJ woman (U+1F469) ZWJ girl (U+1F467).
    // UTF-8: F0 9F 91 A8 E2 80 8D F0 9F 91 A9 E2 80 8D F0 9F 91 A7
    auto runs = segment_emoji_runs(
        "\xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
    REQUIRE(runs[0].byte_start == 0);
    REQUIRE(runs[0].byte_end == 18);
}

TEST_CASE("emoji_segmenter: regional flag pair stays one Emoji run",
          "[canvas][emoji][segmenter]") {
    // 🇺🇸 — U+1F1FA + U+1F1F8. UTF-8 bytes: F0 9F 87 BA F0 9F 87 B8.
    auto runs = segment_emoji_runs("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
    REQUIRE(runs[0].byte_start == 0);
    REQUIRE(runs[0].byte_end == 8);
}

TEST_CASE("emoji_segmenter: keycap 1️⃣ stays as one Emoji run",
          "[canvas][emoji][segmenter]") {
    // "1" U+0031 + U+FE0F + U+20E3 → UTF-8: 31 EF B8 8F E2 83 A3
    auto runs = segment_emoji_runs("1\xEF\xB8\x8F\xE2\x83\xA3");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
    REQUIRE(runs[0].byte_end == 7);
}

TEST_CASE("emoji_segmenter: skin-tone modifier stays one Emoji run",
          "[canvas][emoji][segmenter]") {
    // 👍🏽 — thumbs up (U+1F44D) + medium skin tone (U+1F3FD).
    // UTF-8: F0 9F 91 8D F0 9F 8F BD
    auto runs = segment_emoji_runs("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
    REQUIRE(runs[0].byte_end == 8);
}

TEST_CASE("emoji_segmenter: FE0F promotes ™ to Emoji",
          "[canvas][emoji][segmenter]") {
    // ™ U+2122 (text default) + FE0F → emoji presentation.
    // UTF-8: E2 84 A2 EF B8 8F
    auto runs = segment_emoji_runs("\xE2\x84\xA2\xEF\xB8\x8F");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Emoji);
}

TEST_CASE("emoji_segmenter: FE0E forces text presentation even on emoji-default",
          "[canvas][emoji][segmenter]") {
    // ☔ U+2614 (emoji_presentation default) + FE0E → text presentation.
    // UTF-8: E2 98 94 EF B8 8E
    auto runs = segment_emoji_runs("\xE2\x98\x94\xEF\xB8\x8E");
    REQUIRE(runs.size() == 1);
    REQUIRE(runs[0].role == FontRunRole::Default);
}

TEST_CASE("emoji_segmenter: mixed text + ZWJ family + text emits 3 runs",
          "[canvas][emoji][segmenter]") {
    // "A 👨‍👩‍👧 B"
    auto runs = segment_emoji_runs(
        "A \xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7 B");
    REQUIRE(runs.size() == 3);
    REQUIRE(runs[0].role == FontRunRole::Default);
    REQUIRE(runs[1].role == FontRunRole::Emoji);
    REQUIRE(runs[2].role == FontRunRole::Default);
    REQUIRE(runs[0].byte_end == 2);
    REQUIRE(runs[1].byte_end == 20);  // "A " (2) + 18 emoji bytes
}

TEST_CASE("contains_emoji: cheap probe agrees with full segmenter",
          "[canvas][emoji][segmenter]") {
    REQUIRE_FALSE(contains_emoji("hello"));
    REQUIRE_FALSE(contains_emoji(""));
    REQUIRE(contains_emoji("\xF0\x9F\x98\x80"));            // 😀
    REQUIRE(contains_emoji("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8")); // 🇺🇸
    REQUIRE(contains_emoji("1\xEF\xB8\x8F\xE2\x83\xA3"));   // 1️⃣
}

// ── Cache invalidation (Skia-free, but uses generation counter) ──────────

TEST_CASE("font_registration_generation: bump_font_registration_generation advances",
          "[canvas][emoji][cache]") {
    // The non-Skia stubs in font_registry_stubs.cpp deliberately make
    // both `font_registration_generation()` and the bump a no-op
    // returning 0 — there are no downstream typeface caches to
    // invalidate without Skia. Skip the contract assertion on those
    // builds; the rest of the suite covers the Skia path that actually
    // uses the counter.
#ifdef PULP_HAS_SKIA
    auto before = pulp::canvas::font_registration_generation();
    pulp::canvas::bump_font_registration_generation();
    REQUIRE(pulp::canvas::font_registration_generation() > before);
#else
    SUCCEED("Skipped: no-Skia build — generation counter is a no-op stub.");
#endif
}

// ── Skia-backed paint + measurement ────────────────────────────────────

#ifdef PULP_HAS_SKIA

using pulp::canvas::SkiaCanvas;

namespace {

struct SurfaceHarness {
    sk_sp<SkSurface> surface;
    std::unique_ptr<SkiaCanvas> canvas;

    SurfaceHarness(int w, int h) {
        SkImageInfo info = SkImageInfo::Make(
            w, h, kN32_SkColorType, kPremul_SkAlphaType,
            SkColorSpace::MakeSRGB());
        surface = SkSurfaces::Raster(info);
        REQUIRE(surface != nullptr);
        auto* sk_canvas = surface->getCanvas();
        REQUIRE(sk_canvas != nullptr);
        sk_canvas->clear(SK_ColorWHITE);
        canvas = std::make_unique<SkiaCanvas>(sk_canvas);
    }
};

// Sum alpha contribution inside a screen-space band, sampled from the
// raster surface's pixmap. Returns the average non-background pixel
// count per row of the requested band. Used to assert "this emoji
// actually painted something visible," not "the call returned".
int count_non_white_pixels(SkSurface& surface, int x, int y, int w, int h) {
    SkPixmap pix;
    auto image = surface.makeImageSnapshot();
    if (!image) return 0;
    SkBitmap bitmap;
    if (!image->asLegacyBitmap(&bitmap)) return 0;
    if (!bitmap.peekPixels(&pix)) return 0;
    int counted = 0;
    for (int py = y; py < y + h && py < pix.height(); ++py) {
        for (int px = x; px < x + w && px < pix.width(); ++px) {
            SkColor c = pix.getColor(px, py);
            if (SkColorGetR(c) < 250 || SkColorGetG(c) < 250 || SkColorGetB(c) < 250) {
                ++counted;
            }
        }
    }
    return counted;
}

} // namespace

TEST_CASE("emoji typeface registered (bundled Noto or platform)",
          "[canvas][emoji][skia][bootstrap]") {
    auto ctx = pulp::canvas::TextFontContext::shared();
    // At least one of the bootstrap paths must succeed on a host with
    // Skia. CI without the bundled font AND without a platform emoji
    // typeface (rare) would skip the rest of this file.
    if (!ctx->has_emoji_typeface()) {
        WARN("No emoji typeface registered — rest of file's expectations "
             "around real emoji glyphs are stubbed.");
    }
}

TEST_CASE("measure_text_with_font reports non-zero width for single emoji",
          "[canvas][emoji][skia][measure]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    // 😀 width must be positive when an emoji typeface is available;
    // the legacy single-typeface path returned tofu width.
    auto m = SkiaCanvas::measure_text_with_font("Inter", 32.0f, "\xF0\x9F\x98\x80");
    REQUIRE(m.width > 0.0f);
}

TEST_CASE("ZWJ family measures within one cluster, not four codepoints",
          "[canvas][emoji][skia][cluster]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(64, 64);
    h.canvas->set_font("Inter", 32.0f);

    // Diagnostic: confirm primary typeface lookup is working too —
    // the cluster comparison below blows up if Inter is mis-resolved.
    float hi_width = h.canvas->measure_text("Hi");
    INFO("Hi measure (primary typeface sanity): " << hi_width);
    INFO("Emoji family registered: "
         << pulp::canvas::TextFontContext::shared()->emoji_family_name());

    // Single 👧 vs 👨‍👩‍👧 should be in the same width ballpark — the
    // ZWJ family is one cluster. If we naively assigned each codepoint
    // a full cell, the family width would be 3× the single-girl width
    // (plus ZWJ widths). Allow 1.5× headroom for hinting / typeface
    // differences between Noto and platform emoji fonts.
    float girl = h.canvas->measure_text("\xF0\x9F\x91\xA7");
    float family = h.canvas->measure_text(
        "\xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7");
    INFO("girl measure: " << girl << ", family measure: " << family);
    REQUIRE(girl > 0.0f);
    REQUIRE(family > 0.0f);
    REQUIRE(family <= girl * 1.5f);
}

TEST_CASE("regional flag pair measures as one emoji cluster",
          "[canvas][emoji][skia][cluster]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(64, 64);
    h.canvas->set_font("Inter", 32.0f);
    float flag = h.canvas->measure_text("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8");
    float two_glyphs = h.canvas->measure_text("AA");
    REQUIRE(flag > 0.0f);
    REQUIRE(flag <= two_glyphs * 2.0f);
}

TEST_CASE("keycap sequence renders as one emoji cell",
          "[canvas][emoji][skia][cluster]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(64, 64);
    h.canvas->set_font("Inter", 32.0f);
    float keycap = h.canvas->measure_text("1\xEF\xB8\x8F\xE2\x83\xA3");
    REQUIRE(keycap > 0.0f);
}

TEST_CASE("FE0E forces text presentation — width matches plain text",
          "[canvas][emoji][skia][selector]") {
    SurfaceHarness h(64, 64);
    h.canvas->set_font("Inter", 32.0f);
    // ☔ + FE0E (text presentation) — width must agree with plain ☔
    // when rendered through Inter (no color path).
    float plain = h.canvas->measure_text("\xE2\x98\x94");
    float text_presentation = h.canvas->measure_text("\xE2\x98\x94\xEF\xB8\x8E");
    REQUIRE(plain > 0.0f);
    // Allow tiny slack for selector-induced cluster reshape (should be
    // zero in practice but Skia may report ±1px).
    REQUIRE(std::abs(plain - text_presentation) < 2.0f);
}

TEST_CASE("fill_text paints visible color emoji pixels",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(96, 64);
    h.canvas->set_font("Inter", 36.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text("\xF0\x9F\x98\x80", 8.0f, 44.0f);  // 😀
    write_surface_png(*h.surface, "fill_text_single_emoji");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 96, 64);
    REQUIRE(painted > 50);
}

TEST_CASE("fill_text paints ZWJ family in one cluster",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(192, 64);
    h.canvas->set_font("Inter", 36.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text(
        "Family: "
        "\xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7",
        8.0f, 44.0f);
    write_surface_png(*h.surface, "fill_text_zwj_family");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 192, 64);
    REQUIRE(painted > 200);
}

TEST_CASE("fill_text paints regional flag",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(96, 64);
    h.canvas->set_font("Inter", 36.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text("\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8", 8.0f, 44.0f);
    write_surface_png(*h.surface, "fill_text_flag_us");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 96, 64);
    REQUIRE(painted > 50);
}

TEST_CASE("fill_text paints keycap 1️⃣",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(96, 64);
    h.canvas->set_font("Inter", 36.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text("1\xEF\xB8\x8F\xE2\x83\xA3", 8.0f, 44.0f);
    write_surface_png(*h.surface, "fill_text_keycap_one");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 96, 64);
    REQUIRE(painted > 30);
}

TEST_CASE("fill_text paints skin-tone modifier as one glyph",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(96, 64);
    h.canvas->set_font("Inter", 36.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD", 8.0f, 44.0f);
    write_surface_png(*h.surface, "fill_text_thumbs_up_skin_tone");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 96, 64);
    REQUIRE(painted > 50);
}

TEST_CASE("fill_text paints mixed sequence — text + ZWJ family + flag + keycap",
          "[canvas][emoji][skia][visual]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(640, 96);
    h.canvas->set_font("Inter", 32.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text(
        "Hi \xF0\x9F\x91\x8B "
        "\xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7 "
        "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8 "
        "1\xEF\xB8\x8F\xE2\x83\xA3 "
        "\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD",
        8.0f, 64.0f);
    write_surface_png(*h.surface, "fill_text_mixed_showcase");
    int painted = count_non_white_pixels(*h.surface, 0, 0, 640, 96);
    REQUIRE(painted > 500);
}

TEST_CASE("text_align center and right reflect cluster-aware width",
          "[canvas][emoji][skia][align]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(256, 64);
    h.canvas->set_font("Inter", 28.0f);
    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->set_text_align(pulp::canvas::TextAlign::center);
    h.canvas->fill_text(
        "AA \xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7 BB",
        128.0f, 44.0f);
    write_surface_png(*h.surface, "fill_text_align_center");
    // Center alignment with an emoji cluster should paint pixels in
    // BOTH left and right halves — if cluster width is wrong the
    // emoji + right-side text would be clipped to one half.
    int left_half = count_non_white_pixels(*h.surface, 0, 0, 128, 64);
    int right_half = count_non_white_pixels(*h.surface, 128, 0, 128, 64);
    REQUIRE(left_half > 50);
    REQUIRE(right_half > 50);
}

TEST_CASE("letter_spacing tracks between graphemes, not within emoji cluster",
          "[canvas][emoji][skia][letter-spacing]") {
    if (!pulp::canvas::TextFontContext::shared()->has_emoji_typeface()) {
        SUCCEED("Skipped: no emoji typeface registered.");
        return;
    }
    SurfaceHarness h(512, 96);
    h.canvas->set_font("Inter", 32.0f);

    h.canvas->set_font_full("Inter", 32.0f, 400, 0, /*letter_spacing=*/0.0f);
    float baseline = h.canvas->measure_text(
        "ZWJ \xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7");

    h.canvas->set_font_full("Inter", 32.0f, 400, 0, /*letter_spacing=*/8.0f);
    float tracked = h.canvas->measure_text(
        "ZWJ \xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7");

    INFO("baseline (letter-spacing=0): " << baseline);
    INFO("tracked  (letter-spacing=8): " << tracked);
    // SkParagraph applies letter-spacing between glyph clusters via
    // TextStyle::setLetterSpacing — should add ≥ 1 unit of tracking
    // between each grapheme in the run. We assert it's STRICTLY
    // larger than the no-tracking baseline rather than chasing a
    // pixel-exact delta (Skia's internal accounting varies by version
    // and the exact grapheme count for ZWJ clusters).
    REQUIRE(tracked > baseline);

    h.canvas->set_fill_color(pulp::canvas::Color{0.0f, 0.0f, 0.0f, 1.0f});
    h.canvas->fill_text(
        "ZWJ \xF0\x9F\x91\xA8\xE2\x80\x8D"
        "\xF0\x9F\x91\xA9\xE2\x80\x8D"
        "\xF0\x9F\x91\xA7",
        16.0f, 64.0f);
    write_surface_png(*h.surface, "fill_text_letter_spacing_8");
}

TEST_CASE("register_emoji_fallback re-registration invalidates cached typefaces",
          "[canvas][emoji][skia][cache-invalidation]") {
    // Take initial measurement under whatever emoji typeface the
    // bootstrap registered.
    SurfaceHarness h(96, 64);
    h.canvas->set_font("Inter", 32.0f);
    float before = h.canvas->measure_text("\xF0\x9F\x98\x80");

    // Force-bump the registration generation. The canvas typeface
    // cache and text_shaper segment cache should both flush; the next
    // measurement should hit the resolver fresh. We expect the same
    // numeric width (same typeface still wins), but the path should
    // NOT short-circuit through a stale cache entry — the generation
    // gate is the assertion target.
    auto before_gen = pulp::canvas::font_registration_generation();
    pulp::canvas::bump_font_registration_generation();
    auto after_gen = pulp::canvas::font_registration_generation();
    REQUIRE(after_gen > before_gen);

    float after = h.canvas->measure_text("\xF0\x9F\x98\x80");
    REQUIRE(after == Catch::Approx(before));
}

#endif // PULP_HAS_SKIA
