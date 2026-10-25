// CSS `-webkit-font-smoothing` from a browser capture to the painted text: the
// capture records it, the lowering marks `antialiased` text nodes, the
// materialized Label paints them unhinted, and the document asks for the
// surface preblend that matches.
#include <catch2/catch_test_macros.hpp>

#include "tools/import-design/browser_capture_ir.hpp"

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/canvas/skia_canvas.hpp>
#include <pulp/view/design_codegen.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/widgets.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"

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

pulp::view::Label* find_label(pulp::view::View& view, const std::string& text) {
    if (auto* label = dynamic_cast<pulp::view::Label*>(&view); label && label->text() == text)
        return label;
    for (std::size_t i = 0; i < view.child_count(); ++i)
        if (auto* child = view.child_at(i))
            if (auto* found = find_label(*child, text))
                return found;
    return nullptr;
}

// Ink (sum of 255 - red) of `text` drawn black on white through SkiaCanvas.
double drawn_ink(const std::string& text, FontSmoothing smoothing) {
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
    double ink = 0;
    for (int y = 0; y < pixels.height(); ++y)
        for (int x = 0; x < pixels.width(); ++x)
            ink += 255 - SkColorGetR(pixels.getColor(x, y));
    return ink;
}

} // namespace

TEST_CASE("unhinted text is never served the hinted glyphs of the same run",
          "[browser-capture][font-smoothing]") {
    // Each order shapes a string nothing else has, so the first draw of it
    // is the one a shared paragraph cache would hand to the second.
    const double hinted_first = drawn_ink("Quartz Hymn Vex", FontSmoothing::auto_smoothing);
    const double unhinted_second = drawn_ink("Quartz Hymn Vex", FontSmoothing::antialiased);
    const double unhinted_first = drawn_ink("Jumpy Fjord Wax", FontSmoothing::antialiased);
    const double hinted_second = drawn_ink("Jumpy Fjord Wax", FontSmoothing::auto_smoothing);
    REQUIRE(hinted_first > 0.0);
    REQUIRE(unhinted_first > 0.0);
    // Unhinted glyphs carry visibly less ink (CoreText smoothing is off).
    CHECK(unhinted_second < hinted_first * 0.95);
    CHECK(unhinted_first < hinted_second * 0.95);
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

TEST_CASE("antialiased text is painted unhinted on a chromium surface",
          "[browser-capture][font-smoothing]") {
    auto lowered = lower_smoothing_fixture();
    REQUIRE(lowered.design_ir.has_value());
    auto& ir = *lowered.design_ir;
    auto root = pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    REQUIRE(root);
    auto* smooth = find_label(*root, "Antialiased");
    auto* plain = find_label(*root, "Default");
    REQUIRE(smooth != nullptr);
    REQUIRE(plain != nullptr);
    CHECK(smooth->font_smoothing() == FontSmoothing::antialiased);
    CHECK(plain->font_smoothing() == FontSmoothing::auto_smoothing);
    CHECK(pulp::view::design_ir_text_raster_policy(ir) == TextRasterPolicy::chromium);

    // A document whose text is all `auto` keeps Skia's default preblend.
    find_text(ir.root, "Antialiased")->attributes.erase("font_smoothing");
    CHECK(pulp::view::design_ir_text_raster_policy(ir) == TextRasterPolicy::legacy);
}

TEST_CASE("a label's font smoothing is scoped to its own paint",
          "[browser-capture][font-smoothing]") {
    using pulp::canvas::DrawCommand;
    pulp::view::Label label("Antialiased");
    label.set_bounds({0, 0, 120, 20});
    label.set_font_smoothing(FontSmoothing::antialiased);
    pulp::canvas::RecordingCanvas canvas;
    label.paint(canvas);
    const auto& commands = canvas.commands();
    REQUIRE(commands.size() >= 3);
    // save, the smoothing, the text, restore: the smoothing ends with the scope.
    CHECK(commands.front().type == DrawCommand::Type::save);
    CHECK(commands[1].type == DrawCommand::Type::set_font_smoothing);
    CHECK(commands[1].f[0] == 1.0f);
    CHECK(commands.back().type == DrawCommand::Type::restore);
    CHECK(canvas.count(DrawCommand::Type::fill_text) == 1);

    // An auto label touches no smoothing state at all.
    pulp::view::Label plain("Default");
    plain.set_bounds({0, 0, 120, 20});
    pulp::canvas::RecordingCanvas plain_canvas;
    plain.paint(plain_canvas);
    CHECK(plain_canvas.count(DrawCommand::Type::set_font_smoothing) == 0);
}

TEST_CASE("smoothing set inside a save scope ends with it", "[browser-capture][font-smoothing]") {
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
}

TEST_CASE("an auto label painted after an antialiased one is still hinted",
          "[browser-capture][font-smoothing]") {
    // Two labels on one canvas, the antialiased one first; the auto one must
    // paint exactly as it does alone.
    const auto render = [](bool with_antialiased_first) {
        auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(400, 120));
        surface->getCanvas()->clear(SK_ColorWHITE);
        surface->getCanvas()->scale(2, 2);
        pulp::canvas::SkiaCanvas canvas(surface->getCanvas());
        if (with_antialiased_first) {
            pulp::view::Label smooth("Grave Tusk Fly");
            smooth.set_font_family("\"Helvetica Neue\", sans-serif");
            smooth.set_font_size(15.0f);
            smooth.set_bounds({0, 0, 200, 20});
            smooth.set_font_smoothing(FontSmoothing::antialiased);
            smooth.paint(canvas);
        }
        canvas.save();
        canvas.translate(0, 30);
        pulp::view::Label plain("Grave Tusk Fly");
        plain.set_font_family("\"Helvetica Neue\", sans-serif");
        plain.set_font_size(15.0f);
        plain.set_bounds({0, 0, 200, 20});
        plain.paint(canvas);
        canvas.restore();
        SkPixmap pixels;
        REQUIRE(surface->peekPixels(&pixels));
        std::vector<SkColor> lower; // the auto label's rows only
        for (int y = 60; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x)
                lower.push_back(pixels.getColor(x, y));
        return lower;
    };
    CHECK(render(true) == render(false));
}

TEST_CASE("auto text measures and draws exactly as with no smoothing set",
          "[browser-capture][font-smoothing]") {
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
}

TEST_CASE("generated ui.js carries captured font smoothing and the surface policy",
          "[browser-capture][font-smoothing]") {
    auto lowered = lower_smoothing_fixture();
    REQUIRE(lowered.design_ir.has_value());
    auto& ir = *lowered.design_ir;
    const auto count = [](const std::string& js, const std::string& needle) {
        std::size_t n = 0;
        for (auto at = js.find(needle); at != std::string::npos; at = js.find(needle, at + 1))
            ++n;
        return n;
    };
    const auto js = pulp::view::generate_pulp_js(ir);
    // Exactly the one antialiased node, and the document asks for chromium.
    CHECK(count(js, "setFontSmoothing(") == 1);
    CHECK(count(js, "', 'antialiased');") == 1);
    CHECK(count(js, "setTextRasterPolicy('chromium')") == 1);

    find_text(ir.root, "Antialiased")->attributes.erase("font_smoothing");
    const auto plain = pulp::view::generate_pulp_js(ir);
    CHECK(count(plain, "setFontSmoothing(") == 0);
    CHECK(count(plain, "setTextRasterPolicy(") == 0);
}
