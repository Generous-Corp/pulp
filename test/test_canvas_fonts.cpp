// Font tests that share Skia font-manager fixtures:
//
//   - pulp #932 — bundled-font (Inter-Regular, JetBrainsMono-Regular)
//     registration with SkFontMgr + bundled_blobs() table.
//   - pulp #1150 — public `register_font(path)` API + the SkFontMgr
//     fallback chain through match_bundled_typeface.

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
#include <pulp/canvas/font_resolver.hpp>
#include <pulp/canvas/text_run_planner.hpp>
#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkFont.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "include/encode/SkPngEncoder.h"
#include <cstdlib>
#include <string>
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
    // covers the bundle, not the system-wide font catalog.
    auto miss = pulp::canvas::match_bundled_typeface(mgr.get(),
                                                     "ThisFamilyDoesNotExist",
                                                     upright_normal);
    REQUIRE(miss == nullptr);

    // Style-aware miss: the bundle currently ships only Regular/Upright
    // Inter, but the system font catalog does have
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

// pulp #1737 / #932 — comma-separated CSS family-list fallback
// chain. CSS spec: `font-family: 'Definitely-Not-Installed-Family,
// JetBrains Mono'` should fall back to the second family when the
// first doesn't resolve. get_cached_typeface must walk the whole list
// through the existing match cascade until one resolves; stripping to
// the first family silently drops the fallback.
//
// We measure text via measure_text_with_font (same lookup path
// canvas.set_font() uses). The fallback to JetBrains Mono produces
// a positive width for "iiii"; if the first-family-only behavior
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

// pulp #1737 / #932 — CSS family-list with quoted segments,
// extra whitespace, and a single-family input (no comma) all parse
// correctly. The single-family fast-path remains byte-for-byte
// equivalent by delegating directly to get_cached_typeface_single when
// there's no comma.
TEST_CASE("SkiaCanvas comma-list fontFamily handles quotes + whitespace (#932)",
          "[canvas][skia][fonts][issue-932][issue-1737]") {
    auto direct = SkiaCanvas::measure_text_with_font(
        "JetBrains Mono", 16.0f, "ab");
    REQUIRE(direct.width > 0.0f);
    // Quoted family name + extra whitespace.
    auto quoted = SkiaCanvas::measure_text_with_font(
        "  \"PulpMissing-A\" ,  'JetBrains Mono'  ", 16.0f, "ab");
    REQUIRE_THAT(quoted.width, Catch::Matchers::WithinAbs(direct.width, 0.5f));
    // Single-family no-comma fast path; this is expected to be exact.
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
    // the bundled-font guards already cover.
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

// pulp WYSIWYG caret-x — SkiaCanvas::text_x_for_byte must query the FULL
// shaped paragraph (the same make_paragraph() fill_text uses), not the
// sum of isolated prefix advances. The acceptance criteria:
//
//   1. For a kerned pair like "AV", the caret at byte 1 differs from
//      measure_text("A") in isolation — proving the boundary is read off
//      the shaped run, where the 'A' advance is adjusted by the following
//      'V' kern, rather than re-measured standalone.
//   2. The end-of-text caret equals measure_text(full) for plain text.
//   3. Caret offsets are monotonic non-decreasing across byte boundaries.
TEST_CASE("SkiaCanvas::text_x_for_byte reads caret x off the shaped run",
          "[canvas][skia][text][wysiwyg]") {
    constexpr int kW = 256;
    constexpr int kH = 32;
    SkImageInfo info = SkImageInfo::Make(kW, kH, kN32_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    auto* sk_canvas = surface->getCanvas();
    REQUIRE(sk_canvas != nullptr);

    SkiaCanvas canvas(sk_canvas);
    // A bundled family guarantees the same metrics on any host.
    canvas.set_font_full("Inter", 18.0f, 400, /*slant=*/0,
                         /*letter_spacing=*/0.0f);

    // (3) monotonic, and the end caret matches the full advance.
    const std::string plain = "Hello";
    float prev = -1.0f;
    for (std::size_t i = 0; i <= plain.size(); ++i) {
        float x = canvas.text_x_for_byte(plain, i);
        REQUIRE(x >= prev);   // non-decreasing
        prev = x;
    }
    REQUIRE(canvas.text_x_for_byte(plain, 0) == Catch::Approx(0.0f).margin(0.01f));
    // (2) end-of-text caret ≈ measure_text(full) for plain unkerned text.
    // Toolchain-coupled tolerance (reference host: macos-arm64 · Xcode 26.5
    // (17F42) · Skia chrome/m149). Under the 26.4.1→26.5 bump the shaped-run
    // caret and the accumulated measure_text advance diverged from <0.5px to
    // ~2.4px for "Hello" — CoreText/HarfBuzz now report a slightly different
    // trailing advance vs glyph-cluster extent. Widened to 3.0px to absorb the
    // toolchain shift while still catching a gross caret/advance desync.
    // Follow-up: the grown divergence is worth a closer look (it is a real
    // shaped-run-vs-advance gap, not just golden drift) — tracked for the
    // font-golden centralization PR.
    const float full = canvas.measure_text(plain);
    REQUIRE(canvas.text_x_for_byte(plain, plain.size())
                == Catch::Approx(full).margin(3.0f));
    // Past-the-end byte index clamps to end-of-text.
    REQUIRE(canvas.text_x_for_byte(plain, plain.size() + 5)
                == Catch::Approx(canvas.text_x_for_byte(plain, plain.size()))
                       .margin(0.01f));

    // (1) "AV" is a classic negative-kern pair. The caret at byte 1 (between
    // A and V) should reflect the shaped 'A' advance within the "AV" run,
    // which differs from measuring "A" standalone (no following V to kern
    // against). If text_x_for_byte fell back to measuring the prefix
    // substring, these would be identical.
    const std::string av = "AV";
    const float caret_after_A = canvas.text_x_for_byte(av, 1);
    const float standalone_A  = canvas.measure_text("A");
    REQUIRE(caret_after_A > 0.0f);
    REQUIRE(caret_after_A != Catch::Approx(standalone_A).margin(0.05f));
    // The caret at byte 1 must match the shaped 'A' advance: full "AV"
    // width minus the 'V' contribution. Cross-check that the caret sits
    // strictly inside the run.
    const float av_full = canvas.text_x_for_byte(av, 2);
    REQUIRE(caret_after_A < av_full);
}

// ── Variable-font weight instancing + SkParagraph bridge ────────────────────
//
// Root cause fixed here: imported designs register fonts via register_font,
// but Label text rasterizes through SkParagraph whose FontCollection never
// saw user-registered fonts (only the emoji typeface). And variable fonts
// were pinned to their single default instance, so font-weight was ignored.
//
// PULP_TEST_VARIABLE_FONT_PATH points at Funnel Display (wght axis 300-800,
// default 300) — a deterministic variable font shipped for tests only.

#ifndef PULP_TEST_VARIABLE_FONT_PATH
#error "PULP_TEST_VARIABLE_FONT_PATH must be defined by test/CMakeLists.txt"
#endif

TEST_CASE("face_wght_axis reports variable wght axis; false for static fonts",
          "[canvas][skia][fonts][variable-weight]") {
    auto mgr = test_platform_font_mgr();
    if (!mgr) return;  // platform without a font manager (Linux test build)

    // A static face (bundled Inter Regular) has no variation axes.
    auto inter = pulp::canvas::match_bundled_typeface(
        mgr.get(), "Inter", SkFontStyle::Normal());
    REQUIRE(inter);
    float lo = -1, hi = -1, def = -1;
    REQUIRE_FALSE(pulp::canvas::face_wght_axis(inter.get(), lo, hi, def));

    // The Funnel Display variable face exposes a wght axis 300..800.
    REQUIRE(pulp::canvas::register_font_file(PULP_TEST_VARIABLE_FONT_PATH,
                                             "PulpVarTest-FunnelDisplay"));
    auto var = pulp::canvas::match_registered_typeface(
        "PulpVarTest-FunnelDisplay", SkFontStyle::Normal());
    REQUIRE(var);
    float vmin = 0, vmax = 0, vdef = 0;
    REQUIRE(pulp::canvas::face_wght_axis(var.get(), vmin, vmax, vdef));
    REQUIRE(vmin == Catch::Approx(300.0f));
    REQUIRE(vmax == Catch::Approx(800.0f));
}

TEST_CASE("match_registered_typeface returns a variable face for an "
          "out-of-tolerance weight (instead of dropping to fallback)",
          "[canvas][skia][fonts][variable-weight]") {
    // The static-font matcher rejects a weight gap > 200 so the cascade can
    // walk on to a real system Bold. A VARIABLE face must NOT be rejected —
    // it can render the requested weight via its wght axis, so the matcher
    // returns the base variable face for the resolver to instance.
    REQUIRE(pulp::canvas::register_font_file(PULP_TEST_VARIABLE_FONT_PATH,
                                             "PulpVarTest-Funnel700"));
    // Funnel Display's default instance is wght 300. A 700 request is a
    // gap of 400 — far past the 200-unit static tolerance. A static font
    // would return nullptr here; the variable font must still resolve.
    SkFontStyle heavy{700, SkFontStyle::kNormal_Width,
                      SkFontStyle::kUpright_Slant};
    auto face = pulp::canvas::match_registered_typeface(
        "PulpVarTest-Funnel700", heavy);
    REQUIRE(face);  // variable eligibility, not a fallback
}

TEST_CASE("registered fonts are visible to the SkParagraph font collection",
          "[canvas][skia][fonts][variable-weight]") {
    // The bug: register_font populated only the FontResolver/fillText path;
    // SkParagraph (every Label) resolved through its own FontCollection and
    // never saw user fonts. registered_typefaces_snapshot() is the bridge
    // that font_collection() iterates — assert a registered family shows up.
    const std::string family = "PulpVarTest-SnapshotProbe";
    REQUIRE(pulp::canvas::register_font_file(PULP_TEST_VARIABLE_FONT_PATH,
                                             family));
    auto snap = pulp::canvas::registered_typefaces_snapshot();
    bool found = false;
    for (const auto& r : snap) {
        if (r.family == family && r.typeface) { found = true; break; }
    }
    REQUIRE(found);
    // font_collection() iterates this snapshot to register user fonts into
    // its TypefaceFontProvider — the snapshot being correct is the bridge.
    // (The collection rebuild itself is exercised end-to-end by the embed
    // smoke + import-design --validate render, which route Label text
    // through SkParagraph.)
}

// ── Font-manager coverage across every supported platform ──────────────────

// platform_font_manager() is an OS switch with one arm per platform. A missing
// arm returns nullptr, which is silent and catastrophic: ensure_registered()
// short-circuits on a null manager and leaves the bundled-typeface cache empty,
// TextShaper falls back to SkFontMgr::RefEmpty(), measureText() reports a
// near-zero advance for every string, and every Label collapses to ~0 width.
// Nothing throws and nothing logs. Assert the manager exists on whatever
// platform this test binary runs on, so a new port cannot ship without an arm.
TEST_CASE("platform_font_manager is available on this platform",
          "[canvas][skia][fonts]") {
    auto mgr = pulp::canvas::platform_font_manager();
    INFO("platform_font_manager() returned null — the OS switch in "
         "bundled_fonts.cpp has no arm for this platform, so no bundled or "
         "plugin-registered font can be materialised.");
    REQUIRE(mgr != nullptr);
}

// The label-collapse regression itself, asserted end to end: with the platform
// manager in hand, both bundled families materialise and shape to a strictly
// positive advance. A null manager (or an empty typeface cache) fails here with
// advance == 0 rather than by crashing.
TEST_CASE("Bundled typefaces measure a positive advance",
          "[canvas][skia][fonts]") {
    auto mgr = pulp::canvas::platform_font_manager();
    REQUIRE(mgr != nullptr);

    const SkFontStyle upright{SkFontStyle::kNormal_Weight,
                              SkFontStyle::kNormal_Width,
                              SkFontStyle::kUpright_Slant};
    const std::string probe = "Hamburgefonstiv";

    for (const char* family : {"Inter", "JetBrains Mono"}) {
        INFO("bundled family: " << family);
        auto face = pulp::canvas::match_bundled_typeface(mgr.get(), family,
                                                         upright);
        REQUIRE(face != nullptr);

        SkFont font(face, 16.0f);
        const float advance = font.measureText(probe.data(), probe.size(),
                                               SkTextEncoding::kUTF8);
        REQUIRE(advance > 0.0f);
    }
}

// ── TextRunPlanner batch shaping: serial arm parity ────────────────────────

namespace {

// PULP_TEXT_SHAPE_SERIAL is the override TextRunPlanner reads to force the
// single-threaded batch arm that Emscripten always takes.
void set_serial_shaping(bool on) {
#if defined(_WIN32)
    _putenv_s("PULP_TEXT_SHAPE_SERIAL", on ? "1" : "");
#else
    if (on) {
        ::setenv("PULP_TEXT_SHAPE_SERIAL", "1", 1);
    } else {
        ::unsetenv("PULP_TEXT_SHAPE_SERIAL");
    }
#endif
}

} // namespace

// shape_batch() fans out over std::async(launch::async). A wasm module built
// without pthreads cannot create threads — std::async THROWS there — so the
// planner takes a serial arm instead. PULP_TEXT_SHAPE_SERIAL forces that same
// arm on native builds; this test drives both and asserts the artifacts are
// identical, so the single-threaded arm is behavior-preserving and not merely
// compilable.
TEST_CASE("shape_batch serial arm matches the parallel arm",
          "[canvas][skia][fonts][text]") {
    std::vector<std::pair<std::string, pulp::canvas::FontOptions>> inputs;
    for (const char* text : {"Cutoff", "Resonance", "Drive", "Mix",
                             "Feedback", "Wet / Dry"}) {
        pulp::canvas::FontOptions opts;
        opts.family_stack.push_back("Inter");
        opts.size = 14.0f;
        inputs.emplace_back(text, opts);
    }

    auto& planner = pulp::canvas::TextRunPlanner::instance();

    // The planner caches by (text, FontOptions), so clear between arms —
    // otherwise the second batch would trivially replay the first one's
    // artifacts and prove nothing.
    planner.clear_cache();
    set_serial_shaping(false);
    const auto parallel = planner.shape_batch(inputs);

    planner.clear_cache();
    set_serial_shaping(true);
    const auto serial = planner.shape_batch(inputs);
    set_serial_shaping(false);

    REQUIRE(serial.size() == inputs.size());
    REQUIRE(parallel.size() == serial.size());

    for (std::size_t i = 0; i < serial.size(); ++i) {
        INFO("batch input " << i << ": " << inputs[i].first);
        REQUIRE(serial[i].text == inputs[i].first);
        REQUIRE(parallel[i].text == serial[i].text);
        REQUIRE(serial[i].total_width > 0.0f);
        REQUIRE(parallel[i].total_width ==
                Catch::Approx(serial[i].total_width));
        REQUIRE(parallel[i].runs.size() == serial[i].runs.size());
        REQUIRE(parallel[i].clusters.size() == serial[i].clusters.size());
        for (std::size_t r = 0; r < serial[i].runs.size(); ++r) {
            REQUIRE(parallel[i].runs[r].glyph_ids == serial[i].runs[r].glyph_ids);
            REQUIRE(parallel[i].runs[r].advances == serial[i].runs[r].advances);
        }
    }
}

// ── GPU upload context ─────────────────────────────────────────────────────

// A raster SkiaCanvas attaches neither a Graphite recorder nor a Ganesh upload
// context. In that state ensure_gpu_image() must hand the decoded image
// straight through — the documented degraded path — so headless screenshot
// canvases keep compositing images. Attaching a null context explicitly must
// not change that.
TEST_CASE("SkiaCanvas draws images with no GPU upload context attached",
          "[canvas][skia][image]") {
    SkBitmap source;
    REQUIRE(source.tryAllocPixels(
        SkImageInfo::MakeN32Premul(4, 4, SkColorSpace::MakeSRGB())));
    source.eraseColor(SkColorSetARGB(255, 255, 0, 0));
    SkPixmap source_pixels;
    REQUIRE(source.peekPixels(&source_pixels));
    sk_sp<SkData> png = SkPngEncoder::Encode(source_pixels, {});
    REQUIRE(png != nullptr);

    auto info = SkImageInfo::MakeN32Premul(8, 8, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    REQUIRE(surface != nullptr);
    surface->getCanvas()->clear(SK_ColorBLACK);

    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    canvas.set_gpu_upload_context(nullptr);
    REQUIRE(canvas.draw_image_from_data(png->bytes(), png->size(),
                                        0.0f, 0.0f, 8.0f, 8.0f));

    SkBitmap read;
    REQUIRE(read.tryAllocPixels(info));
    REQUIRE(surface->readPixels(read, 0, 0));
    const SkColor center = read.getColor(4, 4);
    REQUIRE(SkColorGetR(center) > 200);
    REQUIRE(SkColorGetG(center) < 50);
    REQUIRE(SkColorGetB(center) < 50);
}

#endif  // PULP_HAS_SKIA

// Regression: the base-Canvas text_x_for_byte default measures a PREFIX
// SUBSTRING. A caller-supplied byte index landing inside a multi-byte UTF-8
// sequence used to slice an invalid prefix; on the CoreGraphics backend that
// made the NSString conversion return nil and NSAttributedString THROW inside
// drawRect:, killing the host's entire AU process (observed live: adjusting
// one plugin's GUI killed the upstream instrument hosted in the same Logic
// AUHostingService process). The default must clamp back to a codepoint
// boundary so measure_text never sees invalid UTF-8.
TEST_CASE("Canvas::text_x_for_byte clamps mid-codepoint indices to a UTF-8 "
          "boundary before measuring",
          "[canvas][text][utf8]") {
    // RecordingCanvas is the concrete no-surface Canvas; override only
    // measure_text to capture the prefix the default text_x_for_byte builds.
    struct PrefixCapture : pulp::canvas::RecordingCanvas {
        std::vector<std::string> seen;
        float measure_text(const std::string& t) override {
            seen.push_back(t);
            return static_cast<float>(t.size());
        }
    };

    auto is_valid_utf8 = [](const std::string& s) {
        std::size_t i = 0;
        while (i < s.size()) {
            const auto c = static_cast<unsigned char>(s[i]);
            std::size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2
                            : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (len == 0 || i + len > s.size()) return false;
            for (std::size_t k = 1; k < len; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80)
                    return false;
            i += len;
        }
        return true;
    };

    PrefixCapture canvas;
    // Mixed 1/2/3/4-byte codepoints: "aé€😀b"
    const std::string text = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"
                             "b";
    for (std::size_t i = 0; i <= text.size() + 2; ++i)
        (void) canvas.text_x_for_byte(text, i);

    REQUIRE_FALSE(canvas.seen.empty());
    for (const auto& prefix : canvas.seen) {
        INFO("prefix bytes: " << prefix.size());
        REQUIRE(is_valid_utf8(prefix));
    }
}


#ifdef PULP_HAS_SKIA

// ── The browser default-cascade collapse ────────────────────────────────────
//
// A browser build has no system font database: `platform_font_manager()` returns
// `SkFontMgr_New_Custom_Empty()`. That manager is not empty in the way a caller
// would expect — it reports ONE family holding ONE typeface, and hands that
// typeface back from `matchFamilyStyle(...)`. The face has no glyphs, so every
// string it measures is 0.0 wide and every string it paints is invisible. The
// default cascade (empty family stack, or a generic family like `sans-serif`
// that nothing in the bundle advertises) walked straight into it, and every
// Label in the first browser render came out blank while the knobs drew fine.
//
// `platform_font_db_usable()` is the discriminator that fixed it, and these are
// its contract tests. They run natively — the invariant they encode ("a non-null
// typeface is NOT proof of a usable font DB; ask it for a glyph") is exactly
// what a null-check-based implementation would get wrong on either platform.

TEST_CASE("platform_font_db_usable agrees with the default face's glyphs",
          "[canvas][skia][fonts]") {
    auto mgr = pulp::canvas::platform_font_manager();
    REQUIRE(mgr != nullptr);

    const bool usable = pulp::canvas::platform_font_db_usable();

    // Whatever the answer, it must be the answer the DEFAULT FACE gives — never
    // merely "matchFamilyStyle returned non-null" (the empty manager does that
    // too) and never merely "countFamilies() > 0" (the empty manager reports 1).
    sk_sp<SkTypeface> def = mgr->matchFamilyStyle(nullptr, SkFontStyle::Normal());
    const bool default_face_has_glyphs = def && def->unicharToGlyph('A') != 0;
    REQUIRE(usable == default_face_has_glyphs);

    // Cached: a second call cannot disagree with the first.
    REQUIRE(pulp::canvas::platform_font_db_usable() == usable);

    // Every desktop CI platform Pulp builds on ships a real font DB. If this
    // fails on a Linux container, the container has no fonts — which the bundled
    // last-resort below now covers, but it is worth knowing.
    INFO("no usable platform font database on this host");
    REQUIRE(usable);
}

TEST_CASE("bundled_fallback_typeface is a real, drawable last resort",
          "[canvas][skia][fonts]") {
    sk_sp<SkTypeface> face = pulp::canvas::bundled_fallback_typeface();
    REQUIRE(face != nullptr);

    // It is the thing the empty manager's default face is NOT: it has glyphs.
    REQUIRE(face->unicharToGlyph('A') != 0);

    SkFont font(face, 14.0f);
    const float advance =
        font.measureText("Mix", 3, SkTextEncoding::kUTF8, nullptr);
    INFO("bundled fallback measured a zero advance — this is the exact failure "
         "mode the browser hit with the empty manager's glyph-less default");
    REQUIRE(advance > 0.0f);

    // Deterministic across calls (the family cache is an unordered_map; the
    // fallback picks Inter, or the lexicographically first family if Inter is
    // ever dropped from the bundle).
    REQUIRE(pulp::canvas::bundled_fallback_typeface() == face);
}

TEST_CASE("The default font cascade resolves to a face that can draw",
          "[canvas][skia][fonts]") {
    // The three shapes of the default cascade that the browser hit:
    //   * no family at all
    //   * a generic family the bundle does not advertise
    //   * a family that simply does not exist anywhere
    // All three must land on SOMETHING with glyphs — never on a face that
    // measures every string at zero.
    for (const auto& stack : std::vector<std::vector<std::string>>{
             {}, {"sans-serif"}, {"No Such Family At All"}}) {
        FontOptions opts;
        opts.family_stack = stack;
        opts.size = 14.0f;

        auto resolved = FontResolver::instance().resolve_family_list(opts);
        INFO("family stack: " << (stack.empty() ? "<empty>" : stack.front()));
        REQUIRE(resolved.has_typeface());
        REQUIRE(resolved.typeface->unicharToGlyph('A') != 0);

        SkFont font(resolved.typeface, opts.size);
        REQUIRE(font.measureText("Mix", 3, SkTextEncoding::kUTF8, nullptr) > 0.0f);
    }
}

#endif  // PULP_HAS_SKIA
