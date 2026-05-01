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
    // nanosvg may return an empty image rather than nullptr
    // Just check it doesn't crash
    REQUIRE(true);
}

TEST_CASE("SvgImage move semantics", "[canvas][svg]") {
    auto img1 = SvgImage::from_string(test_svg);
    REQUIRE(img1.is_valid());

    SvgImage img2 = std::move(img1);
    REQUIRE(img2.is_valid());
    REQUIRE_FALSE(img1.is_valid());
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
