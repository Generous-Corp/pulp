#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

TEST_CASE("Geometry types", "[view][geometry]") {
    SECTION("Point arithmetic") {
        Point a{10, 20}, b{5, 3};
        auto sum = a + b;
        REQUIRE(sum.x == 15);
        REQUIRE(sum.y == 23);

        auto diff = a - b;
        REQUIRE(diff.x == 5);
        REQUIRE(diff.y == 17);
    }

    SECTION("Rect contains") {
        Rect r{10, 10, 100, 50};
        REQUIRE(r.contains({50, 30}));
        REQUIRE_FALSE(r.contains({5, 30}));
        REQUIRE_FALSE(r.contains({50, 65}));
    }

    SECTION("Rect inset") {
        Rect r{0, 0, 100, 80};
        auto inset = r.inset(10);
        REQUIRE(inset.x == 10);
        REQUIRE(inset.y == 10);
        REQUIRE(inset.width == 80);
        REQUIRE(inset.height == 60);
    }

    SECTION("Rect center") {
        Rect r{0, 0, 100, 60};
        auto c = r.center();
        REQUIRE_THAT(c.x, WithinAbs(50.0, 0.001));
        REQUIRE_THAT(c.y, WithinAbs(30.0, 0.001));
    }
}

TEST_CASE("View child management", "[view]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    bool attached = false;
    bool detached = false;

    class TestView : public View {
    public:
        bool& attached_ref;
        bool& detached_ref;
        TestView(bool& a, bool& d) : attached_ref(a), detached_ref(d) {}
        void on_attached() override { attached_ref = true; }
        void on_detached() override { detached_ref = true; }
    };

    auto child = std::make_unique<TestView>(attached, detached);
    auto* child_ptr = child.get();

    root.add_child(std::move(child));
    REQUIRE(root.child_count() == 1);
    REQUIRE(root.child_at(0) == child_ptr);
    REQUIRE(child_ptr->parent() == &root);
    REQUIRE(attached);

    auto removed = root.remove_child(child_ptr);
    REQUIRE(removed != nullptr);
    REQUIRE(root.child_count() == 0);
    REQUIRE(detached);
    REQUIRE(removed->parent() == nullptr);
}

TEST_CASE("View hit testing", "[view]") {
    View root;
    root.set_bounds({0, 0, 400, 300});

    auto child1 = std::make_unique<View>();
    child1->set_bounds({10, 10, 100, 100});
    auto* c1_ptr = child1.get();

    auto child2 = std::make_unique<View>();
    child2->set_bounds({200, 50, 150, 200});
    auto* c2_ptr = child2.get();

    root.add_child(std::move(child1));
    root.add_child(std::move(child2));

    REQUIRE(root.hit_test({50, 50}) == c1_ptr);
    REQUIRE(root.hit_test({250, 100}) == c2_ptr);
    REQUIRE(root.hit_test({150, 150}) == &root); // Between children

    // Hidden child should not be hit
    c1_ptr->set_visible(false);
    REQUIRE(root.hit_test({50, 50}) == &root);
}

TEST_CASE("View theme resolution", "[view][theme]") {
    View root;
    root.set_theme(Theme::dark());

    auto child = std::make_unique<View>();
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    // Child resolves from parent's theme
    auto c = child_ptr->resolve_color("bg.primary");
    auto expected = Theme::dark().color("bg.primary").value();
    REQUIRE(c == expected);

    // Child can override
    Theme override_theme;
    override_theme.colors["bg.primary"] = color_from_hex(0xFF0000);
    child_ptr->set_theme(override_theme);

    auto c2 = child_ptr->resolve_color("bg.primary");
    REQUIRE(c2.r8() == 0xFF);
    REQUIRE(c2.g8() == 0x00);

    // Non-overridden colors still resolve from parent
    auto c3 = child_ptr->resolve_color("text.primary");
    auto expected3 = Theme::dark().color("text.primary").value();
    REQUIRE(c3 == expected3);
}

TEST_CASE("View simulate_click dispatches to target", "[view][events]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    bool clicked = false;

    class ClickView : public View {
    public:
        bool& clicked_ref;
        ClickView(bool& c) : clicked_ref(c) {}
        void on_mouse_down(Point) override { clicked_ref = true; }
    };

    auto child = std::make_unique<ClickView>(clicked);
    child->set_bounds({50, 50, 100, 100});
    root.add_child(std::move(child));

    root.simulate_click({75, 75}); // Inside child
    REQUIRE(clicked);
}

TEST_CASE("View simulate_drag calls drag sequence", "[view][events]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    int drag_count = 0;

    class DragView : public View {
    public:
        int& count;
        DragView(int& c) : count(c) {}
        void on_mouse_drag(Point) override { ++count; }
    };

    auto child = std::make_unique<DragView>(drag_count);
    child->set_bounds({0, 0, 200, 200});
    root.add_child(std::move(child));

    root.simulate_drag({10, 10}, {190, 190}, 5);
    REQUIRE(drag_count == 5);
}

TEST_CASE("View keyboard focus traversal", "[view][focus]") {
    View root;
    root.set_bounds({0, 0, 300, 100});

    auto k1 = std::make_unique<Knob>();
    k1->set_id("k1");
    auto* k1_ptr = k1.get();

    auto k2 = std::make_unique<Knob>();
    k2->set_id("k2");
    auto* k2_ptr = k2.get();

    auto label = std::make_unique<Label>("not focusable");

    root.add_child(std::move(k1));
    root.add_child(std::move(label));
    root.add_child(std::move(k2));

    // Focus first
    auto* focused = View::focus_next(root, nullptr);
    REQUIRE(focused == k1_ptr);
    REQUIRE(k1_ptr->has_focus());

    // Tab to next
    focused = View::focus_next(root, focused);
    REQUIRE(focused == k2_ptr);
    REQUIRE(k2_ptr->has_focus());
    REQUIRE_FALSE(k1_ptr->has_focus());

    // Wrap around
    focused = View::focus_next(root, focused);
    REQUIRE(focused == k1_ptr);

    // Shift-Tab backward
    focused = View::focus_prev(root, focused);
    REQUIRE(focused == k2_ptr);
}

TEST_CASE("View accessibility defaults", "[view][a11y]") {
    Knob knob;
    REQUIRE(knob.access_role() == View::AccessRole::slider);

    Fader fader;
    REQUIRE(fader.access_role() == View::AccessRole::slider);

    Toggle toggle;
    REQUIRE(toggle.access_role() == View::AccessRole::toggle);

    Label label("Gain");
    REQUIRE(label.access_role() == View::AccessRole::label);
    REQUIRE(label.access_label() == "Gain");

    Meter meter;
    REQUIRE(meter.access_role() == View::AccessRole::meter);

    View plain;
    REQUIRE(plain.access_role() == View::AccessRole::none);
}

TEST_CASE("View accessibility can be overridden", "[view][a11y]") {
    Knob knob;
    knob.set_access_label("Filter Cutoff");
    knob.set_access_value("1200 Hz");

    REQUIRE(knob.access_label() == "Filter Cutoff");
    REQUIRE(knob.access_value() == "1200 Hz");
}

TEST_CASE("Flex layout column", "[view][layout]") {
    View root;
    root.set_bounds({0, 0, 300, 400});
    root.flex().direction = FlexDirection::column;

    auto c1 = std::make_unique<View>();
    c1->flex().preferred_height = 50;
    auto* c1_ptr = c1.get();

    auto c2 = std::make_unique<View>();
    c2->flex().flex_grow = 1;
    auto* c2_ptr = c2.get();

    auto c3 = std::make_unique<View>();
    c3->flex().preferred_height = 30;
    auto* c3_ptr = c3.get();

    root.add_child(std::move(c1));
    root.add_child(std::move(c2));
    root.add_child(std::move(c3));
    root.layout_children();

    // c1: top, fixed 50px
    REQUIRE_THAT(c1_ptr->bounds().y, WithinAbs(0.0, 0.1));
    REQUIRE_THAT(c1_ptr->bounds().height, WithinAbs(50.0, 0.1));
    REQUIRE_THAT(c1_ptr->bounds().width, WithinAbs(300.0, 0.1));

    // c2: flexible, fills remaining space (400 - 50 - 30 = 320)
    REQUIRE_THAT(c2_ptr->bounds().y, WithinAbs(50.0, 0.1));
    REQUIRE_THAT(c2_ptr->bounds().height, WithinAbs(320.0, 0.1));

    // c3: bottom, fixed 30px
    REQUIRE_THAT(c3_ptr->bounds().y, WithinAbs(370.0, 0.1));
    REQUIRE_THAT(c3_ptr->bounds().height, WithinAbs(30.0, 0.1));
}

TEST_CASE("Flex layout row", "[view][layout]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;
    root.flex().gap = 10;

    auto c1 = std::make_unique<View>();
    c1->flex().preferred_width = 80;
    auto* c1_ptr = c1.get();

    auto c2 = std::make_unique<View>();
    c2->flex().flex_grow = 1;
    auto* c2_ptr = c2.get();

    root.add_child(std::move(c1));
    root.add_child(std::move(c2));
    root.layout_children();

    // c1: left, fixed 80px
    REQUIRE_THAT(c1_ptr->bounds().x, WithinAbs(0.0, 0.1));
    REQUIRE_THAT(c1_ptr->bounds().width, WithinAbs(80.0, 0.1));
    REQUIRE_THAT(c1_ptr->bounds().height, WithinAbs(100.0, 0.1)); // stretched

    // c2: fills remaining (300 - 80 - 10 gap = 210)
    REQUIRE_THAT(c2_ptr->bounds().x, WithinAbs(90.0, 0.1));
    REQUIRE_THAT(c2_ptr->bounds().width, WithinAbs(210.0, 0.1));
}

TEST_CASE("Flex layout with padding", "[view][layout]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.flex().direction = FlexDirection::column;
    root.flex().padding = 20;

    auto child = std::make_unique<View>();
    child->flex().flex_grow = 1;
    auto* child_ptr = child.get();

    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE_THAT(child_ptr->bounds().x, WithinAbs(20.0, 0.1));
    REQUIRE_THAT(child_ptr->bounds().y, WithinAbs(20.0, 0.1));
    REQUIRE_THAT(child_ptr->bounds().width, WithinAbs(160.0, 0.1));
    REQUIRE_THAT(child_ptr->bounds().height, WithinAbs(160.0, 0.1));
}

TEST_CASE("View compositing layer for opacity", "[view][layer]") {
    pulp::canvas::RecordingCanvas rc;
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 50, 50});
    child->set_opacity(0.5f);
    child->set_background_color(pulp::canvas::Color::rgba(1.0f, 0.0f, 0.0f));

    root.add_child(std::move(child));
    root.paint_all(rc);

    REQUIRE(rc.command_count() > 0);
}

TEST_CASE("View needs_layer flag", "[view][layer]") {
    View v;
    REQUIRE_FALSE(v.needs_layer());
    v.set_needs_layer(true);
    REQUIRE(v.needs_layer());
}

TEST_CASE("View filter_blur triggers layer", "[view][layer]") {
    pulp::canvas::RecordingCanvas rc;
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 50, 50});
    child->set_filter_blur(4.0f);
    child->set_background_color(pulp::canvas::Color::rgba(0.0f, 0.0f, 1.0f));

    root.add_child(std::move(child));
    root.paint_all(rc);

    REQUIRE(rc.command_count() > 0);
}
