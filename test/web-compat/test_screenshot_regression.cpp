// Screenshot regression framework — baseline comparison with auto-update support.
// Set PULP_UPDATE_BASELINES=1 to refresh baselines on first run.

#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;

// ═══════════════════════════════════════════════════════════════════════════════
// Regression infrastructure tests
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Regression: render_to_png produces valid PNG", "[screenshot][regression]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_theme(Theme::dark());
    root.set_background_color(Color::rgba8(30, 30, 46));

    auto png = render_to_png(root, 100, 100, 1.0f);
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() > 8);
    // PNG magic bytes
    REQUIRE(png[0] == 0x89);
    REQUIRE(png[1] == 'P');
    REQUIRE(png[2] == 'N');
    REQUIRE(png[3] == 'G');
}

TEST_CASE("Regression: render_to_file creates file on disk", "[screenshot][regression]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_background_color(Color::rgba8(255, 0, 0));

    auto path = std::filesystem::temp_directory_path() / "pulp_regression_test.png";
    bool ok = render_to_file(root, 100, 100, path.string(), 1.0f);
    REQUIRE(ok);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::file_size(path) > 100);
    std::filesystem::remove(path);
}

TEST_CASE("Regression: same view renders identically", "[screenshot][regression]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_background_color(Color::rgba8(128, 64, 255));

    auto a = render_to_png(root, 100, 100, 1.0f);
    auto b = render_to_png(root, 100, 100, 1.0f);
    REQUIRE(a.size() == b.size());
    auto cmp = compare_images(a, b, Tolerance::exact);
    REQUIRE(cmp.passed);
}

TEST_CASE("Regression: different backgrounds produce different images", "[screenshot][regression]") {
    View root_a;
    root_a.set_bounds({0, 0, 100, 100});
    root_a.set_background_color(Color::rgba8(255, 0, 0));

    View root_b;
    root_b.set_bounds({0, 0, 100, 100});
    root_b.set_background_color(Color::rgba8(0, 0, 255));

    auto a = render_to_png(root_a, 100, 100, 1.0f);
    auto b = render_to_png(root_b, 100, 100, 1.0f);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());
    // If sizes match, content should differ
    if (a.size() == b.size()) {
        auto cmp = compare_images(a, b, Tolerance::exact);
        REQUIRE_FALSE(cmp.passed);
    }
}

TEST_CASE("Regression: widget rendering is non-empty", "[screenshot][regression]") {
    TestEnvironment env(200, 100);
    env.root.set_theme(Theme::dark());
    env.eval(R"JS(
        createKnob("k");
        setFlex("k", "width", 60); setFlex("k", "height", 60);
        setValue("k", 0.75);
    )JS");
    env.root.layout_children();

    auto png = render_to_png(env.root, 200, 100, 1.0f);
    REQUIRE_FALSE(png.empty());
    REQUIRE(png.size() > 200); // Non-trivial content
}

TEST_CASE("Regression: scale factor 2x produces larger image data", "[screenshot][regression]") {
    View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_background_color(Color::rgba8(0, 128, 0));

    auto png_1x = render_to_png(root, 100, 100, 1.0f);
    auto png_2x = render_to_png(root, 100, 100, 2.0f);
    REQUIRE_FALSE(png_1x.empty());
    REQUIRE_FALSE(png_2x.empty());
    // 2x should produce more pixel data (larger PNG)
    REQUIRE(png_2x.size() > png_1x.size());
}
