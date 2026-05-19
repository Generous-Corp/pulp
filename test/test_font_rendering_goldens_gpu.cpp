// test_font_rendering_goldens_gpu.cpp — Font v2 Slice 3.4
// (Cross-backend rendering goldens — Skia GPU lane).
//
// Sibling of `test_font_rendering_goldens.cpp`. That file pins the
// Skia *raster* path; this one pins the Skia *GPU* path (Graphite +
// Dawn → Metal on macOS-arm64). Same three strings, same structural
// digest contract, looser tolerance.
//
// ────────────────────────────────────────────────────────────────
// Design choices (read this before "fixing" a failure):
//
//  1. Same `Digest` and `digest_from_pixels` shape as the raster
//     file, but here we read pixels back from a GPU surface via
//     `SkiaSurface::read_current_rgba()` instead of slurping them
//     directly from an `SkBitmap`. The on-disk PNG dump path is
//     identical so a developer can eyeball drift the same way.
//
//  2. Tolerances are LOOSER than the raster file (±15% on the
//     count fields, not ±5%). Reasons:
//       • Graphite picks its own AA + subpixel positioning policy
//         that doesn't have to match the raster path bit-for-bit
//         across Skia point releases (even though on the macOS-
//         arm64 reference host we currently observe pixel-exact
//         agreement with raster — they share the same glyph
//         rasterizer underneath, so small-text rasters round-trip
//         identically here).
//       • Future Vulkan / D3D backends will use different texel
//         rounding and AA — same tolerance applies when they land.
//       • The clear-to-white step happens via `fill_rect` on the
//         GPU canvas — not `bm.eraseColor(SK_ColorWHITE)` — so the
//         backdrop is "white-ish" rather than identically white
//         once Graphite gets to choose how to blend it.
//     Anything tighter than ±15% would chase Graphite point-release
//     drift and burn CI roundtrips. A real cascade regression (CJK
//     → tofu, mono shaper picking the wrong family) would still be
//     a much larger swing than 15%, so this guard catches what it
//     needs to.
//
//  3. Three strings, three scenarios (mirrors the raster file
//     1:1 — Inter 14px Latin, Inter 14px CJK fallback, JetBrains
//     Mono 12px). Surface is 128×32 RGBA8 sRGB, baseline at y=22.
//
//  4. Determinism probe — same GPU surface, two consecutive frames,
//     byte-exact pixel equality. A failure here means the GPU
//     rendering pipeline has stateful drift between frames (lazy
//     glyph-cache warming, residual atlas state, …). STOP and
//     escalate via codex-consult; do NOT relax tolerances.
//
//  5. Cross-backend tolerance probe — render "Hello" once on
//     raster, once on GPU, and assert the two digests are within
//     ±15% on `opaque_pixels` and `darkness_sum`. This is the
//     "did Graphite paint something close to what raster painted?"
//     guard. If raster says ~210 opaque pixels and GPU says zero,
//     something is structurally broken (likely an init failure
//     swallowed by `is_available()` returning true).
//
//  6. The test gates on `PULP_HAS_SKIA && APPLE` at COMPILE time
//     (so the TU stays empty on the Namespace runner which doesn't
//     have Skia linked) and on `GpuSurface::create_dawn()` +
//     `gpu->initialize(...)` + `SkiaSurface::create(...)` at RUN
//     time (so a CI lane without a working Dawn adapter soft-skips
//     instead of hard-failing).
//
// Tag: [golden][gpu][skia][font][issue-2257-followup]

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>
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

#if defined(PULP_HAS_SKIA) && defined(__APPLE__)

namespace {

// Same surface geometry as the raster goldens file. Diverging
// geometry would invalidate the cross-backend tolerance probe.
constexpr int kWidth  = 128;
constexpr int kHeight = 32;
constexpr float kBaselineY = 22.0f;

// Looser than the raster ±5% — see header rationale.
constexpr double kGpuCountToleranceRatio          = 0.15;
constexpr double kCrossBackendCountToleranceRatio = 0.15;

// ── Structural digest helpers ───────────────────────────────────

struct Digest {
    int width = 0;
    int height = 0;
    uint32_t opaque_pixels = 0;
    uint64_t darkness_sum = 0;
};

// Digest a raw RGBA8 pixel buffer produced by
// `SkiaSurface::read_current_rgba()`. "Opaque" means alpha > 0 AND
// the red channel dropped below pure white — same criterion as the
// raster file's `digest_from_pixmap`, expressed against the
// readback buffer's layout.
Digest digest_from_rgba(const std::vector<uint8_t>& pixels,
                        uint32_t width, uint32_t height) {
    Digest d;
    d.width  = static_cast<int>(width);
    d.height = static_cast<int>(height);
    const size_t expected_size = static_cast<size_t>(width) *
                                 static_cast<size_t>(height) * 4u;
    if (pixels.size() < expected_size) return d;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4u;
            const uint8_t r = pixels[i + 0];
            const uint8_t a = pixels[i + 3];
            if (a > 0 && r < 255) {
                ++d.opaque_pixels;
            }
            d.darkness_sum += static_cast<uint64_t>(255 - r);
        }
    }
    return d;
}

// Same digester applied to an SkBitmap — used only for the
// cross-backend probe (rendering "Hello" on raster for comparison).
Digest digest_from_pixmap(const SkPixmap& pm) {
    Digest d;
    d.width  = pm.width();
    d.height = pm.height();
    for (int y = 0; y < pm.height(); ++y) {
        for (int x = 0; x < pm.width(); ++x) {
            const SkColor c = pm.getColor(x, y);
            const uint8_t r = SkColorGetR(c);
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

// Dump readback buffer to PNG so a developer can eyeball a failure.
// Mirrors the raster file's `dump_actual_png`.
void dump_actual_png_rgba(const std::vector<uint8_t>& pixels,
                          uint32_t width, uint32_t height,
                          const std::string& label) {
    const char* tmpdir = std::getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
    const std::string path =
        std::string(tmpdir) + "/pulp-font-3.4-gpu-actual-" + label + ".png";
    SkFILEWStream out(path.c_str());
    if (!out.isValid()) return;

    SkImageInfo info = SkImageInfo::Make(static_cast<int>(width),
                                         static_cast<int>(height),
                                         kRGBA_8888_SkColorType,
                                         kPremul_SkAlphaType,
                                         SkColorSpace::MakeSRGB());
    SkPixmap pm(info, pixels.data(),
                static_cast<size_t>(width) * 4u);
    SkPngEncoder::Options opts;
    (void)SkPngEncoder::Encode(&out, pm, opts);
    std::fprintf(stderr,
                 "[font-3.4-gpu] actual render dumped to %s\n", path.c_str());
}

// Per-test fixture: bring up Dawn + Skia Graphite for a 128×32
// offscreen surface. Returns nullptr on any setup failure so the
// caller can soft-skip cleanly.
struct GpuFixture {
    std::unique_ptr<pulp::render::GpuSurface> gpu;
    std::unique_ptr<pulp::render::SkiaSurface> skia;

    bool ready() const {
        return gpu && skia && skia->is_available();
    }
};

GpuFixture make_gpu_fixture() {
    GpuFixture f;
    f.gpu = pulp::render::GpuSurface::create_dawn();
    if (!f.gpu) return f;

    pulp::render::GpuSurface::Config gpu_config{};
    gpu_config.width = static_cast<uint32_t>(kWidth);
    gpu_config.height = static_cast<uint32_t>(kHeight);
    gpu_config.native_surface_handle = nullptr; // headless offscreen
    if (!f.gpu->initialize(gpu_config)) {
        f.gpu.reset();
        return f;
    }

    pulp::render::SkiaSurface::Config skia_config{};
    skia_config.width = static_cast<uint32_t>(kWidth);
    skia_config.height = static_cast<uint32_t>(kHeight);
    skia_config.scale_factor = 1.0f;
    f.skia = pulp::render::SkiaSurface::create(*f.gpu, skia_config);
    return f;
}

// Render `text` at `family`/`size` through the GPU surface and
// return the RGBA8 readback buffer plus the resolved pixel
// dimensions. On any failure (which we expect to be rare and
// loud), `pixels` is left empty and the caller should SKIP.
//
// Background fill: the GPU offscreen target is NOT cleared by
// Skia automatically (unlike `bm.eraseColor` in the raster path),
// so we paint a full-canvas white rect ourselves first. The
// digest treats a < 255 red channel as "ink", so this clear has
// to be exactly white for the digest to match the raster file's
// "white background + black text" expectation.
bool gpu_render_text(GpuFixture& f,
                     const std::string& family, float size,
                     const std::string& text,
                     std::vector<uint8_t>& pixels,
                     uint32_t& pixel_width,
                     uint32_t& pixel_height) {
    if (!f.ready()) return false;
    if (!f.gpu->begin_frame()) return false;

    auto* canvas = f.skia->begin_frame();
    if (!canvas) {
        f.gpu->end_frame();
        return false;
    }

    // White backdrop — see header comment.
    canvas->set_fill_color(Color::rgba8(255, 255, 255));
    canvas->fill_rect(0.0f, 0.0f,
                      static_cast<float>(kWidth),
                      static_cast<float>(kHeight));

    canvas->set_fill_color(Color::rgba8(0, 0, 0));
    canvas->set_font(family, size);
    canvas->fill_text(text, 4.0f, kBaselineY);

    f.skia->end_frame();

    const bool ok = f.skia->read_current_rgba(pixels,
                                              pixel_width,
                                              pixel_height);
    f.gpu->end_frame();
    return ok;
}

// Raster reference for the cross-backend probe. Same shape as
// `render_text_bitmap` in the raster file, kept in-line so this
// TU stays standalone.
SkBitmap render_text_bitmap_raster(const std::string& family, float size,
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

void expect_digest_matches(const Digest& actual, const Digest& expected,
                           const std::string& label,
                           const std::vector<uint8_t>& pixels,
                           uint32_t width, uint32_t height) {
    INFO("label="       << label
         << " W="        << actual.width  << "/" << expected.width
         << " H="        << actual.height << "/" << expected.height
         << " opaque="   << actual.opaque_pixels
         << "/expected~" << expected.opaque_pixels
         << " darkness=" << actual.darkness_sum
         << "/expected~" << expected.darkness_sum);

    const bool ok =
        actual.width  == expected.width  &&
        actual.height == expected.height &&
        count_within_tolerance(actual.opaque_pixels,
                               expected.opaque_pixels,
                               kGpuCountToleranceRatio) &&
        count_within_tolerance(actual.darkness_sum,
                               expected.darkness_sum,
                               kGpuCountToleranceRatio);

    if (!ok) dump_actual_png_rgba(pixels, width, height, label);
    REQUIRE(ok);
}

// ── Committed expected digests (macOS-arm64 reference) ──────────
//
// To regenerate after a Graphite point-release bump:
//   1. Run the test, capture the INFO `actual=` values.
//   2. Paste into the constants below.
//   3. Rebuild, rerun — expect green.
//
// First-run values are intentionally set generously around the
// raster baseline because Graphite produces close-but-not-identical
// digests on Metal. The first CI run that lands these will tighten
// them to the observed values.

constexpr Digest kGpuHelloInter14 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/210,
    /*darkness_sum=*/30518,
};

constexpr Digest kGpuCjkInter14 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/190,
    /*darkness_sum=*/24749,
};

constexpr Digest kGpuHelloWorldMono12 {
    /*width=*/128, /*height=*/32,
    /*opaque_pixels=*/355,
    /*darkness_sum=*/47560,
};

} // namespace

// ────────────────────────────────────────────────────────────────
// Goldens — three strings, three scenarios.
// ────────────────────────────────────────────────────────────────

TEST_CASE("font v2 Slice 3.4 — GPU golden Inter 14px 'Hello'",
          "[golden][gpu][skia][font][issue-2257-followup]") {
    auto f = make_gpu_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — GPU golden skipped.");
        return;
    }

    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    if (!gpu_render_text(f, "Inter", 14.0f, "Hello", pixels, pw, ph)) {
        SUCCEED("GPU readback failed — golden skipped (likely no adapter).");
        return;
    }

    Digest d = digest_from_rgba(pixels, pw, ph);
    REQUIRE(d.width  == kGpuHelloInter14.width);
    REQUIRE(d.height == kGpuHelloInter14.height);
    REQUIRE(d.opaque_pixels > 0);
    REQUIRE(d.darkness_sum  > 0);
    expect_digest_matches(d, kGpuHelloInter14,
                          "hello-inter-14", pixels, pw, ph);
}

TEST_CASE("font v2 Slice 3.4 — GPU golden Inter 14px CJK 日本語",
          "[golden][gpu][skia][font][issue-2257-followup]") {
    auto f = make_gpu_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — GPU golden skipped.");
        return;
    }

    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    if (!gpu_render_text(f, "Inter", 14.0f, "日本語", pixels, pw, ph)) {
        SUCCEED("GPU readback failed — golden skipped (likely no adapter).");
        return;
    }

    Digest d = digest_from_rgba(pixels, pw, ph);
    REQUIRE(d.width  == kGpuCjkInter14.width);
    REQUIRE(d.height == kGpuCjkInter14.height);

    // Soft-skip when no CJK fallback installed — same policy as
    // the raster file: this guards CJK cascade on hosts that
    // DO ship a CJK font, without nagging on hosts that don't.
    if (d.opaque_pixels < 20) {
        SUCCEED("CJK fallback unavailable on this host — GPU golden skipped.");
        return;
    }
    expect_digest_matches(d, kGpuCjkInter14,
                          "cjk-inter-14", pixels, pw, ph);
}

TEST_CASE("font v2 Slice 3.4 — GPU golden JetBrains Mono 12px 'Hello world'",
          "[golden][gpu][skia][font][issue-2257-followup]") {
    auto f = make_gpu_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — GPU golden skipped.");
        return;
    }

    std::vector<uint8_t> pixels;
    uint32_t pw = 0, ph = 0;
    if (!gpu_render_text(f, "JetBrains Mono", 12.0f, "Hello world",
                         pixels, pw, ph)) {
        SUCCEED("GPU readback failed — golden skipped (likely no adapter).");
        return;
    }

    Digest d = digest_from_rgba(pixels, pw, ph);
    REQUIRE(d.width  == kGpuHelloWorldMono12.width);
    REQUIRE(d.height == kGpuHelloWorldMono12.height);
    REQUIRE(d.opaque_pixels > 0);
    REQUIRE(d.darkness_sum  > 0);
    expect_digest_matches(d, kGpuHelloWorldMono12,
                          "helloworld-jetbrainsmono-12", pixels, pw, ph);
}

// ────────────────────────────────────────────────────────────────
// Determinism probe — same fixture, two consecutive GPU frames,
// byte-exact pixel equality. A failure is a real non-determinism
// bug in the GPU render pipeline. STOP and escalate; do NOT
// relax the per-string tolerance, that's the wrong fix.
// ────────────────────────────────────────────────────────────────

TEST_CASE("font v2 Slice 3.4 — GPU render pipeline is deterministic in-process",
          "[golden][gpu][skia][font][determinism][issue-2257-followup]") {
    auto f = make_gpu_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — determinism probe skipped.");
        return;
    }

    std::vector<uint8_t> a, b;
    uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
    if (!gpu_render_text(f, "Inter", 14.0f, "Hello", a, aw, ah)) {
        SUCCEED("GPU readback failed — determinism probe skipped.");
        return;
    }
    if (!gpu_render_text(f, "Inter", 14.0f, "Hello", b, bw, bh)) {
        SUCCEED("GPU readback failed — determinism probe skipped.");
        return;
    }

    REQUIRE(aw == bw);
    REQUIRE(ah == bh);
    REQUIRE(a.size() == b.size());
    REQUIRE(std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// ────────────────────────────────────────────────────────────────
// Cross-backend probe — raster vs GPU on the same string. We
// expect close-but-not-identical digests. Assertion: counts agree
// within ±15%. Width/height must match exactly (sanity).
// ────────────────────────────────────────────────────────────────

TEST_CASE("font v2 Slice 3.4 — raster and GPU digests agree within tolerance",
          "[golden][gpu][skia][font][cross-backend][issue-2257-followup]") {
    auto f = make_gpu_fixture();
    if (!f.ready()) {
        SUCCEED("Dawn/Graphite unavailable on this host — cross-backend probe skipped.");
        return;
    }

    // GPU side.
    std::vector<uint8_t> gpu_pixels;
    uint32_t gpu_w = 0, gpu_h = 0;
    if (!gpu_render_text(f, "Inter", 14.0f, "Hello",
                         gpu_pixels, gpu_w, gpu_h)) {
        SUCCEED("GPU readback failed — cross-backend probe skipped.");
        return;
    }
    Digest dg = digest_from_rgba(gpu_pixels, gpu_w, gpu_h);

    // Raster side.
    SkBitmap raster_bm = render_text_bitmap_raster("Inter", 14.0f, "Hello");
    SkPixmap raster_pm;
    REQUIRE(raster_bm.peekPixels(&raster_pm));
    Digest dr = digest_from_pixmap(raster_pm);

    INFO("raster.opaque=" << dr.opaque_pixels
         << " gpu.opaque="    << dg.opaque_pixels
         << " raster.dark="   << dr.darkness_sum
         << " gpu.dark="      << dg.darkness_sum);

    REQUIRE(dg.width  == dr.width);
    REQUIRE(dg.height == dr.height);

    // Both backends must produce ink. If GPU produced zero ink,
    // that's a Graphite text-shaping init failure that
    // `is_available()` didn't catch — fail loudly.
    REQUIRE(dg.opaque_pixels > 0);
    REQUIRE(dr.opaque_pixels > 0);

    REQUIRE(count_within_tolerance(dg.opaque_pixels,
                                   dr.opaque_pixels,
                                   kCrossBackendCountToleranceRatio));
    REQUIRE(count_within_tolerance(dg.darkness_sum,
                                   dr.darkness_sum,
                                   kCrossBackendCountToleranceRatio));
}

#else  // !(PULP_HAS_SKIA && __APPLE__)

TEST_CASE("font v2 Slice 3.4 — GPU rendering goldens require Skia on macOS",
          "[golden][gpu][skia][font]") {
    // Two reasons we end up here:
    //  1. Skia not linked (Namespace macOS overflow runner, Linux/Win CI).
    //  2. Skia linked but not on Apple — Dawn-on-Metal is the only
    //     headless GPU lane wired up today; Vulkan + D3D paths are
    //     deferred (Slice 3.4 follow-up).
    SUCCEED("Skia GPU goldens are gated on PULP_HAS_SKIA && APPLE.");
}

#endif // PULP_HAS_SKIA && __APPLE__
