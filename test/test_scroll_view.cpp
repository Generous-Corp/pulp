// Automated test for ScrollView (already exists from animation merge)
#include <catch2/catch_test_macros.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;

TEST_CASE("ScrollView: scroll_by changes offset", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    sv.scroll_by(0, 50);
    REQUIRE(sv.target_scroll_y() > 0.0f);
}

TEST_CASE("ScrollView: scroll wheel event scrolls", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    // macOS: positive delta = scroll up, negative = scroll down
    // scroll_by multiplies by sensitivity (30), so delta_y=1 → scroll_by(0, 30)
    // which increases target_scroll_y (scrolls content up = you see lower content)
    MouseEvent wheel;
    wheel.is_wheel = true;
    wheel.scroll_delta_y = 1.0f;  // Scroll down
    sv.on_mouse_event(wheel);

    REQUIRE(sv.target_scroll_y() > 0.0f);
}

TEST_CASE("ScrollView: scroll clamped to content", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 500});

    // Scroll way past max
    sv.scroll_by(0, 10000);
    // Target should be clamped to max (500 - 100 = 400)
    REQUIRE(sv.target_scroll_y() <= 400.0f);

    // Scroll way past min
    sv.set_scroll(0, 0);
    sv.scroll_by(0, -10000);
    REQUIRE(sv.target_scroll_y() >= 0.0f);
}

TEST_CASE("ScrollView: paint_all renders without crash", "[scrollview]") {
    pulp::canvas::RecordingCanvas rc;
    ScrollView sv;
    sv.set_theme(Theme::dark());
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 300});

    auto label = std::make_unique<Label>("Scrollable content");
    label->set_bounds({0, 0, 200, 20});
    sv.add_child(std::move(label));

    sv.paint(rc);
    REQUIRE(rc.commands().size() > 0);
}

TEST_CASE("ScrollView: bar opacity starts at zero", "[scrollview]") {
    ScrollView sv;
    REQUIRE(sv.bar_opacity() == 0.0f);
}

TEST_CASE("ScrollView: hit testing follows scrolled content", "[scrollview]") {
    ScrollView sv;
    sv.set_bounds({0, 0, 200, 100});
    sv.set_content_size({200, 300});

    auto label = std::make_unique<Label>("Scrolled");
    auto* label_ptr = label.get();
    label->set_bounds({0, 120, 120, 20});
    sv.add_child(std::move(label));

    sv.set_scroll(0, 100);

    auto* hit = sv.hit_test({10, 30});
    REQUIRE(hit == label_ptr);
}
