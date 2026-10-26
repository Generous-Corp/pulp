// CSS `-webkit-font-smoothing` from a browser capture to the painted text: the
// capture records it, the lowering marks `antialiased` text nodes, SkiaCanvas
// draws them unhinted, and the document asks for the surface preblend that
// matches. The materialized Label's side lives with the widget tests
// (test_typography_inheritance.cpp, test_design_import_native_materializer.cpp)
// so this file stays off the view headers.
#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/view/design_import.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/paragraph_cache.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#endif

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace pulp::import_design;
using pulp::canvas::FontSmoothing;
using pulp::canvas::TextRasterPolicy;
using pulp::view::IRNode;
namespace fs = std::filesystem;

namespace {

BrowserCaptureIrResult lower_smoothing_fixture() {
    BrowserCaptureIrOptions options;
    options.native_panel_lowering = true;
    return lower_browser_capture_to_ir(fs::path(PULP_BROWSER_CAPTURE_FIXTURE_ROOT) /
                                           "browser-capture-font-smoothing" / "capture.json",
                                       options);
}

IRNode* find_text(IRNode& node, const std::string& text) {
    if (node.type == "text" && node.text_content == text)
        return &node;
    for (auto& child : node.children)
        if (auto* found = find_text(child, text))
            return found;
    return nullptr;
}

#ifdef PULP_HAS_SKIA
// `text` drawn black on white through SkiaCanvas: its pixels.
std::vector<SkColor> drawn(const std::string& text, FontSmoothing smoothing) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 80));
    surface->getCanvas()->clear(SK_ColorWHITE);
    surface->getCanvas()->scale(2, 2);
    pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
    canvas.set_font_full("Helvetica Neue", 15.0f, 400, 0, 0.0f);
    canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 255));
    canvas.set_font_smoothing(smoothing);
    canvas.fill_text(text, 10, 25);
    SkPixmap pixels;
    REQUIRE(surface->peekPixels(&pixels));
    std::vector<SkColor> all;
    for (int y = 0; y < pixels.height(); ++y)
        for (int x = 0; x < pixels.width(); ++x)
            all.push_back(pixels.getColor(x, y));
    return all;
}

// Ink (sum of 255 - red) of a render from drawn().
double ink_of(const std::vector<SkColor>& pixels) {
    double ink = 0;
    for (const SkColor c : pixels)
        ink += 255 - SkColorGetR(c);
    return ink;
}

double drawn_ink(const std::string& text, FontSmoothing smoothing) {
    return ink_of(drawn(text, smoothing));
}
#endif

} // namespace

TEST_CASE("unhinted text is never served the hinted glyphs of the same run",
          "[browser-capture][font-smoothing]") {
#ifndef PULP_HAS_SKIA
    SKIP("glyph hinting is a Skia canvas setting; this build has no Skia");
#else
    // The same run with and without glyph hinting is two paragraph cache
    // entries: drawing one never hands back the other's glyphs, in either
    // order, while drawing the same run again is served from the cache.
    auto& cache = pulp::canvas::ParagraphCache::instance();
    REQUIRE(cache.enabled());
    const auto check_order = [&](const std::string& text, FontSmoothing first,
                                 FontSmoothing second) {
        cache.clear();
        cache.reset_stats();
        const auto a = drawn(text, first);
        const auto b = drawn(text, second);
        CHECK(cache.stats().entries == 2);
        CHECK(a != b);
        const auto hits = cache.stats().hits;
        CHECK(drawn(text, first) == a);
        CHECK(cache.stats().hits > hits);
    };
    // Each order shapes a string nothing else has, so the first draw of it is
    // the one a shared cache entry would hand to the second.
    check_order("Quartz Hymn Vex", FontSmoothing::auto_smoothing, FontSmoothing::antialiased);
    check_order("Jumpy Fjord Wax", FontSmoothing::antialiased, FontSmoothing::auto_smoothing);
#if defined(__APPLE__)
    // On CoreText faces hinting thickens the glyphs, so unhinted text also
    // carries visibly less ink. FreeType's hinting does not, so this is not
    // asserted elsewhere.
    CHECK(drawn_ink("Brave Glyph Kin", FontSmoothing::antialiased) <
          drawn_ink("Brave Glyph Kin", FontSmoothing::auto_smoothing) * 0.95);
#endif
#endif
}

TEST_CASE("a captured antialiased text node is marked, and only that one",
          "[browser-capture][font-smoothing]") {
    auto lowered = lower_smoothing_fixture();
    REQUIRE(lowered.design_ir.has_value());
    auto& root = lowered.design_ir->root;
    const auto smoothing_of = [&](const std::string& text) -> std::string {
        auto* node = find_text(root, text);
        REQUIRE(node != nullptr);
        const auto it = node->attributes.find("font_smoothing");
        return it == node->attributes.end() ? std::string("(none)") : it->second;
    };
    CHECK(smoothing_of("Antialiased") == "antialiased");
    // `auto` and `subpixel-antialiased` rasterize alike on macOS.
    CHECK(smoothing_of("Default") == "(none)");
    CHECK(smoothing_of("Subpixel") == "(none)");
}

TEST_CASE("a document holding antialiased text asks for the chromium surface",
          "[browser-capture][font-smoothing]") {
    auto lowered = lower_smoothing_fixture();
    REQUIRE(lowered.design_ir.has_value());
    auto& ir = *lowered.design_ir;
    CHECK(pulp::view::design_ir_text_raster_policy(ir) == TextRasterPolicy::chromium);

    // A document whose text is all `auto` keeps Skia's default preblend.
    find_text(ir.root, "Antialiased")->attributes.erase("font_smoothing");
    CHECK(pulp::view::design_ir_text_raster_policy(ir) == TextRasterPolicy::legacy);
}

TEST_CASE("smoothing set inside a save scope ends with it", "[browser-capture][font-smoothing]") {
#ifndef PULP_HAS_SKIA
    SKIP("glyph hinting is a Skia canvas setting; this build has no Skia");
#else
    const auto ink_after_scope = [](const std::string& text) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 80));
        surface->getCanvas()->clear(SK_ColorWHITE);
        surface->getCanvas()->scale(2, 2);
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        canvas.set_font_full("Helvetica Neue", 15.0f, 400, 0, 0.0f);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 255));
        canvas.save();
        canvas.set_font_smoothing(FontSmoothing::antialiased);
        canvas.restore();
        canvas.fill_text(text, 10, 25);
        SkPixmap pixels;
        REQUIRE(surface->peekPixels(&pixels));
        double ink = 0;
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                ink += 255 - SkColorGetR(pixels.getColor(x, y));
        return ink;
    };
    CHECK(ink_after_scope("Brisk Lynx Oath") ==
          drawn_ink("Brisk Lynx Oath", FontSmoothing::auto_smoothing));
#endif
}

TEST_CASE("auto text measures and draws exactly as with no smoothing set",
          "[browser-capture][font-smoothing]") {
#ifndef PULP_HAS_SKIA
    SKIP("glyph hinting is a Skia canvas setting; this build has no Skia");
#else
    // Advances: setting auto, or scoping antialiased away, changes nothing.
    const auto advance = [](int mode) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(40, 40));
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        canvas.set_font_full("Helvetica Neue", 15.0f, 400, 0, 0.0f);
        if (mode == 1)
            canvas.set_font_smoothing(FontSmoothing::auto_smoothing);
        if (mode == 2) {
            canvas.save();
            canvas.set_font_smoothing(FontSmoothing::antialiased);
            canvas.restore();
        }
        return canvas.measure_text("Spectral Sweep 64");
    };
    CHECK(advance(1) == advance(0));
    CHECK(advance(2) == advance(0));
    // Baseline and glyphs: the same auto text drawn with smoothing never set
    // and with it explicitly auto is the same image.
    CHECK(drawn_ink("Calm Vow Jig", FontSmoothing::auto_smoothing) > 0.0);
    const auto draw = [](bool explicit_auto) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 80));
        surface->getCanvas()->clear(SK_ColorWHITE);
        surface->getCanvas()->scale(2, 2);
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        canvas.set_font_full("Helvetica Neue", 15.0f, 400, 0, 0.0f);
        canvas.set_fill_color(pulp::canvas::Color::rgba8(0, 0, 0, 255));
        if (explicit_auto)
            canvas.set_font_smoothing(FontSmoothing::auto_smoothing);
        canvas.fill_text("Oxen Plume Dwarf", 10, 25);
        SkPixmap pixels;
        REQUIRE(surface->peekPixels(&pixels));
        std::vector<SkColor> all;
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                all.push_back(pixels.getColor(x, y));
        return all;
    };
    CHECK(draw(true) == draw(false));
#endif
}
