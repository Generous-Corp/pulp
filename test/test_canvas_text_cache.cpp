// Canvas text-path caching: correctness + a measurable workload.
//
// `SkiaCanvas::fill_text` historically built a fresh SkFont AND a fresh
// SkParagraph for every single text draw. A UI that repaints continuously
// (an analyzer canvas driven by requestAnimationFrame) redraws the same
// immutable ruler and axis labels every frame, so that construction cost
// recurs at frame rate while the shaped result never changes.
//
// The tests below pin the behavior any cache in that path must preserve:
// identical inputs render identically, and every input that can change the
// rendered pixels — colour, size, weight, slant, letter-spacing, family, and
// the font-registry generation — must still change them. A cache that gets
// any of those wrong renders stale text, which is worse than a slow one.
//
// `[.perf]` is a Catch2 hidden tag: the timing case is excluded from a
// default run and executed explicitly when measuring.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/canvas.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef PULP_HAS_SKIA

#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/canvas/bundled_fonts.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

using pulp::canvas::Color;
using pulp::canvas::SkiaCanvas;

namespace {

constexpr int kW = 320;
constexpr int kH = 64;

struct RasterTarget {
    sk_sp<SkSurface> surface;
    SkCanvas* sk_canvas = nullptr;

    RasterTarget(int w = kW, int h = kH) {
        SkImageInfo info = SkImageInfo::Make(w, h, kN32_SkColorType,
                                             kPremul_SkAlphaType,
                                             SkColorSpace::MakeSRGB());
        surface = SkSurfaces::Raster(info);
        if (surface) sk_canvas = surface->getCanvas();
    }

    void clear() { sk_canvas->clear(SK_ColorBLACK); }

    // Ink-weighted signature. A whole-surface mean is a poor comparator on a
    // dark UI (mostly background either way), so sum the non-background
    // luminance and count the inked pixels instead: that responds to glyph
    // shape and colour rather than to the background that dominates the frame.
    struct Ink {
        long long luma = 0;
        int pixels = 0;
        bool operator==(const Ink& o) const {
            return luma == o.luma && pixels == o.pixels;
        }
        bool operator!=(const Ink& o) const { return !(*this == o); }
    };

    Ink ink() const {
        SkPixmap pm;
        Ink out;
        if (!surface->peekPixels(&pm)) return out;
        for (int y = 0; y < pm.height(); ++y) {
            for (int x = 0; x < pm.width(); ++x) {
                SkColor c = pm.getColor(x, y);
                const int r = SkColorGetR(c), g = SkColorGetG(c), b = SkColorGetB(c);
                const int l = r + g + b;
                if (l > 12) {           // above the cleared-black floor
                    out.luma += l;
                    out.pixels += 1;
                }
            }
        }
        return out;
    }
};

// Draw one label with an explicit style, return its ink signature.
RasterTarget::Ink draw_once(const std::string& text,
                            const std::string& family,
                            float size,
                            int weight = 400,
                            int slant = 0,
                            float letter_spacing = 0.0f,
                            Color color = Color::rgba(1.0f, 1.0f, 1.0f, 1.0f)) {
    RasterTarget t;
    if (!t.sk_canvas) return {};
    t.clear();
    SkiaCanvas canvas(t.sk_canvas);
    canvas.set_font_full(family, size, weight, slant, letter_spacing);
    canvas.set_fill_color(color);
    canvas.fill_text(text, 4.0f, static_cast<float>(kH) * 0.7f);
    return t.ink();
}

}  // namespace

// ── Correctness: a cache must be transparent ────────────────────────────────

TEST_CASE("Repeated identical text draws render identically",
          "[canvas][text][cache]") {
    const auto a = draw_once("100Hz", "Inter", 13.0f);
    const auto b = draw_once("100Hz", "Inter", 13.0f);
    const auto c = draw_once("100Hz", "Inter", 13.0f);
    REQUIRE(a.pixels > 0);          // proves the probe sees ink at all
    REQUIRE(a == b);
    REQUIRE(b == c);
}

TEST_CASE("Text colour is not shared between draws of the same string",
          "[canvas][text][cache]") {
    // The paragraph bakes the fill paint into its TextStyle, so a cache keyed
    // only on the string would render the second draw in the first's colour.
    const auto white = draw_once("RES 61/64", "Inter", 13.0f, 400, 0, 0.0f,
                                 Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));
    const auto dim = draw_once("RES 61/64", "Inter", 13.0f, 400, 0, 0.0f,
                               Color::rgba(0.25f, 0.25f, 0.25f, 1.0f));
    REQUIRE(white.pixels > 0);
    REQUIRE(dim.pixels > 0);
    REQUIRE(white.luma > dim.luma);
}

TEST_CASE("Text size participates in the cache key",
          "[canvas][text][cache]") {
    const auto small = draw_once("-12", "Inter", 10.0f);
    const auto large = draw_once("-12", "Inter", 22.0f);
    REQUIRE(small.pixels > 0);
    REQUIRE(large.pixels > small.pixels);
}

TEST_CASE("Text weight participates in the cache key",
          "[canvas][text][cache]") {
    const auto regular = draw_once("1kHz", "Inter", 16.0f, 400);
    const auto bold = draw_once("1kHz", "Inter", 16.0f, 700);
    REQUIRE(regular.pixels > 0);
    REQUIRE(bold.pixels > 0);
    REQUIRE(regular != bold);
}

TEST_CASE("Letter spacing participates in the cache key",
          "[canvas][text][cache]") {
    const auto tight = draw_once("10kHz", "Inter", 14.0f, 400, 0, 0.0f);
    const auto loose = draw_once("10kHz", "Inter", 14.0f, 400, 0, 3.0f);
    REQUIRE(tight.pixels > 0);
    REQUIRE(loose.pixels > 0);
    REQUIRE(tight != loose);
}

TEST_CASE("Distinct strings do not collide in the cache",
          "[canvas][text][cache]") {
    const auto a = draw_once("100Hz", "Inter", 14.0f);
    const auto b = draw_once("10kHz", "Inter", 14.0f);
    REQUIRE(a.pixels > 0);
    REQUIRE(b.pixels > 0);
    REQUIRE(a != b);
}

TEST_CASE("A cached glyph run still moves when the draw position moves",
          "[canvas][text][cache]") {
    // A cached paragraph is reused across draws that differ only in position.
    // SkParagraph populates a text-blob cache lazily inside paint(); if any of
    // that were baked at absolute coordinates, the second draw would land on
    // top of the first. Assert the ink actually translates with x.
    auto ink_min_x = [](float x) {
        RasterTarget t;
        t.clear();
        SkiaCanvas canvas(t.sk_canvas);
        canvas.set_font_full("Inter", 16.0f, 400, 0, 0.0f);
        canvas.set_fill_color(Color::rgba(1.0f, 1.0f, 1.0f, 1.0f));
        canvas.fill_text("ABC", x, static_cast<float>(kH) * 0.7f);
        SkPixmap pm;
        int min_x = kW;
        if (!t.surface->peekPixels(&pm)) return min_x;
        for (int py = 0; py < pm.height(); ++py) {
            for (int px = 0; px < pm.width(); ++px) {
                SkColor c = pm.getColor(px, py);
                if (SkColorGetR(c) + SkColorGetG(c) + SkColorGetB(c) > 12) {
                    if (px < min_x) min_x = px;
                }
            }
        }
        return min_x;
    };

    const int near = ink_min_x(4.0f);
    const int far = ink_min_x(60.0f);
    REQUIRE(near < kW);              // ink was found at all
    REQUIRE(far < kW);
    REQUIRE(far > near + 40);        // moved with the requested offset
}

TEST_CASE("Registering a font invalidates cached text for that family",
          "[canvas][text][cache]") {
    // The caches key on merged_generation_for(), the documented eviction
    // signal. Without it, the first draw of a not-yet-registered family would
    // pin its fallback typeface for the life of the process, and an async
    // webfont arriving later would never appear.
    const std::string family = "PulpCanvasTextCacheProbe";

    const auto before = draw_once("Handgloves", family, 18.0f);

    const bool registered =
        pulp::canvas::register_font_file(PULP_TEST_FONT_PATH, family);
    if (!registered) {
        SUCCEED("register_font_file returned false — no usable font backend "
                "in this configuration; invalidation is untestable here");
        return;
    }

    const auto after = draw_once("Handgloves", family, 18.0f);

    REQUIRE(before.pixels > 0);
    REQUIRE(after.pixels > 0);
    // The family now resolves to Inter rather than the platform fallback, so
    // the rendered run must differ. If it does not, a stale cache entry was
    // served.
    REQUIRE(before != after);
}

TEST_CASE("Measured advance is stable across repeated measurement",
          "[canvas][text][cache]") {
    const auto first = SkiaCanvas::measure_text_with_font("Inter", 14.0f, "100Hz");
    const auto second = SkiaCanvas::measure_text_with_font("Inter", 14.0f, "100Hz");
    REQUIRE(first.width > 0.0f);
    REQUIRE(first.width == second.width);
}

// ── Workload: the cost this change targets ──────────────────────────────────

namespace {

// One analyzer frame: immutable ruler/axis labels redrawn every frame, plus a
// readout that changes each frame so the measurement is not a pure best case.
double run_label_workload(const std::string& family, float size, int frames) {
    const std::vector<std::string> stat = {
        "-6", "-12", "-18", "-24", "-30", "-36",
        "100Hz", "1kHz", "10kHz", "RES 61/64", "MID / SIDE", "CROSSOVER",
    };
    RasterTarget t(990, 645);
    if (!t.sk_canvas) return 0.0;

    // Cleared once, outside the timed region. Clearing a 990x645 surface per
    // frame costs more than the text drawn onto it (a profile of this loop put
    // neon::rect_memset32 at ~47% of samples), which would swamp the very cost
    // this case exists to measure. A real frame does clear; this benchmark is
    // deliberately isolating the text path, not simulating a whole frame.
    t.clear();
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        SkiaCanvas canvas(t.sk_canvas);
        canvas.set_font_full(family, size, 400, 0, 0.0f);
        canvas.set_fill_color(Color::rgba(0.8f, 0.8f, 0.85f, 1.0f));
        float y = 20.0f;
        for (const auto& s : stat) {
            canvas.fill_text(s, 8.0f, y);
            y += 18.0f;
        }
        canvas.fill_text("BAND " + std::to_string(f), 8.0f, y);
    }
    const auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

constexpr int kPerfCallsPerFrame = 13;   // 12 static labels + 1 changing readout

// Frame count is env-tunable so the same case can be stretched long enough to
// attach a sampling profiler to it.
int perf_frames() {
    static const int n = [] {
        if (const char* v = std::getenv("PULP_TEXT_PERF_FRAMES")) {
            const int parsed = std::atoi(v);
            if (parsed > 0) return parsed;
        }
        return 200;
    }();
    return n;
}
const int kPerfFrames = perf_frames();

void report(const char* label, double ms) {
    const int calls = kPerfFrames * kPerfCallsPerFrame;
    std::printf("\n[text-perf] %-28s %6.1f ms total  %.3f ms/frame  %5.1f us/call\n",
                label, ms, ms / kPerfFrames, (ms * 1000.0) / calls);
    std::fflush(stdout);
}

}  // namespace

// A CSS family LIST is the realistic case and the expensive one: resolving
// "JetBrains Mono, monospace" splits the list, builds a FontOptions per entry,
// and does a family-name comparison per candidate — none of which the
// single-family fast path pays. Spectr's analyzer canvas sets exactly
// `ctx.font = '9px JetBrains Mono, monospace'`, so this is the shape of the
// real workload; the single-family case is kept alongside it to show that the
// two are not interchangeable for measurement purposes.
TEST_CASE("Repeated label redraw throughput family list",
          "[.perf][canvas][text]") {
    report("family-list", run_label_workload("JetBrains Mono, monospace",
                                             9.0f, kPerfFrames));
    SUCCEED();
}

TEST_CASE("Repeated label redraw throughput single family",
          "[.perf][canvas][text]") {
    report("single-family", run_label_workload("Inter", 13.0f, kPerfFrames));
    SUCCEED();
}

#endif  // PULP_HAS_SKIA
