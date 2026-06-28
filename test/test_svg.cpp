#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/svg.hpp>
#include <pulp/canvas/canvas.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

using namespace pulp::canvas;

static const char* test_svg = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <circle cx="50" cy="50" r="40" fill="#ff0000" stroke="#000000" stroke-width="2"/>
  <rect x="10" y="10" width="30" height="30" fill="#00ff00"/>
</svg>
)";

static std::filesystem::path make_temp_svg_path(const char* stem) {
    const auto tick = std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count();
    return std::filesystem::temp_directory_path() /
           (std::string(stem) + "-" + std::to_string(tick) + ".svg");
}

TEST_CASE("SvgImage from string", "[canvas][svg]") {
    auto img = SvgImage::from_string(test_svg);
    REQUIRE(img.is_valid());
    REQUIRE(img.width() > 0);
    REQUIRE(img.height() > 0);
}

TEST_CASE("SvgImage rasterize", "[canvas][svg]") {
    auto img = SvgImage::from_string(test_svg);
    REQUIRE(img.is_valid());

    auto pixels = img.rasterize(64, 64);
    REQUIRE(pixels.size() == 64 * 64 * 4); // RGBA

    // Check that some pixels are non-zero (not all transparent)
    bool has_content = false;
    for (size_t i = 3; i < pixels.size(); i += 4) {
        if (pixels[i] > 0) { has_content = true; break; }
    }
    REQUIRE(has_content);
}

TEST_CASE("SvgImage from file", "[canvas][svg]") {
    const auto path = make_temp_svg_path("pulp-test-svg-from-file");
    {
        std::ofstream file(path);
        REQUIRE(file.is_open());
        file << test_svg;
        REQUIRE(file.good());
    }

    auto img = SvgImage::from_file(path.string());
    REQUIRE(img.is_valid());
    REQUIRE(img.width() > 0);
    REQUIRE(img.height() > 0);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST_CASE("SvgImage missing file is invalid", "[canvas][svg]") {
    const auto path = make_temp_svg_path("pulp-test-svg-missing");

    std::error_code ec;
    std::filesystem::remove(path, ec);

    auto img = SvgImage::from_file(path.string());
    REQUIRE_FALSE(img.is_valid());
    REQUIRE(img.width() == 0);
    REQUIRE(img.height() == 0);
}

TEST_CASE("SvgImage rasterize guards invalid image and dimensions", "[canvas][svg]") {
    SvgImage invalid;
    REQUIRE_FALSE(invalid.is_valid());
    REQUIRE(invalid.rasterize(16, 16).empty());
    REQUIRE(invalid.rasterize(0, 16).empty());
    REQUIRE(invalid.rasterize(16, 0).empty());
    REQUIRE(invalid.rasterize(-1, 16).empty());
    REQUIRE(invalid.rasterize(16, -1).empty());

    auto img = SvgImage::from_string(test_svg);
    REQUIRE(img.is_valid());
    REQUIRE(img.rasterize(0, 16).empty());
    REQUIRE(img.rasterize(16, 0).empty());
    REQUIRE(img.rasterize(-1, 16).empty());
    REQUIRE(img.rasterize(16, -1).empty());
}

TEST_CASE("SvgImage render to RecordingCanvas", "[canvas][svg]") {
    auto img = SvgImage::from_string(test_svg);
    REQUIRE(img.is_valid());

    RecordingCanvas canvas;
    img.render(canvas, 0, 0, 100, 100);

    // Should have generated some drawing commands
    REQUIRE(canvas.command_count() > 0);
    REQUIRE(canvas.count(DrawCommand::Type::save) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::restore) >= 1);
}

TEST_CASE("SvgImage invalid render emits no commands", "[canvas][svg]") {
    SvgImage img;
    RecordingCanvas canvas;

    img.render(canvas, 10, 20, 30, 40);

    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("SvgImage invalid string", "[canvas][svg]") {
    auto img = SvgImage::from_string("not svg");

    REQUIRE(img.width() == 0);
    REQUIRE(img.height() == 0);
    REQUIRE(img.rasterize(16, 16).empty());

    RecordingCanvas canvas;
    img.render(canvas, 0, 0, 16, 16);
    REQUIRE(canvas.command_count() == 0);
}

TEST_CASE("SvgImage move semantics", "[canvas][svg]") {
    auto img1 = SvgImage::from_string(test_svg);
    REQUIRE(img1.is_valid());

    SvgImage img2 = std::move(img1);
    REQUIRE(img2.is_valid());
    REQUIRE_FALSE(img1.is_valid());
}

// pulp #72 — preset previews (Spectr's band-shape thumbnails were the
// live-traffic example) render through SvgImage::render() into a Canvas,
// not through nsvgRasterize() directly. The previous implementation:
//   (a) only emitted `stroke_line` commands — fills were dropped silently;
//   (b) treated each Bezier control point as a line endpoint, producing
//       jagged polylines connecting handles instead of smooth curves.
// A path with `fill="#abc"` and no stroke would render NOTHING — which
// is what the user observed as "preset preview blank."
//
// These regression tests enforce both contracts at the RecordingCanvas
// level so a refactor that drops the path-API calls fails CI before the
// user sees blank thumbnails again.
TEST_CASE("SvgImage::render emits fill_current_path for fill-only path [issue-72]",
          "[canvas][svg][issue-72]") {
    // Filled rect with NO stroke — the case that previously rendered nothing.
    auto img = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 10 10" width="10" height="10">
  <rect x="0" y="0" width="10" height="10" fill="#ff0000"/>
</svg>
)");
    REQUIRE(img.is_valid());

    RecordingCanvas canvas;
    img.render(canvas, 0, 0, 100, 100);

    REQUIRE(canvas.count(DrawCommand::Type::fill_current_path) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_current_path) == 0);
    // Pre-fix bug: emitted stroke_line commands instead of building a path.
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("SvgImage::render emits cubic_to for curved paths [issue-72]",
          "[canvas][svg][issue-72]") {
    // A cubic Bezier curve — the band-shape preview pattern. Pre-fix this
    // would have stepped through control points as straight-line endpoints.
    auto img = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <path d="M 10 50 C 30 10, 70 90, 90 50" stroke="#000" stroke-width="2" fill="none"/>
</svg>
)");
    REQUIRE(img.is_valid());

    RecordingCanvas canvas;
    img.render(canvas, 0, 0, 100, 100);

    REQUIRE(canvas.count(DrawCommand::Type::move_to) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::cubic_to) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_current_path) >= 1);
    // Stroke path rendering must not degrade to stroke_line commands.
    REQUIRE(canvas.count(DrawCommand::Type::stroke_line) == 0);
}

TEST_CASE("SvgImage::render forwards evenodd fill-rule [issue-72]",
          "[canvas][svg][issue-72]") {
    // Paths with `fill-rule="evenodd"` are common in preset-thumbnail
    // SVGs that use cutouts/holes. The fill
    // rule must reach Canvas's path API, not be hardcoded to nonzero.
    auto evenodd = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <path d="M 10 10 L 90 10 L 90 90 L 10 90 Z M 30 30 L 70 30 L 70 70 L 30 70 Z"
        fill="#000" fill-rule="evenodd"/>
</svg>
)");
    REQUIRE(evenodd.is_valid());
    RecordingCanvas eo_canvas;
    evenodd.render(eo_canvas, 0, 0, 100, 100);
    REQUIRE(eo_canvas.count(DrawCommand::Type::fill_current_path) >= 1);
    // RecordingCanvas stores the FillRule in cmd.f[0] (1.0 = evenodd,
    // 0.0 = nonzero). See test_widget_bridge.cpp `ctx.fill('evenodd')`
    // case for the same encoding.
    bool saw_evenodd = false;
    for (const auto& c : eo_canvas.commands()) {
        if (c.type == DrawCommand::Type::fill_current_path && c.f[0] == 1.0f) {
            saw_evenodd = true;
            break;
        }
    }
    REQUIRE(saw_evenodd);

    // Default rule (nonzero) should still pass through as nonzero.
    auto nonzero = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <path d="M 10 10 L 90 10 L 90 90 L 10 90 Z" fill="#f00"/>
</svg>
)");
    REQUIRE(nonzero.is_valid());
    RecordingCanvas nz_canvas;
    nonzero.render(nz_canvas, 0, 0, 100, 100);
    REQUIRE(nz_canvas.count(DrawCommand::Type::fill_current_path) >= 1);
    bool saw_nonzero = false;
    for (const auto& c : nz_canvas.commands()) {
        if (c.type == DrawCommand::Type::fill_current_path && c.f[0] == 0.0f) {
            saw_nonzero = true;
            break;
        }
    }
    REQUIRE(saw_nonzero);
}

TEST_CASE("SvgImage::render emits both fill + stroke for filled+stroked path [issue-72]",
          "[canvas][svg][issue-72]") {
    auto img = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100" width="100" height="100">
  <circle cx="50" cy="50" r="40" fill="#f00" stroke="#000" stroke-width="2"/>
</svg>
)");
    REQUIRE(img.is_valid());

    RecordingCanvas canvas;
    img.render(canvas, 0, 0, 100, 100);

    REQUIRE(canvas.count(DrawCommand::Type::fill_current_path) >= 1);
    REQUIRE(canvas.count(DrawCommand::Type::stroke_current_path) >= 1);
}

TEST_CASE("SvgImage move assignment", "[canvas][svg]") {
    auto img1 = SvgImage::from_string(test_svg);
    REQUIRE(img1.is_valid());

    auto img2 = SvgImage::from_string(R"(
<svg xmlns="http://www.w3.org/2000/svg" width="8" height="8">
  <rect width="8" height="8" fill="#0000ff"/>
</svg>
)");
    REQUIRE(img2.is_valid());

    img2 = std::move(img1);

    REQUIRE(img2.is_valid());
    REQUIRE_FALSE(img1.is_valid());
    REQUIRE(img2.width() == 100.0f);
    REQUIRE(img2.height() == 100.0f);
}
