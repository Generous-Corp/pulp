#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/split_view.hpp>

#include <vector>

using namespace pulp::view;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;

TEST_CASE("SplitView clamps explicit split fractions", "[view][split]") {
    SplitView split;
    REQUIRE(split.split_fraction() == 0.5f);

    split.set_split_fraction(-1.0f);
    REQUIRE(split.split_fraction() == 0.0f);

    split.set_split_fraction(2.0f);
    REQUIRE(split.split_fraction() == 1.0f);
}

TEST_CASE("SplitView lays out horizontal and vertical children",
          "[view][split]") {
    SplitView split;
    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* first_ptr = first.get();
    auto* second_ptr = second.get();

    split.set_first(std::move(first));
    split.set_second(std::move(second));
    split.set_bounds({0.0f, 0.0f, 200.0f, 100.0f});
    split.set_divider_width(4.0f);
    split.set_split_fraction(0.5f);
    split.layout_children();

    REQUIRE(first_ptr->bounds().width == 98.0f);
    REQUIRE(second_ptr->bounds().x == 102.0f);
    REQUIRE(second_ptr->bounds().width == 98.0f);

    split.set_orientation(SplitView::Orientation::vertical);
    split.layout_children();
    REQUIRE(first_ptr->bounds().height == 48.0f);
    REQUIRE(second_ptr->bounds().y == 52.0f);
    REQUIRE(second_ptr->bounds().height == 48.0f);
}

TEST_CASE("SplitView drag honors minimum pane sizes and fires callbacks",
          "[view][split]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 200.0f, 100.0f});
    split.set_divider_width(4.0f);
    split.set_min_first_size(40.0f);
    split.set_min_second_size(60.0f);

    std::vector<float> callbacks;
    split.on_split_changed = [&](float fraction) { callbacks.push_back(fraction); };

    split.on_mouse_down({100.0f, 50.0f});
    split.on_mouse_drag({10.0f, 50.0f});
    REQUIRE(split.split_fraction() == 0.2f);
    REQUIRE(callbacks.back() == 0.2f);

    split.on_mouse_drag({190.0f, 50.0f});
    REQUIRE(split.split_fraction() == 0.7f);
    REQUIRE(callbacks.back() == 0.7f);

    split.on_mouse_up({190.0f, 50.0f});
    const auto callback_count = callbacks.size();
    split.on_mouse_drag({100.0f, 50.0f});
    REQUIRE(callbacks.size() == callback_count);
}

TEST_CASE("SplitView paint draws divider and orientation-specific grips",
          "[view][split]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 200.0f, 100.0f});
    split.set_divider_width(4.0f);

    RecordingCanvas horizontal;
    split.paint(horizontal);
    REQUIRE(horizontal.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(horizontal.count(DrawCommand::Type::fill_circle) == 3);

    split.set_orientation(SplitView::Orientation::vertical);
    RecordingCanvas vertical;
    split.paint(vertical);
    REQUIRE(vertical.count(DrawCommand::Type::fill_rect) == 1);
    REQUIRE(vertical.count(DrawCommand::Type::fill_circle) == 3);
}
