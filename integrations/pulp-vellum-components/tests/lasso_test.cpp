#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/lasso.hpp>
#include <vellum/canvas/recording_canvas.hpp>

#include "render_helpers.hpp"

#include <memory>
#include <vector>

using namespace pulp::vellum_components;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

TEST_CASE("SelectionRect contains and intersects use half-open bounds",
          "[pulp-vellum-components][lasso]") {
    SelectionRect rect{10.0f, 20.0f, 30.0f, 40.0f};

    REQUIRE(rect.contains(10.0f, 20.0f));
    REQUIRE(rect.contains(39.9f, 59.9f));
    REQUIRE_FALSE(rect.contains(40.0f, 60.0f));
    REQUIRE_FALSE(rect.contains(9.9f, 20.0f));

    REQUIRE(rect.intersects(0.0f, 0.0f, 11.0f, 21.0f));
    REQUIRE(rect.intersects(39.0f, 59.0f, 10.0f, 10.0f));
    REQUIRE_FALSE(rect.intersects(40.0f, 20.0f, 10.0f, 10.0f));
    REQUIRE_FALSE(rect.intersects(0.0f, 0.0f, 10.0f, 10.0f));
}

TEST_CASE("LassoComponent normalizes reverse drags and fires callbacks",
          "[pulp-vellum-components][lasso]") {
    LassoComponent lasso;
    std::vector<SelectionRect> changed;
    std::vector<SelectionRect> completed;
    lasso.on_selection_changed = [&](const SelectionRect& rect) { changed.push_back(rect); };
    lasso.on_selection_complete = [&](const SelectionRect& rect) { completed.push_back(rect); };

    lasso.begin_selection(50.0f, 60.0f);
    REQUIRE(lasso.is_active());

    lasso.update_selection(20.0f, 10.0f);
    REQUIRE(changed.size() == 1);
    REQUIRE(changed[0].x == 20.0f);
    REQUIRE(changed[0].y == 10.0f);
    REQUIRE(changed[0].width == 30.0f);
    REQUIRE(changed[0].height == 50.0f);

    lasso.end_selection();
    REQUIRE_FALSE(lasso.is_active());
    REQUIRE(completed.size() == 1);
    REQUIRE(completed[0].x == 20.0f);
    REQUIRE(completed[0].height == 50.0f);

    lasso.update_selection(100.0f, 100.0f);
    lasso.end_selection();
    REQUIRE(changed.size() == 1);
    REQUIRE(completed.size() == 1);
}

TEST_CASE("LassoComponent mouse helpers drive paint lifecycle", "[pulp-vellum-components][lasso]") {
    LassoComponent lasso;

    RecordingCanvas inactive;
    lasso.paint(inactive);
    REQUIRE(inactive.commands().empty());

    lasso.on_mouse_down({5.0f, 6.0f});
    lasso.on_mouse_drag({25.0f, 16.0f});

    RecordingCanvas active;
    lasso.paint(active);
    REQUIRE(active.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(active.count(DrawCommand::Type::stroke_rect) == 1);

    lasso.on_mouse_up({25.0f, 16.0f});
    REQUIRE_FALSE(lasso.is_active());

    RecordingCanvas after;
    lasso.paint(after);
    REQUIRE(after.commands().empty());
}

TEST_CASE("LassoComponent paints its translucent fill over the content beneath",
          "[pulp-vellum-components][lasso]") {
    ::vellum::view::View root;
    root.set_bounds({0, 0, 120, 80});
    root.set_background_color(::vellum::canvas::Color::rgba8(20, 20, 20, 255));
    auto owned = std::make_unique<LassoComponent>();
    auto* lasso = owned.get();
    owned->set_bounds({0, 0, 120, 80});
    root.add_child(std::move(owned));

    const auto idle = testing::render(root, 120, 80);
    CHECK(testing::near(idle.at(30, 30), 20, 20, 20, 1));

    lasso->begin_selection(10.0f, 10.0f);
    lasso->update_selection(70.0f, 50.0f);
    const auto active = testing::render(root, 120, 80);
    // Inside: 100,150,255 at alpha 40/255 over 20,20,20.
    const auto inside = active.at(40, 30);
    CHECK(testing::near(inside, 33, 41, 57, 3));
    // Outside the rectangle the content is untouched.
    CHECK(testing::near(active.at(100, 70), 20, 20, 20, 1));
    // The border line is more opaque than the fill.
    CHECK(active.at(40, 10).b > inside.b);
    testing::maybe_write_png(root, 120, 80, "lasso.png");

    lasso->end_selection();
    CHECK(testing::near(testing::render(root, 120, 80).at(40, 30), 20, 20, 20, 1));
}
