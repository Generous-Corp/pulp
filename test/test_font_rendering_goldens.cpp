// test_font_rendering_goldens.cpp — cross-backend font rendering goldens.
// (Cross-backend rendering goldens).
//
// Captures a small, fast, deterministic golden hash of three
// bundled-font glyph rasters so a regression in the Skia raster
// path (font cascade, glyph metrics, hinting policy, AA settings,
// or upstream Skia bump) fails CI before it lands. The goal is
// *guard against drift*, not pixel-perfect bit-identical rendering
// across hosts.
//
// ────────────────────────────────────────────────────────────────
// Design choices (read this header before "fixing" a failure):
//
//  1. Goldens live in the source file (not on disk). The
//     committed expected values are the contract for each CI host
//     class — a real font regression manifests as a hash mismatch
//     on a host where the rendering used to be stable. There is no
//     golden-file sync workflow to forget about. If a Skia bump
//     genuinely changes rendering, update the constants in this
//     file deliberately as part of the bump PR.
//
//  2. We use a *structural* digest, not a byte-exact pixel hash.
//     The structural fingerprint is
//         (width, height, opaque_pixel_count, sum_of_luminance)
//     with the opaque-pixel count and the luminance sum tolerant
//     to ±5% drift. Exact-pixel goldens are brittle across
//     Skia point releases, hinter changes, and AMD/Intel/Apple
//     subpixel rounding — and we already have a stricter
//     "did this render at all" guard from the parity harness.
//     The cross-backend guard wants "the cascade picked the
//     right typeface and produced ink in the expected
//     neighbourhood", which is what a structural digest answers.
//
//  3. Three strings, three scenarios:
//       a. "Hello"        / Inter / 14px       — Latin happy path
//       b. "日本語"        / Inter / 14px       — CJK fallback path
//       c. "Hello world"  / JetBrains Mono / 12px — mono path
//     Each is rendered onto a 128x32 RGBA8 Skia raster surface
//     and the digest is compared to a committed constant.
//
//  4. A determinism probe — the same string rendered twice in
//     the same process must produce *bit-identical* pixels.
//     If that ever fails, it's a render-pipeline non-determinism
//     bug (e.g. uninitialised glyph-cache state, racy lazy init)
//     and not a golden-file issue. Treat it as a render-pipeline
//     investigation rather than relaxing the golden.
//
//  5. On mismatch we dump the actual bitmap to a stable path
//     so a developer can eyeball the diff. We bypass
//     `render_to_file` (which is a view-layer API and pulls in
//     more dependencies than the canvas test wants) and write
//     the PNG via Skia's `SkPngEncoder` directly.
//
// Tag: [golden][skia][font]

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/font_options.hpp>
#include <pulp/canvas/font_resolver.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/encode/SkPngEncoder.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#endif

using namespace pulp::canvas;

#ifdef PULP_HAS_SKIA

namespace {

// Canvas dimensions chosen to keep the test fast (small bitmap,
// small hash domain) while leaving enough horizontal headroom for
// a 12-char monospace string at 12px.
constexpr int kWidth  = 128;
constexpr int kHeight = 32;

// Glyph baseline inside the bitmap. Picked so ascender + descender
// stay inside the surface for the 14px and 12px sizes used below.
constexpr float kBaselineY = 22.0f;

// Tolerance for the *count* fields of the structural digest. Five
// percent is generous enough to absorb subpixel AA rounding drift
// across Skia point releases but tight enough to catch a real
// cascade regression (e.g. CJK falling back to tofu boxes, which
// would slash the opaque-pixel count by far more than 5%).
constexpr double kCountToleranceRatio = 0.05;

// ── Structural digest helpers ───────────────────────────────────

struct Digest {
    int width = 0;
    int height = 0;
    // Count of pixels with alpha > 0 in the rendered glyph layer.
    // For black-on-white text this is "ink coverage in pixels".
    uint32_t opaque_pixels = 0;
    // Sum of inverted luminance (255 - R) over all pixels — gives
    // a coarse "how dark did this render" fingerprint that is
    // sensitive to AA settings but tolerant of subpixel jitter.
    uint64_t darkness_sum = 0;
};

Digest digest_from_pixmap(const SkPixmap& pm) {
    Digest d;
    d.width = pm.width();
    d.height = pm.height();
    for (int y = 0; y < pm.height(); ++y) {
        for (int x = 0; x < pm.width(); ++x) {
            const SkColor c = pm.getColor(x, y);
            const uint8_t r = SkColorGetR(c);
            // Surface is cleared to white, so any non-white pixel
            // is "ink".  Use the alpha-channel check for opacity
            // and the red-channel inversion as a darkness signal.
            const uint8_t a = SkColorGetA(c);
            if (a > 0 && r < 255) {
                ++d.opaque_pixels;
            }
            d.darkness_sum += static_cast<uint64_t>(255 - r);
        }
    }
    return d;
}

bool count_within_tolerance(uint64_t actual, uint64_t expected,
                            double ratio) {
    if (expected == 0) return actual == 0;
    const double tol = static_cast<double>(expected) * ratio;
    const double delta = std::abs(static_cast<double>(actual) -
                                  static_cast<double>(expected));
    return delta <= tol;
}

// Dump bitmap to a PNG under $TMPDIR so a developer can eyeball
// a failure. Best-effort — silently skipped if the env doesn't
// give us a writable temp dir.
void dump_actual_png(const SkBitmap& bm, const std::string& label) {
    const char* tmpdir = std::getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    const std::string path =
        std::string(tmpdir) + "/pulp-font-3.4-actual-" + label + ".png";
    SkFILEWStream out(path.c_str());
    if (!out.isValid()) return;
    SkPixmap pm;
    if (!bm.peekPixels(&pm)) return;
    SkPngEncoder::Options opts;
    (void)SkPngEncoder::Encode(&out, pm, opts);
    std::fprintf(stderr,
                 "[font-3.4] actual render dumped to %s\n", path.c_str());
}

// Single shared render path. Returns a populated SkBitmap (caller
// keeps ownership). Surface is RGBA8 + sRGB to match the rest of
// the Pulp canvas tests, cleared to opaque white, painted with
// black text via SkiaCanvas at the requested family/size.
SkBitmap render_text_bitmap(const std::string& family, float size,
                            const std::string& text) {
    SkBitmap bm;
    SkImageInfo info = SkImageInfo::Make(kWidth, kHeight,
                                         kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    bm.allocPixels(info);
    bm.eraseColor(SK_ColorWHITE);

    SkCanvas sk_canvas(bm);
    SkiaCanvas canvas(&sk_canvas);
    canvas.set_fill_color(Color::rgba8(0, 0, 0));
    canvas.set_font(family, size);
    canvas.fill_text(text, 4.0f, kBaselineY);
    return bm;
}

// Compare a freshly-rendered digest against the committed
// constants. Width/height must match exactly; counts are checked
// with the documented 5% structural tolerance.
void expect_digest_matches(const Digest& actual, const Digest& expected,
                           const std::string& label, const SkBitmap& bm) {
    INFO("label="          << label
         << " W="           << actual.width  << "/" << expected.width
         << " H="           << actual.height << "/" << expected.height
         << " opaque="      << actual.opaque_pixels
         << "/expected~"    << expected.opaque_pixels
         << " darkness="    << actual.darkness_sum
         << "/expected~"    << expected.darkness_sum);

    const bool ok =
        actual.width == expected.width &&
        actual.height == expected.height &&
        count_within_tolerance(actual.opaque_pixels,
                               expected.opaque_pixels,
                               kCountToleranceRatio) &&
        count_within_tolerance(actual.darkness_sum,
                               expected.darkness_sum,
                               kCountToleranceRatio);

    if (!ok) dump_actual_png(bm, label);
    REQUIRE(ok);
}

// ── Committed expected digests ──────────────────────────────────
//
// These numbers were captured on CI hosts running the repo's
// pinned Skia. The 5% tolerance above keeps each host-class
// baseline stable across minor Skia point releases while avoiding
// false equivalence between CoreText-backed macOS rasterization
// and FreeType/fontconfig-backed Linux rasterization. If a future
// Skia bump shifts these by more than 5% on a reference host,
// regenerate the constants in the same PR as the bump and keep
// the LOC diff auditable.
//
// Reference host: macos-arm64 · Xcode 26.5 (17F42) · Skia chrome/m149.
// The 'Hello'/Inter digest was regenerated for the Xcode 26.4.1→26.5
// toolchain bump (CoreText/HarfBuzz AA shifted opaque-pixel coverage
// past the 5% band: 210→244, 30518→31403). CJK + Mono stayed within
// tolerance and were left untouched.
//
// To regenerate: build + run this test, copy the `actual=` values
// from the INFO line into the constants below, rebuild, rerun,
// expect green.

// Regenerated when the bundled faces reached the paint path, which is the
// same cause that moved the 'Hello'/Inter digest — that change regenerated
// the Latin golden and missed this one.
//
// Bundled Inter carries no CJK, so 日本語 now leaves the primary face and is
// served by the system fallback cascade. Previously the requested "Inter"
// resolved through CoreText to a substitute that covered CJK itself, so no
// fallback happened and the digest recorded the substitute's glyphs. The
// near-doubling (190→365 opaque, 24749→46850 darkness) is a denser fallback
// face, not extra ink on the same one.
//
// Verified by LOOKING at the dumped render, not by accepting the number: three
// well-formed glyphs on one baseline, no doubling, no ghosting, no .notdef
// tofu. That check matters because a ~2x ink jump is also what a run painted
// twice looks like, and this branch changed fallback-run baseline seating.
// Deterministic across repeated runs.
//
// THE AUTHORITY FOR THESE NUMBERS IS THE GATE VM, NOT A DEV MACHINE.
//
// The required macOS check runs on an ephemeral Tart VM from the
// `pulp-build-runner` golden image, whose Xcode is pinned in
// `.shipyard/vm-image.toml` precisely so these goldens are reproducible. A
// Mac Studio or laptop can be several Xcode point releases ahead of it, and
// this file has already recorded one toolchain bump moving a digest past the
// 5% band. So "captured on my machine" is NOT the same claim as "correct for
// CI", and the reference-host line below is the pin, not wherever a value
// happened to be measured.
//
// These CJK values were measured on macos-arm64 / Xcode 26.6 (17F113), which
// is AHEAD of the pinned gate toolchain. Treat them as provisional until a
// gate run confirms them; if it disagrees, harvest the real numbers from the
// `actual=` values the failure prints rather than guessing again, and do NOT
// respond by widening the tolerance — that would weaken a test that is
// currently doing its job.
//
// CJK fallback face selection is host-dependent in a way the Latin goldens are
// not, so a host whose cascade picks a different CJK face needs its own
// baseline.
//
// This constant used to sit OUTSIDE the `#if defined(__linux__)` split that
// `kHelloInter14` and `kHelloWorldMono12` live inside, so one macOS measurement
// silently governed Linux too. The bundled-faces change is macOS-only in effect
// — the paint path already bridged the bundled faces on platforms with no font
// DB of their own, which is why the Linux Hello digest never moved — so leaving
// them shared would have made the Linux expectation wrong the moment that host
// gained a CJK face. Split, with Linux holding the value it always had.
//
// Linux is believed to soft-skip today (its image carries fontconfig but no CJK
// font package, and the case completes far faster than a real render), so this
// arm is currently latent rather than exercised. It is NOT a claim that
// FreeType/fontconfig produces this number — it is the pre-existing value,
// preserved rather than extrapolated from a macOS capture.
#if defined(__linux__)
constexpr Digest kCjkInter14 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/190,
    /*darkness_sum=*/24749,
};
#else
constexpr Digest kCjkInter14 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/365,
    /*darkness_sum=*/46850,
};
#endif

// Hosted Linux x64 renders the bundled Latin fonts through
// FreeType/fontconfig, which produces stable but lighter AA than
// the macOS CoreText reference. Keep separate constants instead
// of widening the global tolerance and weakening the regression
// signal on every platform.
#if defined(__linux__)
constexpr Digest kHelloInter14 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/197,
    /*darkness_sum=*/23436,
};

constexpr Digest kHelloWorldMono12 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/332,
    /*darkness_sum=*/34617,
};
#else
constexpr Digest kHelloInter14 {
    /*width=*/128, /*height=*/32,
    // Regenerated 2026-08-02 when the bundled faces reached the SkParagraph
    // path. 244/31403 was NOT bundled Inter: the paragraph collection only
    // learned the bundled faces on platforms with no font DB of their own, so
    // macOS resolved "Inter" through CoreText to a substitute. Verified against
    // Chrome rendering this exact string at this exact size from this repo's
    // own Inter-Regular.ttf — ink width 32.00px vs Chrome's 32.50px (-1.5%),
    // where the old render was 31.00px (-4.6%), and the letterforms now match.
    // The Linux digest below never moved: FreeType always used the bundled
    // face, and macOS's opaque count converging on it is the corroboration.
    /*opaque_pixels=*/210,
    /*darkness_sum=*/30518,
};

constexpr Digest kHelloWorldMono12 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/355,
    /*darkness_sum=*/47560,
};
#endif

} // namespace

// ────────────────────────────────────────────────────────────────
// Goldens — three small text strings, three scenarios.
// ────────────────────────────────────────────────────────────────

TEST_CASE("font rendering golden: Inter 14px 'Hello' on raster",
          "[golden][skia][font]") {
    SkBitmap bm = render_text_bitmap("Inter", 14.0f, "Hello");
    SkPixmap pm;
    REQUIRE(bm.peekPixels(&pm));
    Digest d = digest_from_pixmap(pm);
    // Width / height MUST always be right; this catches the
    // "rendered into a zero-size surface" failure mode that the
    // pixel-count tolerance alone would not.
    REQUIRE(d.width  == kHelloInter14.width);
    REQUIRE(d.height == kHelloInter14.height);
    // Lower bound: SOMETHING painted. Without this the test
    // would pass on a totally-blank surface so long as the
    // expected count was also zero.
    REQUIRE(d.opaque_pixels > 0);
    REQUIRE(d.darkness_sum  > 0);
    expect_digest_matches(d, kHelloInter14, "hello-inter-14", bm);
}

TEST_CASE("font rendering golden: Inter 14px CJK 日本語 on raster",
          "[golden][skia][font][cjk]") {
    // CJK is the *cascade* case: Inter doesn't ship CJK glyphs,
    // so the request falls through to the platform font manager.
    // On macOS that lands on Hiragino/Apple SD; on Linux without
    // fontconfig it will paint nothing — we soft-skip in that
    // case so this test stays a useful guard on the platforms
    // that DO have a CJK fallback.
    //
    // Use a deterministic probe instead of the post-render
    // `opaque_pixels < 20` threshold. The threshold approach is unsound because
    // Skia's `fill_text` paints the `.notdef` glyph (tofu boxes) when no
    // CJK face is found — those tofu pixels easily exceed 20,
    // bypassing the soft-skip and producing spurious golden
    // mismatches. Ask the resolver directly: does the cascade
    // produce a face that covers U+65E5 (日)? If not, skip.
    {
        FontOptions probe_opts;
        probe_opts.family_stack.push_back("Inter");
        probe_opts.size = 14.0f;
        ResolvedFont primary =
            FontResolver::instance().resolve_family_list(probe_opts);
        ResolvedFont cjk = FontResolver::instance()
            .resolve_character_fallback(probe_opts, primary,
                                        /*U+65E5 日*/ 0x65E5);
        if (!cjk.has_typeface()) {
            SUCCEED("CJK fallback unavailable on this host — "
                    "golden skipped (resolver probe negative).");
            return;
        }
    }

    SkBitmap bm = render_text_bitmap("Inter", 14.0f, "日本語");
    SkPixmap pm;
    REQUIRE(bm.peekPixels(&pm));
    Digest d = digest_from_pixmap(pm);

    REQUIRE(d.width  == kCjkInter14.width);
    REQUIRE(d.height == kCjkInter14.height);

    expect_digest_matches(d, kCjkInter14, "cjk-inter-14", bm);
}

TEST_CASE("font rendering golden: JetBrains Mono 12px 'Hello world'",
          "[golden][skia][font][monospace]") {
    SkBitmap bm = render_text_bitmap("JetBrains Mono", 12.0f, "Hello world");
    SkPixmap pm;
    REQUIRE(bm.peekPixels(&pm));
    Digest d = digest_from_pixmap(pm);
    REQUIRE(d.width  == kHelloWorldMono12.width);
    REQUIRE(d.height == kHelloWorldMono12.height);
    REQUIRE(d.opaque_pixels > 0);
    REQUIRE(d.darkness_sum  > 0);
    expect_digest_matches(d, kHelloWorldMono12,
                          "helloworld-jetbrainsmono-12", bm);
}

// ────────────────────────────────────────────────────────────────
// Determinism probe — same string, twice, same process. Must
// produce bit-identical pixels. A failure here is a real
// non-determinism bug, NOT a golden-file rot signal.
// ────────────────────────────────────────────────────────────────

TEST_CASE("font rendering golden: render pipeline is deterministic in-process",
          "[golden][skia][font][determinism]") {
    SkBitmap a = render_text_bitmap("Inter", 14.0f, "Hello");
    SkBitmap b = render_text_bitmap("Inter", 14.0f, "Hello");

    SkPixmap pma, pmb;
    REQUIRE(a.peekPixels(&pma));
    REQUIRE(b.peekPixels(&pmb));
    REQUIRE(pma.width()  == pmb.width());
    REQUIRE(pma.height() == pmb.height());
    REQUIRE(pma.rowBytes() == pmb.rowBytes());

    // Byte-exact pixel comparison. If this fails, something in
    // the render pipeline is racy / lazily-initialized / depends
    // on global state. Treat that as a render-pipeline bug; do
    // not paper over by relaxing the golden tolerance, that is a
    // different bug class.
    const size_t total_bytes = pma.rowBytes() * pma.height();
    REQUIRE(std::memcmp(pma.addr(), pmb.addr(), total_bytes) == 0);
}

#else  // !PULP_HAS_SKIA

TEST_CASE("font rendering goldens require Skia",
          "[golden][skia][font]") {
    SUCCEED("Skia not compiled — golden harness needs SkSurfaces::Raster.");
}

#endif  // PULP_HAS_SKIA
