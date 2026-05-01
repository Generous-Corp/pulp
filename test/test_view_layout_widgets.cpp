#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/view/concertina_panel.hpp>
#include <pulp/view/split_view.hpp>

using Catch::Matchers::WithinAbs;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using namespace pulp::view;

namespace {

class FixedHeightView : public View {
public:
    explicit FixedHeightView(float height) : height_(height) {}

    float intrinsic_height() const override { return height_; }

private:
    float height_;
};

class PaintedHeightView : public FixedHeightView {
public:
    explicit PaintedHeightView(float height) : FixedHeightView(height) {}

    void paint(pulp::canvas::Canvas& canvas) override {
        ++paint_count;
        canvas.fill_rect(1.0f, 2.0f, 3.0f, 4.0f);
    }

    int paint_count = 0;
};

} // namespace

TEST_CASE("SplitView lays out horizontal and vertical panes around divider",
          "[view][split_view][layout]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 400.0f, 200.0f});
    split.set_divider_width(10.0f);
    split.set_split_fraction(0.25f);

    auto first = std::make_unique<View>();
    auto second = std::make_unique<View>();
    auto* first_ptr = first.get();
    auto* second_ptr = second.get();

    split.set_first(std::move(first));
    split.set_second(std::move(second));
    split.layout_children();

    REQUIRE_THAT(first_ptr->bounds().width, WithinAbs(95.0f, 0.001f));
    REQUIRE_THAT(first_ptr->bounds().height, WithinAbs(200.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().x, WithinAbs(105.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().width, WithinAbs(295.0f, 0.001f));

    split.set_orientation(SplitView::Orientation::vertical);
    split.set_divider_width(8.0f);
    split.layout_children();

    REQUIRE_THAT(first_ptr->bounds().width, WithinAbs(400.0f, 0.001f));
    REQUIRE_THAT(first_ptr->bounds().height, WithinAbs(46.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().y, WithinAbs(54.0f, 0.001f));
    REQUIRE_THAT(second_ptr->bounds().height, WithinAbs(146.0f, 0.001f));
}

TEST_CASE("SplitView drag clamps to minimum pane sizes and reports changes",
          "[view][split_view][interaction]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 400.0f, 100.0f});
    split.set_min_first_size(80.0f);
    split.set_min_second_size(120.0f);

    int callback_count = 0;
    float last_fraction = 0.0f;
    split.on_split_changed = [&](float fraction) {
        ++callback_count;
        last_fraction = fraction;
    };

    split.on_mouse_down({200.0f, 50.0f});
    split.on_mouse_drag({20.0f, 50.0f});
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.2f, 0.001f));
    REQUIRE_THAT(last_fraction, WithinAbs(0.2f, 0.001f));

    split.on_mouse_drag({390.0f, 50.0f});
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.7f, 0.001f));
    REQUIRE(callback_count == 2);

    split.on_mouse_up({390.0f, 50.0f});
    split.on_mouse_drag({160.0f, 50.0f});
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.7f, 0.001f));
}

TEST_CASE("SplitView ignores drags that did not start on the divider",
          "[view][split_view][interaction]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 300.0f, 100.0f});
    split.set_split_fraction(0.5f);

    bool notified = false;
    split.on_split_changed = [&](float) { notified = true; };

    split.on_mouse_down({12.0f, 12.0f});
    split.on_mouse_drag({250.0f, 12.0f});

    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.5f, 0.001f));
    REQUIRE_FALSE(notified);
}

TEST_CASE("SplitView vertical drag uses height constraints",
          "[view][split_view][interaction]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 200.0f, 300.0f});
    split.set_orientation(SplitView::Orientation::vertical);
    split.set_min_first_size(90.0f);
    split.set_min_second_size(60.0f);

    split.on_mouse_down({100.0f, 150.0f});
    split.on_mouse_drag({100.0f, 20.0f});
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.3f, 0.001f));

    split.on_mouse_drag({100.0f, 290.0f});
    REQUIRE_THAT(split.split_fraction(), WithinAbs(0.8f, 0.001f));
}

TEST_CASE("SplitView paint records divider and orientation-specific grips",
          "[view][split_view][paint]") {
    SplitView split;
    split.set_bounds({0.0f, 0.0f, 120.0f, 80.0f});

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

TEST_CASE("ConcertinaPanel keeps content visibility in sync with expansion",
          "[view][concertina][state]") {
    ConcertinaPanel panel;
    auto first = std::make_unique<FixedHeightView>(40.0f);
    auto second = std::make_unique<FixedHeightView>(50.0f);
    auto* first_ptr = first.get();
    auto* second_ptr = second.get();

    panel.add_section("First", std::move(first), false);
    panel.add_section("Second", std::move(second), true);

    REQUIRE_FALSE(first_ptr->visible());
    REQUIRE(second_ptr->visible());

    panel.expand(0);
    REQUIRE(first_ptr->visible());

    panel.collapse(0);
    REQUIRE_FALSE(first_ptr->visible());

    panel.set_exclusive(true);
    panel.expand(0);
    panel.expand(1);
    REQUIRE_FALSE(first_ptr->visible());
    REQUIRE(second_ptr->visible());
}

TEST_CASE("ConcertinaPanel invalid indices are no-ops",
          "[view][concertina][state]") {
    ConcertinaPanel panel;
    panel.add_section("Only", nullptr, false);

    panel.expand(-1);
    panel.expand(5);
    panel.collapse(-1);
    panel.toggle(7);

    REQUIRE_FALSE(panel.is_expanded(-1));
    REQUIRE_FALSE(panel.is_expanded(5));
    REQUIRE_FALSE(panel.is_expanded(0));
}

TEST_CASE("ConcertinaPanel layout positions expanded content and fallback heights",
          "[view][concertina][layout]") {
    ConcertinaPanel panel;
    panel.set_bounds({0.0f, 0.0f, 320.0f, 240.0f});
    panel.set_header_height(20.0f);

    auto first = std::make_unique<FixedHeightView>(40.0f);
    auto collapsed = std::make_unique<FixedHeightView>(70.0f);
    auto fallback = std::make_unique<FixedHeightView>(0.0f);
    auto* first_ptr = first.get();
    auto* collapsed_ptr = collapsed.get();
    auto* fallback_ptr = fallback.get();

    panel.add_section("First", std::move(first), true);
    panel.add_section("Collapsed", std::move(collapsed), false);
    panel.add_section("Fallback", std::move(fallback), true);
    panel.layout_sections();

    REQUIRE_THAT(first_ptr->bounds().x, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(first_ptr->bounds().y, WithinAbs(20.0f, 0.001f));
    REQUIRE_THAT(first_ptr->bounds().width, WithinAbs(320.0f, 0.001f));
    REQUIRE_THAT(first_ptr->bounds().height, WithinAbs(40.0f, 0.001f));

    REQUIRE_THAT(collapsed_ptr->bounds().height, WithinAbs(0.0f, 0.001f));

    REQUIRE_THAT(fallback_ptr->bounds().y, WithinAbs(100.0f, 0.001f));
    REQUIRE_THAT(fallback_ptr->bounds().height, WithinAbs(100.0f, 0.001f));
}

TEST_CASE("ConcertinaPanel mouse down toggles headers and skips content rows",
          "[view][concertina][interaction]") {
    ConcertinaPanel panel;
    panel.set_header_height(20.0f);
    panel.add_section("First", std::make_unique<FixedHeightView>(40.0f), true);
    panel.add_section("Second", std::make_unique<FixedHeightView>(30.0f), false);

    panel.on_mouse_down({5.0f, 25.0f});
    REQUIRE(panel.is_expanded(0));
    REQUIRE_FALSE(panel.is_expanded(1));

    panel.on_mouse_down({5.0f, 65.0f});
    REQUIRE(panel.is_expanded(1));

    panel.on_mouse_down({5.0f, 5.0f});
    REQUIRE_FALSE(panel.is_expanded(0));
}

TEST_CASE("ConcertinaPanel paint records headers and paints expanded content",
          "[view][concertina][paint]") {
    ConcertinaPanel panel;
    panel.set_bounds({0.0f, 0.0f, 300.0f, 200.0f});
    panel.set_header_height(24.0f);

    auto content = std::make_unique<PaintedHeightView>(42.0f);
    auto* content_ptr = content.get();
    panel.add_section("Open", std::move(content), true);
    panel.add_section("Closed", nullptr, false);

    RecordingCanvas canvas;
    panel.paint(canvas);

    REQUIRE(content_ptr->paint_count == 1);
    REQUIRE_THAT(content_ptr->bounds().x, WithinAbs(0.0f, 0.001f));
    REQUIRE_THAT(content_ptr->bounds().y, WithinAbs(24.0f, 0.001f));
    REQUIRE_THAT(content_ptr->bounds().width, WithinAbs(300.0f, 0.001f));
    REQUIRE_THAT(content_ptr->bounds().height, WithinAbs(42.0f, 0.001f));
    REQUIRE(canvas.count(DrawCommand::Type::fill_rect) >= 4);
    REQUIRE(canvas.count(DrawCommand::Type::fill_text) == 4);
    REQUIRE(canvas.count(DrawCommand::Type::translate) == 1);
}
