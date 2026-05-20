// test_canvas_fonts.cpp — extracted from test_canvas.cpp in the 2026-05
// Phase 5 (P5-2 first cut) refactor. Covers two font-related test
// clusters that share Skia font-manager fixtures:
//
//   - pulp #932 — bundled-font (Inter-Regular, JetBrainsMono-Regular)
//     registration with SkFontMgr + bundled_blobs() table.
//   - pulp #1150 — public `register_font(path)` API + the SkFontMgr
//     fallback chain through match_bundled_typeface.
//
// Test set is byte-equivalent to the in-place section it replaces;
// only the TU boundary moved.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/sdf_atlas.hpp>
#include <array>
#include <functional>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;

#ifdef PULP_HAS_SKIA

// ── pulp #932 — bundled-font registration with SkFontMgr ────────────────────

// pulp_add_binary_data wires Inter-Regular.ttf and JetBrainsMono-Regular.ttf
// into pulp-canvas at build time. This test asserts the C++ side of #932:
// that match_bundled_typeface() returns a non-null SkTypeface for both
// bundled families even when the host system doesn't ship them. Without
// #932, "JetBrains Mono" fell through to SkFontMgr::matchFamilyStyle which
// returns null on a stock macOS install — and the next non-ASCII fill_text
// call would throw std::out_of_range during glyph fallback.
#include <pulp/canvas/bundled_fonts.hpp>
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#endif

namespace {

// Mirror skia_canvas.cpp's get_font_manager() but for tests — returns
// whatever platform manager the prebuilt Skia ships with on this OS, or
// nullptr (Linux without fontconfig wired into the test binary, etc.).
sk_sp<SkFontMgr> test_platform_font_mgr() {
#if defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
    return SkFontMgr_New_DirectWrite();
#else
    return nullptr; // Linux/Android: pulp-test-canvas doesn't link FreeType.
#endif
}

} // namespace

TEST_CASE("Bundled font count matches the embedded asset list (#932)",
          "[canvas][skia][fonts][issue-932]") {
    // Two faces ship today: Inter-Regular and JetBrainsMono-Regular. If a
    // future PR grows the bundle, bump this expectation deliberately so
    // we catch accidental drops.
    REQUIRE(pulp::canvas::bundled_font_count() == 2);
}

TEST_CASE("Bundled fonts resolve via SkFontMgr::makeFromData (#932)",
          "[canvas][skia][fonts][issue-932]") {
    auto mgr = test_platform_font_mgr();
    if (!mgr) {
        SUCCEED("Skipping bundled-font lookup — no platform font manager "
                "linked into pulp-test-canvas on this platform.");
        return;
    }

    SkFontStyle upright_normal{SkFontStyle::kNormal_Weight,
                               SkFontStyle::kNormal_Width,
                               SkFontStyle::kUpright_Slant};

    // Inter is the "sans" half of the bundle. Even on a Mac without Inter
    // installed system-wide, this must succeed because the .ttf is baked
    // into pulp-canvas.
    auto inter = pulp::canvas::match_bundled_typeface(mgr.get(), "Inter",
                                                     upright_normal);
    REQUIRE(inter != nullptr);
    SkString inter_family;
    inter->getFamilyName(&inter_family);
    REQUIRE(std::string(inter_family.c_str()) == "Inter");

    // JetBrains Mono is the "mono" half — this is the family that
    // motivated #932 (the std::out_of_range em-dash crash).
    auto jb = pulp::canvas::match_bundled_typeface(mgr.get(),
                                                   "JetBrains Mono",
                                                   upright_normal);
    REQUIRE(jb != nullptr);
    SkString jb_family;
    jb->getFamilyName(&jb_family);
    REQUIRE(std::string(jb_family.c_str()) == "JetBrains Mono");

    // A name we DON'T bundle must miss — match_bundled_typeface only
    // covers the bundle, not the system-wide font catalogue.
    auto miss = pulp::canvas::match_bundled_typeface(mgr.get(),
                                                     "ThisFamilyDoesNotExist",
                                                     upright_normal);
    REQUIRE(miss == nullptr);

    // Style-aware miss (Codex P2 on PR #956): the bundle currently ships
    // only Regular/Upright Inter, but the system font catalogue does have
    // a real Inter Bold and Italic. If a caller asks for Inter at
    // weight=Bold, returning the bundled Regular face would mask the
    // system Bold and silently regress #927's weight/slant honouring.
    // match_bundled_typeface MUST return nullptr in that case so the
    // skia_canvas lookup keeps walking to matchFamilyStyle().
    SkFontStyle bold_normal{SkFontStyle::kBold_Weight,
                            SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant};
    auto bold_miss = pulp::canvas::match_bundled_typeface(mgr.get(), "Inter",
                                                           bold_normal);
    REQUIRE(bold_miss == nullptr);

    SkFontStyle italic{SkFontStyle::kNormal_Weight,
                       SkFontStyle::kNormal_Width,
                       SkFontStyle::kItalic_Slant};
    auto italic_miss = pulp::canvas::match_bundled_typeface(mgr.get(), "Inter",
                                                             italic);
    REQUIRE(italic_miss == nullptr);
}

TEST_CASE("match_bundled_typeface is null-safe when no font mgr is available "
          "(#932)",
          "[canvas][skia][fonts][issue-932]") {
    // Linux-without-fontconfig and any future platform that returns a null
    // SkFontMgr from get_font_manager() must fail gracefully — the bundle
    // can't be materialised without a manager, so callers should fall back
    // to the legacy code path rather than crash. Catch2 runs cases in
    // random order so the registration cache may already be populated by
    // the prior test; query a deliberately-unknown family so the
    // null-safety we're checking isn't masked by a happy lookup hit.
    auto miss = pulp::canvas::match_bundled_typeface(
        nullptr, "PulpDoesNotShipThisFamily-932",
        SkFontStyle{SkFontStyle::kNormal_Weight, SkFontStyle::kNormal_Width,
                    SkFontStyle::kUpright_Slant});
    REQUIRE(miss == nullptr);
}

// pulp #1737 (#932 followup) — comma-separated CSS family-list fallback
// chain. CSS spec: `font-family: 'Definitely-Not-Installed-Family,
// JetBrains Mono'` should fall back to the second family when the
// first doesn't resolve. Pre-fix the bridge stripped to the first
// family and the canvas tried only that one — silently dropping the
// fallback. The new get_cached_typeface walks the whole list through
// the existing match cascade until one resolves.
//
// We measure text via measure_text_with_font (same lookup path
// canvas.set_font() uses). The fallback to JetBrains Mono produces
// a positive width for "iiii"; if the first-family-only behaviour
// regressed (and SkFontMgr returned a default that doesn't include
// the test glyphs), the fallback would silently give a different
// width. Compare against the JetBrains-Mono-direct measurement.
TEST_CASE("SkiaCanvas comma-list fontFamily falls back through SkFontMgr (#932)",
          "[canvas][skia][fonts][issue-932][issue-1737]") {
    // Direct resolution of the bundled family.
    auto direct = SkiaCanvas::measure_text_with_font(
        "JetBrains Mono", 16.0f, "iiii");
    REQUIRE(direct.width > 0.0f);
    // Comma list with an unavailable first family + JetBrains Mono
    // second. Should resolve to JetBrains Mono and produce the
    // SAME width as the direct path (modulo platform float wobble).
    auto fallback = SkiaCanvas::measure_text_with_font(
        "PulpDefinitelyNotInstalled-1737, JetBrains Mono", 16.0f, "iiii");
    REQUIRE(fallback.width > 0.0f);
    REQUIRE_THAT(fallback.width, Catch::Matchers::WithinAbs(direct.width, 0.5f));
}

// pulp #1737 (#932 followup) — CSS family-list with quoted segments,
// extra whitespace, and a single-family input (no comma) all parse
// correctly. The single-family fast-path must remain identical to
// pre-fix behaviour — get_cached_typeface_single is delegated to
// directly when there's no comma.
TEST_CASE("SkiaCanvas comma-list fontFamily handles quotes + whitespace (#932)",
          "[canvas][skia][fonts][issue-932][issue-1737]") {
    auto direct = SkiaCanvas::measure_text_with_font(
        "JetBrains Mono", 16.0f, "ab");
    REQUIRE(direct.width > 0.0f);
    // Quoted family name + extra whitespace.
    auto quoted = SkiaCanvas::measure_text_with_font(
        "  \"PulpMissing-A\" ,  'JetBrains Mono'  ", 16.0f, "ab");
    REQUIRE_THAT(quoted.width, Catch::Matchers::WithinAbs(direct.width, 0.5f));
    // Single-family no-comma fast path — identical to pre-fix.
    auto single = SkiaCanvas::measure_text_with_font(
        "JetBrains Mono", 16.0f, "ab");
    REQUIRE_THAT(single.width, Catch::Matchers::WithinAbs(direct.width, 0.001f));
}

TEST_CASE("SkiaCanvas::measure_text_with_font picks up bundled "
          "JetBrains Mono (#932)",
          "[canvas][skia][fonts][issue-932]") {
    // End-to-end through the same lookup path canvas.set_font() takes:
    // make_font → get_cached_typeface → bundled-font cache. Width must be
    // strictly positive — the fallback (no typeface) returns a synthesised
    // 0.5 * size * len estimate, but a real typeface produces glyph-derived
    // advances that vary with content. Compare two strings of different
    // length to confirm we're going through real font metrics.
    auto a = SkiaCanvas::measure_text_with_font("JetBrains Mono", 16.0f, "i");
    auto b = SkiaCanvas::measure_text_with_font("JetBrains Mono", 16.0f,
                                                 "iiiiiiiiii");
    REQUIRE(a.width > 0.0f);
    REQUIRE(b.width > a.width);
}

// ── pulp #1150 — public font-registration API ───────────────────────────────
// The public `register_font` / `register_font_file` / `is_font_registered`
// surface declared in `pulp/canvas/bundled_fonts.hpp` is the path plugin
// authors take to make their own bundled .ttf resolve through
// `canvas.set_font()` and `setFontFamily()`. Before #1150,
// `AssetManager::register_font_family` existed but was never consulted by
// SkFontMgr — every plugin font fell through silently to the platform
// matcher (or to a nullptr typeface).
//
// `PULP_TEST_FONT_PATH` is wired in via test/CMakeLists.txt and points at
// `external/fonts/Inter-Regular.ttf` so we have a deterministic .ttf that
// is guaranteed to exist on every supported host. We deliberately register
// it under an *override* family name ("PulpRegistrationTestFamily-1150")
// so the test doesn't fight the bundled-font cache (which already knows
// about "Inter").

#ifndef PULP_TEST_FONT_PATH
#error "PULP_TEST_FONT_PATH must be defined by test/CMakeLists.txt — points "
       "at external/fonts/Inter-Regular.ttf for the #1150 registration tests."
#endif

TEST_CASE("register_font_file resolves a custom family through Skia (#1150)",
          "[canvas][skia][fonts][issue-1150]") {
    const std::string family = "PulpRegistrationTestFamily-1150";

    // Pre-condition: the family must not be registered yet on this fresh
    // process. Catch2 runs cases in random order, but no other case in
    // this binary registers under the same name.
    REQUIRE_FALSE(pulp::canvas::is_font_registered(family));

    const bool ok = pulp::canvas::register_font_file(PULP_TEST_FONT_PATH,
                                                     family);
    if (!ok) {
        // The Skia prebuilt this binary links against has no platform
        // font manager wired in (e.g. Linux without fontconfig). The
        // public API is documented to return false in that case so the
        // caller can degrade gracefully — assert that contract instead
        // of failing the case on a host that legitimately can't.
        SUCCEED("register_font_file returned false — no platform font "
                "manager available in this build, registration is a "
                "documented soft-fail.");
        return;
    }

    REQUIRE(pulp::canvas::is_font_registered(family));

    // The whole point of registration: the family becomes resolvable
    // through the same path skia_canvas.cpp / text_shaper.cpp use for
    // bundled and platform fonts. `match_registered_typeface` is the
    // narrowest probe; `SkiaCanvas::measure_text_with_font` is the
    // end-to-end check.
    SkFontStyle upright_normal{SkFontStyle::kNormal_Weight,
                               SkFontStyle::kNormal_Width,
                               SkFontStyle::kUpright_Slant};
    auto face = pulp::canvas::match_registered_typeface(family,
                                                        upright_normal);
    REQUIRE(face != nullptr);

    auto shaped = SkiaCanvas::measure_text_with_font(family, 16.0f,
                                                     "Hello, world!");
    REQUIRE(shaped.width > 0.0f);

    // Style miss: the registered face is Regular/Upright. Asking for
    // Bold MUST return nullptr so skia_canvas's cascade keeps walking
    // (matchFamilyStyle can synthesise a faux-bold or pick a system
    // Bold). Without this guard, registered fonts would hijack every
    // weight/slant variant of the same family — exactly the regression
    // bundled fonts already protect against (Codex P2 on PR #956).
    SkFontStyle bold_normal{SkFontStyle::kBold_Weight,
                            SkFontStyle::kNormal_Width,
                            SkFontStyle::kUpright_Slant};
    auto bold_miss = pulp::canvas::match_registered_typeface(family,
                                                              bold_normal);
    REQUIRE(bold_miss == nullptr);

    // Same guard on the slant axis: the registered face is Upright, so an
    // Italic request must also miss the registry and let the cascade walk
    // on to a real system Italic. match_registered_typeface requires the
    // slant to match exactly — a faux-italic Upright face must never be
    // returned for an Italic query.
    SkFontStyle italic_normal{SkFontStyle::kNormal_Weight,
                              SkFontStyle::kNormal_Width,
                              SkFontStyle::kItalic_Slant};
    auto italic_miss = pulp::canvas::match_registered_typeface(family,
                                                                italic_normal);
    REQUIRE(italic_miss == nullptr);
}

TEST_CASE("register_font is idempotent — re-registering the same family is "
          "safe (#1150)",
          "[canvas][skia][fonts][issue-1150]") {
    const std::string family = "PulpRegistrationIdempotentTest-1150";

    REQUIRE_FALSE(pulp::canvas::is_font_registered(family));

    const bool first = pulp::canvas::register_font_file(PULP_TEST_FONT_PATH,
                                                        family);
    if (!first) {
        SUCCEED("Soft-fail on this build (no platform SkFontMgr). Skipping "
                "idempotence assertion.");
        return;
    }
    REQUIRE(pulp::canvas::is_font_registered(family));

    // Second call with the same family must succeed and leave the family
    // resolvable. A "second registration tears down the first" bug would
    // surface as `is_font_registered == false` after the second call.
    const bool second = pulp::canvas::register_font_file(PULP_TEST_FONT_PATH,
                                                         family);
    REQUIRE(second);
    REQUIRE(pulp::canvas::is_font_registered(family));

    SkFontStyle upright_normal{SkFontStyle::kNormal_Weight,
                               SkFontStyle::kNormal_Width,
                               SkFontStyle::kUpright_Slant};
    auto face = pulp::canvas::match_registered_typeface(family,
                                                        upright_normal);
    REQUIRE(face != nullptr);
}

TEST_CASE("Unregistered families don't resolve through the registry (#1150)",
          "[canvas][skia][fonts][issue-1150]") {
    // Negative case: an unknown family must miss the registry. The
    // skia_canvas cascade falls through to `match_bundled_typeface` and
    // then `SkFontMgr::matchFamilyStyle` — those are exercised
    // separately. The contract here is "registry only returns what was
    // explicitly registered, never a platform-matched fallback".
    const std::string family = "PulpUnregisteredFamily-1150";
    REQUIRE_FALSE(pulp::canvas::is_font_registered(family));

    SkFontStyle upright_normal{SkFontStyle::kNormal_Weight,
                               SkFontStyle::kNormal_Width,
                               SkFontStyle::kUpright_Slant};
    auto face = pulp::canvas::match_registered_typeface(family,
                                                        upright_normal);
    REQUIRE(face == nullptr);

    // Empty inputs must also miss without crashing.
    REQUIRE_FALSE(pulp::canvas::is_font_registered(""));
    REQUIRE(pulp::canvas::match_registered_typeface("", upright_normal)
            == nullptr);

    // register_font with null/zero data must reject cleanly.
    REQUIRE_FALSE(pulp::canvas::register_font(nullptr, 0, "Anything"));

    // register_font_file with a non-existent path must reject cleanly.
    REQUIRE_FALSE(pulp::canvas::register_font_file(
        "/this/path/does/not/exist/font.ttf", "AlsoAnything"));
}

// pulp #1350 — fill_rect / fill_rounded_rect / fill_circle on SkiaCanvas
// must honor an active linear gradient set via set_fill_gradient_linear,
// matching the behavior of fill_current_path. Pre-fix the rect-family
// helpers all went through a free `make_fill_paint(Color)` that only
// knew about the solid fill color, so a Canvas2D consumer that called
// `ctx.fillStyle = ctx.createLinearGradient(...); ctx.fillRect(...)`
// got a flat first-stop color instead of the gradient.
TEST_CASE("SkiaCanvas::fill_rect honors active linear gradient",
          "[canvas][skia][gradient][issue-1350]") {
    constexpr int kW = 64;
    constexpr int kH = 8;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);
    sk_canvas->clear(SK_ColorBLACK);

    SkiaCanvas canvas(sk_canvas);

    Color stops[2] = {
        Color::rgba(1.0f, 0.0f, 0.0f, 1.0f),  // red at x=0
        Color::rgba(0.0f, 1.0f, 0.0f, 1.0f),  // green at x=kW
    };
    float positions[2] = {0.0f, 1.0f};
    canvas.set_fill_gradient_linear(0.0f, 0.0f,
                                     static_cast<float>(kW), 0.0f,
                                     stops, positions, 2);
    canvas.fill_rect(0.0f, 0.0f,
                     static_cast<float>(kW), static_cast<float>(kH));

    // Read back two pixels at the gradient endpoints. If the rect ignored
    // the gradient and used the solid fill_color_ default, both pixels
    // would be identical white. With the fix they must differ — and the
    // left pixel must skew red while the right pixel skews green.
    SkPixmap pm;
    REQUIRE(surface->peekPixels(&pm));
    SkColor left = pm.getColor(2, kH / 2);
    SkColor right = pm.getColor(kW - 3, kH / 2);

    REQUIRE(left != right);
    REQUIRE(SkColorGetR(left)  > SkColorGetG(left));   // left is red-dominant
    REQUIRE(SkColorGetG(right) > SkColorGetR(right));  // right is green-dominant
}

#endif  // PULP_HAS_SKIA
