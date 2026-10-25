// test_coretext_text.cpp: the CoreText face registry the CoreGraphics canvas
// paints with, and TextShaper measuring the same faces.
//
// A label is laid out from TextShaper's metrics and painted by the canvas. On
// Apple the CoreGraphics canvas paints through CoreText; if it resolves a
// different face than the one measured, every label sits at the wrong
// baseline. Before the registry, "Inter" (Pulp's default UI family, bundled,
// not installed) measured as Inter under Skia but painted as CoreText's
// Helvetica substitute, and without Skia it was not measured at all (a
// 0.85 em rule of thumb), putting labels 2-4 pt higher than in a Skia build.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#ifdef __APPLE__

#include <pulp/canvas/bundled_fonts.hpp>
#include <pulp/canvas/cg_canvas.hpp>
#include <pulp/canvas/text_shaper.hpp>

#include "coretext_text.hpp"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <string>

using Catch::Approx;
using namespace pulp::canvas;

namespace {

std::string family_of(const void* font) {
    auto ct = static_cast<CTFontRef>(font);
    std::string out;
    if (CFStringRef name = CTFontCopyFamilyName(ct)) {
        char buf[256] = {};
        CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
        out = buf;
        CFRelease(name);
    }
    return out;
}

struct BitmapCanvas {
    CGContextRef ctx = nullptr;
    BitmapCanvas() {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        ctx = CGBitmapContextCreate(nullptr, 64, 64, 8, 0, cs,
                                    static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast));
        CGColorSpaceRelease(cs);
    }
    ~BitmapCanvas() {
        CGContextRelease(ctx);
    }
};

} // namespace

TEST_CASE("CoreText registry resolves the bundled families, not a substitute",
          "[canvas][text][coretext]") {
    for (const char* family : {"Inter", "JetBrains Mono", "Jost"}) {
        const void* font = detail::coretext_create_font(family, 12.0f, 400, 0);
        REQUIRE(font != nullptr);
        CHECK(family_of(font) == family);
        CFRelease(static_cast<CTFontRef>(font));
        CHECK(detail::coretext_has_registered_family(family));
    }
    // A family stack walks past an unknown first entry.
    const void* font = detail::coretext_create_font("'No Such Face', Inter", 12.0f, 400, 0);
    CHECK(family_of(font) == "Inter");
    CFRelease(static_cast<CTFontRef>(font));
}

TEST_CASE("CoreText registry picks the closest bundled weight", "[canvas][text][coretext]") {
    const void* regular = detail::coretext_create_font("Jost", 20.0f, 400, 0);
    const void* bold = detail::coretext_create_font("Jost", 20.0f, 700, 0);
    CHECK(detail::coretext_advance("Weight", "Jost", 20.0f, 700, 0) >
          detail::coretext_advance("Weight", "Jost", 20.0f, 400, 0));
    CFRelease(static_cast<CTFontRef>(regular));
    CFRelease(static_cast<CTFontRef>(bold));
}

TEST_CASE("CoreGraphics canvas measures text with the face TextShaper measures",
          "[canvas][text][coretext]") {
    BitmapCanvas bitmap;
    REQUIRE(bitmap.ctx != nullptr);
    CoreGraphicsCanvas canvas(bitmap.ctx, 64.0f, 64.0f);
    canvas.set_font_full("Inter", 14.0f, 400, 0, 0.0f);
    const float painted = canvas.measure_text("Input Gain");

    TextShaper shaper;
    const auto prepared = shaper.prepare("Input Gain", "Inter", 14.0f, 400, 0);
    REQUIRE(prepared.metrics_are_real());
    // Skia and CoreText lay the same face out to within subpixel rounding;
    // without Skia, TextShaper measures through CoreText itself.
    CHECK(prepared.total_width() == Approx(painted).margin(0.5));
    CHECK(painted == Approx(detail::coretext_advance("Input Gain", "Inter", 14.0f, 400, 0)));
}

TEST_CASE("CoreGraphics canvas applies OpenType features it is given", "[canvas][text][coretext]") {
    BitmapCanvas bitmap;
    REQUIRE(bitmap.ctx != nullptr);
    CoreGraphicsCanvas canvas(bitmap.ctx, 64.0f, 64.0f);
    canvas.set_font_full("Inter", 24.0f, 400, 0, 0.0f);
    // Inter's default figures are proportional; tabular figures make every
    // digit the same advance.
    const float ones = canvas.measure_text("111111");
    const float nines = canvas.measure_text("999999");
    CHECK(nines - ones > 5.0f);
    canvas.set_font_features({{Canvas::make_font_feature_tag("tnum"), 1}});
    CHECK(canvas.measure_text("111111") == Approx(canvas.measure_text("999999")).margin(0.5));
    canvas.clear_font_features();
    CHECK(canvas.measure_text("111111") == Approx(ones));
}

TEST_CASE("TextShaper face metrics match the CoreText face the canvas paints",
          "[canvas][text][coretext]") {
    // Skia's CoreText backend defines the worst-case extents from the font
    // bounding box and the typographic metrics from the font; the CoreText
    // path uses the same definitions, so both builds agree on the baseline.
    const auto m = detail::coretext_face_metrics("Inter", 12.0f, 400, 0);
    REQUIRE(m.ascent > 0.0f);
    TextShaper shaper;
    const auto prepared = shaper.prepare(" ", "Inter", 12.0f, 400, 0);
    REQUIRE(prepared.metrics_are_real());
    CHECK(prepared.ascent() == Approx(std::max(m.top, m.ascent)).margin(0.01));
    CHECK(prepared.descent() == Approx(std::max(m.bottom, m.descent)).margin(0.01));
    // Inter's real ascent is about 1.108 em, nothing like the 0.85 em guess.
    CHECK(prepared.ascent() > 12.0f);
}

TEST_CASE("register_font reaches the CoreText registry under its family",
          "[canvas][text][coretext]") {
    CHECK_FALSE(detail::coretext_has_registered_family("Pulp CoreText Alias"));
    // Register the bundled Inter file again under a new family name through
    // the public API; the canvas must then resolve that name to Inter.
    const auto before = font_registration_generation();
    REQUIRE(register_font_file(PULP_TEST_INTER_TTF, "Pulp CoreText Alias"));
    CHECK(font_registration_generation() > before);
    CHECK(is_font_registered("Pulp CoreText Alias"));
    const void* font = detail::coretext_create_font("Pulp CoreText Alias", 12.0f, 400, 0);
    CHECK(family_of(font) == "Inter");
    CFRelease(static_cast<CTFontRef>(font));
}

#else

TEST_CASE("CoreText registry is Apple-only", "[canvas][text][coretext]") {
    SUCCEED("not an Apple platform");
}

#endif
