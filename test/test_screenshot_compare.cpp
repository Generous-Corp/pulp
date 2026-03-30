#include <catch2/catch_test_macros.hpp>
#include <pulp/view/screenshot_compare.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>

using namespace pulp::view;

TEST_CASE("CompareResult passes with high similarity", "[view][compare]") {
    CompareResult r;
    r.valid = true;
    r.similarity = 0.95f;
    REQUIRE(r.passes(0.85f));
    REQUIRE_FALSE(r.passes(0.99f));
}

TEST_CASE("CompareResult fails when invalid", "[view][compare]") {
    CompareResult r;
    r.valid = false;
    r.similarity = 1.0f;
    REQUIRE_FALSE(r.passes());
}

TEST_CASE("compare_screenshots identical images", "[view][compare]") {
    // Render the same view twice — should be identical
    View root;
    root.set_theme(Theme::dark());
    root.flex().direction = FlexDirection::column;
    root.flex().padding = 10;

    auto label = std::make_unique<Label>("Test");
    label->flex().preferred_height = 20;
    root.add_child(std::move(label));

    auto png1 = render_to_png(root, 100, 50, 1.0f);
    auto png2 = render_to_png(root, 100, 50, 1.0f);

    REQUIRE_FALSE(png1.empty());
    REQUIRE_FALSE(png2.empty());

    auto result = compare_screenshots(png1, png2);
    REQUIRE(result.valid);
    REQUIRE(result.similarity >= 0.99f);
    REQUIRE(result.diff_pixels == 0);
    REQUIRE(result.passes());
}

TEST_CASE("compare_screenshots different images", "[view][compare]") {
    // Render two visually distinct views
    View root1;
    root1.set_theme(Theme::dark());
    root1.flex().direction = FlexDirection::column;
    root1.flex().padding = 8;
    auto l1 = std::make_unique<Label>("Dark theme text");
    l1->flex().preferred_height = 30;
    root1.add_child(std::move(l1));

    View root2;
    root2.set_theme(Theme::light());
    root2.flex().direction = FlexDirection::column;
    root2.flex().padding = 8;
    auto l2 = std::make_unique<Label>("Light theme text");
    l2->flex().preferred_height = 30;
    root2.add_child(std::move(l2));

    auto png1 = render_to_png(root1, 100, 50, 1.0f);
    auto png2 = render_to_png(root2, 100, 50, 1.0f);

    REQUIRE_FALSE(png1.empty());
    REQUIRE_FALSE(png2.empty());

    auto result = compare_screenshots(png1, png2, 16);  // Tighter tolerance
    REQUIRE(result.valid);
    REQUIRE(result.similarity < 0.95f);
    REQUIRE(result.diff_pixels > 0);
}

TEST_CASE("compare_screenshots handles empty input", "[view][compare]") {
    std::vector<uint8_t> empty;
    std::vector<uint8_t> valid = {1, 2, 3};  // Not a real PNG

    auto r1 = compare_screenshots(empty, valid);
    REQUIRE_FALSE(r1.valid);

    auto r2 = compare_screenshots(valid, empty);
    REQUIRE_FALSE(r2.valid);
}

TEST_CASE("generate_diff_image produces output", "[view][compare]") {
    View root1;
    root1.set_theme(Theme::dark());
    root1.flex().direction = FlexDirection::column;

    View root2;
    root2.set_theme(Theme::light());
    root2.flex().direction = FlexDirection::column;

    auto png1 = render_to_png(root1, 80, 40, 1.0f);
    auto png2 = render_to_png(root2, 80, 40, 1.0f);

    auto diff = generate_diff_image(png1, png2);
    REQUIRE_FALSE(diff.empty());
    // Diff should be a valid PNG (starts with PNG signature)
    REQUIRE(diff.size() > 8);
    REQUIRE(diff[0] == 0x89);
    REQUIRE(diff[1] == 'P');
    REQUIRE(diff[2] == 'N');
    REQUIRE(diff[3] == 'G');
}
