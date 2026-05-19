#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/lasso.hpp>

#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

TEST_CASE("SelectionRect contains and intersects use half-open bounds",
          "[view][lasso]") {
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
          "[view][lasso]") {
    LassoComponent lasso;
    std::vector<SelectionRect> changed;
    std::vector<SelectionRect> completed;
    lasso.on_selection_changed = [&](const SelectionRect& rect) {
        changed.push_back(rect);
    };
    lasso.on_selection_complete = [&](const SelectionRect& rect) {
        completed.push_back(rect);
    };

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

TEST_CASE("LassoComponent mouse helpers drive paint lifecycle",
          "[view][lasso]") {
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
