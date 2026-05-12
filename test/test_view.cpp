#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using Catch::Matchers::WithinAbs;

namespace {

class DummyWindowHost final : public WindowHost {
public:
    void show() override {}
    void hide() override {}
    bool is_visible() const override { return false; }
    void repaint() override { ++repaint_count; }
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}

    int repaint_count = 0;
};

class DummyPluginViewHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override { ++repaint_count; }
    void set_size(uint32_t width, uint32_t height) override { size_ = {width, height}; }
    Size get_size() const override { return size_; }

    int repaint_count = 0;

private:
    Size size_{};
};

class HostAwareView final : public View {
public:
    void on_attached() override {
        seen_window_host = window_host();
        seen_plugin_view_host = plugin_view_host();
    }

    WindowHost* seen_window_host = nullptr;
    PluginViewHost* seen_plugin_view_host = nullptr;
};

} // namespace

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

TEST_CASE("View child removal ignores unknown children", "[view][coverage]") {
    View root;
    auto child = std::make_unique<View>();
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    View stranger;
    auto removed = root.remove_child(&stranger);

    REQUIRE(removed == nullptr);
    REQUIRE(root.child_count() == 1);
    REQUIRE(root.child_at(0) == child_ptr);
    REQUIRE(child_ptr->parent() == &root);
}

TEST_CASE("View host references propagate across subtrees", "[view][hosts]") {
    View root;

    auto branch = std::make_unique<View>();
    auto grandchild = std::make_unique<View>();
    auto* grandchild_ptr = grandchild.get();
    branch->add_child(std::move(grandchild));
    auto* branch_ptr = branch.get();
    root.add_child(std::move(branch));

    DummyWindowHost window_host;
    DummyPluginViewHost plugin_view_host;
    root.set_window_host(&window_host);
    root.set_plugin_view_host(&plugin_view_host);

    REQUIRE(root.window_host() == &window_host);
    REQUIRE(root.plugin_view_host() == &plugin_view_host);
    REQUIRE(branch_ptr->window_host() == &window_host);
    REQUIRE(branch_ptr->plugin_view_host() == &plugin_view_host);
    REQUIRE(grandchild_ptr->window_host() == &window_host);
    REQUIRE(grandchild_ptr->plugin_view_host() == &plugin_view_host);

    auto attached_after_host = std::make_unique<HostAwareView>();
    auto* attached_after_host_ptr = attached_after_host.get();
    branch_ptr->add_child(std::move(attached_after_host));

    REQUIRE(attached_after_host_ptr->seen_window_host == &window_host);
    REQUIRE(attached_after_host_ptr->seen_plugin_view_host == &plugin_view_host);

    auto removed = root.remove_child(branch_ptr);
    REQUIRE(removed != nullptr);
    REQUIRE(removed->window_host() == nullptr);
    REQUIRE(removed->plugin_view_host() == nullptr);
    REQUIRE(removed->child_at(0)->window_host() == nullptr);
    REQUIRE(removed->child_at(0)->plugin_view_host() == nullptr);
    REQUIRE(attached_after_host_ptr->window_host() == nullptr);
    REQUIRE(attached_after_host_ptr->plugin_view_host() == nullptr);
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

TEST_CASE("View hit testing honors disabled hit-testable and overflow states",
          "[view][coverage]") {
    View root;
    root.set_bounds({0, 0, 160, 160});

    auto child = std::make_unique<View>();
    child->set_bounds({20, 20, 40, 20});
    child->set_overflow(View::Overflow::visible);
    auto* child_ptr = child.get();
    auto grandchild = std::make_unique<View>();
    grandchild->set_bounds({0, 30, 20, 20});
    auto* grandchild_ptr = grandchild.get();
    child->add_child(std::move(grandchild));
    root.add_child(std::move(child));

    REQUIRE(root.hit_test({30, 55}) == grandchild_ptr);

    child_ptr->set_hit_testable(false);
    REQUIRE(root.hit_test({30, 55}) == &root);

    child_ptr->set_hit_testable(true);
    child_ptr->set_enabled(false);
    REQUIRE(root.hit_test({30, 55}) == &root);

    child_ptr->set_enabled(true);
    REQUIRE(root.hit_test({200, 200}) == nullptr);
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

TEST_CASE("View dimensions frame clock and repaint helpers resolve inherited state",
          "[view][coverage]") {
    class ResizedView : public View {
    public:
        void on_resized() override { ++resized_count; }
        int resized_count = 0;
    };

    ResizedView resized;
    resized.set_bounds({0, 0, 32, 24});
    resized.set_bounds({0, 0, 32, 24});
    resized.set_bounds({0, 0, 64, 24});
    REQUIRE(resized.resized_count == 2);

    View root;
    Theme theme;
    theme.dimensions["spacing.tight"] = 7.0f;
    root.set_theme(theme);

    auto child = std::make_unique<View>();
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    REQUIRE_THAT(child_ptr->resolve_dimension("spacing.tight", 1.0f),
                 WithinAbs(7.0f, 0.001));
    REQUIRE_THAT(child_ptr->resolve_dimension("spacing.missing", 3.0f),
                 WithinAbs(3.0f, 0.001));

    FrameClock clock;
    root.set_frame_clock(&clock);
    REQUIRE(root.frame_clock() == &clock);
    REQUIRE(child_ptr->frame_clock() == &clock);
    REQUIRE(View{}.frame_clock() == nullptr);

    DummyWindowHost window_host;
    root.request_repaint();
    REQUIRE(window_host.repaint_count == 0);
    root.set_window_host(&window_host);
    child_ptr->request_repaint();
    REQUIRE(window_host.repaint_count == 1);

    View plugin_root;
    DummyPluginViewHost plugin_host;
    plugin_root.set_plugin_view_host(&plugin_host);
    plugin_root.request_repaint();
    REQUIRE(plugin_host.repaint_count == 1);
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

TEST_CASE("View pointer capture hover and inspector hooks cover edge paths",
          "[view][coverage]") {
    View v;

    v.set_pointer_capture(7);
    v.set_pointer_capture(7);
    REQUIRE(v.has_pointer_capture(7));
    v.release_pointer_capture(3);
    REQUIRE(v.has_pointer_capture(7));
    v.release_pointer_capture(7);
    REQUIRE_FALSE(v.has_pointer_capture(7));

    int hover_enter = 0;
    int hover_leave = 0;
    v.on_hover_enter = [&] { ++hover_enter; };
    v.on_hover_leave = [&] { ++hover_leave; };
    v.set_hovered(true);
    v.set_hovered(true);
    v.set_hovered(false);
    REQUIRE(hover_enter == 1);
    REQUIRE(hover_leave == 1);

    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
    REQUIRE_FALSE(View::call_inspector_key_hook(KeyEvent{}));
    REQUIRE_FALSE(View::call_inspector_mouse_hook(MouseEvent{}));

    int key_calls = 0;
    int mouse_calls = 0;
    View::set_inspector_key_hook([&](const KeyEvent& event) {
        ++key_calls;
        return event.key == KeyCode::escape;
    });
    View::set_inspector_mouse_hook([&](const MouseEvent& event) {
        ++mouse_calls;
        return event.is_down;
    });

    REQUIRE(View::call_inspector_key_hook(KeyEvent{KeyCode::escape}));
    REQUIRE(View::call_inspector_mouse_hook(MouseEvent{{}, {}, MouseButton::left, 0, 0, 1, true}));
    REQUIRE(key_calls == 1);
    REQUIRE(mouse_calls == 1);

    int overlay_calls = 0;
    int paint_hook_calls = 0;
    View::overlay_queue().clear();
    View::overlay_queue().push_back(View::OverlayRequest{
        [&](pulp::canvas::Canvas& canvas) {
            ++overlay_calls;
            canvas.fill_rect(0, 0, 1, 1);
        }
    });
    View::overlay_queue().push_back(View::OverlayRequest{});
    View::set_inspector_paint_hook([&](pulp::canvas::Canvas& canvas) {
        ++paint_hook_calls;
        canvas.fill_rect(1, 1, 1, 1);
    });

    pulp::canvas::RecordingCanvas canvas;
    View::paint_overlays(canvas);
    REQUIRE(View::overlay_queue().empty());
    REQUIRE(overlay_calls == 1);
    REQUIRE(paint_hook_calls == 1);
    REQUIRE(canvas.count(pulp::canvas::DrawCommand::Type::fill_rect) == 2);

    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
    View::set_inspector_paint_hook({});
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

TEST_CASE("Grid layout with no columns leaves children unchanged",
          "[view][layout][coverage]") {
    View root;
    root.set_bounds({0, 0, 120, 80});
    root.set_layout_mode(LayoutMode::grid);

    auto child = std::make_unique<View>();
    child->set_bounds({5, 6, 7, 8});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    root.layout_children();

    REQUIRE_THAT(child_ptr->bounds().x, WithinAbs(5.0f, 0.001));
    REQUIRE_THAT(child_ptr->bounds().y, WithinAbs(6.0f, 0.001));
    REQUIRE_THAT(child_ptr->bounds().width, WithinAbs(7.0f, 0.001));
    REQUIRE_THAT(child_ptr->bounds().height, WithinAbs(8.0f, 0.001));
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

// ── View::set_transform_matrix (issue-930) ────────────────────────────────
//
// Full 2D affine matrix on a View, applied at paint time as a concat onto
// the current canvas matrix. Mirrors CanvasRenderingContext2D.setTransform's
// (a,b,c,d,e,f) layout. Used by the React/CSS adapter for translateX(-50%)
// centering and as the foundation for animation.

TEST_CASE("View transform_matrix is identity-disabled by default", "[view][transform][issue-930]") {
    View v;
    REQUIRE_FALSE(v.has_transform_matrix());

    v.set_transform_matrix(2.0f, 0.0f, 0.0f, 3.0f, 17.0f, 23.0f);
    REQUIRE(v.has_transform_matrix());

    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    v.get_transform_matrix(a, b, c, d, e, f);
    REQUIRE_THAT(a, WithinAbs(2.0f, 1e-5f));
    REQUIRE_THAT(b, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(c, WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(d, WithinAbs(3.0f, 1e-5f));
    REQUIRE_THAT(e, WithinAbs(17.0f, 1e-5f));
    REQUIRE_THAT(f, WithinAbs(23.0f, 1e-5f));

    v.clear_transform_matrix();
    REQUIRE_FALSE(v.has_transform_matrix());
}

TEST_CASE("View::paint_all emits concat_transform when matrix is set",
          "[view][transform][issue-930]") {
    pulp::canvas::RecordingCanvas rc;
    View v;
    v.set_bounds({0, 0, 100, 100});

    // No matrix set: paint_all should NOT emit concat_transform.
    v.paint_all(rc);
    REQUIRE(rc.count(pulp::canvas::DrawCommand::Type::concat_transform) == 0);

    // Set matrix: paint_all emits exactly one concat_transform with the
    // CanvasRenderingContext2D (a,b,c,d,e,f) order preserved.
    rc.clear();
    v.set_transform_matrix(2.0f, 0.5f, 0.25f, 3.0f, 17.0f, 23.0f);
    v.paint_all(rc);

    int found = 0;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::concat_transform) {
            REQUIRE_THAT(cmd.f[0], WithinAbs(2.0f,  1e-5f));
            REQUIRE_THAT(cmd.f[1], WithinAbs(0.5f,  1e-5f));
            REQUIRE_THAT(cmd.f[2], WithinAbs(0.25f, 1e-5f));
            REQUIRE_THAT(cmd.f[3], WithinAbs(3.0f,  1e-5f));
            REQUIRE_THAT(cmd.f[4], WithinAbs(17.0f, 1e-5f));
            REQUIRE_THAT(cmd.f[5], WithinAbs(23.0f, 1e-5f));
            ++found;
        }
    }
    REQUIRE(found == 1);
}

TEST_CASE("View transform composes — translateX(-50%) child lands at correct root position",
          "[view][transform][issue-930]") {
    // Acceptance scenario from the issue: a parent View at root bounds
    // (0,0,100,100) with set_transform(1,0,0,1,50,0) (a translation of +50
    // in x) should mean a child painted at local (10,10) lands at root (60,10).
    //
    // We assert by inspecting the recorded command stream:
    //   1. parent's bounds translate(0, 0)
    //   2. parent's concat_transform(1, 0, 0, 1, 50, 0)
    //   3. child's bounds translate(10, 10)
    // Walking these in order on a hypothetical SkMatrix accumulates (60, 10)
    // as the child's origin in root space — the same composition Skia performs.
    pulp::canvas::RecordingCanvas rc;

    View root;
    root.set_bounds({0, 0, 100, 100});
    root.set_transform_matrix(1.0f, 0.0f, 0.0f, 1.0f, 50.0f, 0.0f);

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 30, 30});

    root.add_child(std::move(child));
    root.paint_all(rc);

    // Replay just the transform-affecting ops to compute the child's origin
    // in root space — the test invariant is "root translate ∘ root concat ∘
    // child translate = (60, 10)".
    float tx = 0.0f, ty = 0.0f;
    bool saw_root_concat = false;
    int saves = 0;
    int translate_idx = 0;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::save) ++saves;
        if (cmd.type == pulp::canvas::DrawCommand::Type::translate) {
            // Order: [0] root bounds(0,0), [1] (after concat) child bounds(10,10)
            if (translate_idx == 0) {
                REQUIRE_THAT(cmd.f[0], WithinAbs(0.0f, 1e-5f));
                REQUIRE_THAT(cmd.f[1], WithinAbs(0.0f, 1e-5f));
            } else if (translate_idx == 1) {
                tx = cmd.f[0];
                ty = cmd.f[1];
            }
            ++translate_idx;
        }
        if (cmd.type == pulp::canvas::DrawCommand::Type::concat_transform) {
            REQUIRE_THAT(cmd.f[4], WithinAbs(50.0f, 1e-5f));  // e
            REQUIRE_THAT(cmd.f[5], WithinAbs(0.0f,  1e-5f));  // f
            saw_root_concat = true;
        }
    }
    REQUIRE(saw_root_concat);
    REQUIRE(saves >= 2);  // root + child
    // Child's local-bounds translate happens in the child's saved frame,
    // which is the parent frame * concat(50,0). So child at local (10,10)
    // sits at root (10 + 50, 10 + 0) = (60, 10).
    REQUIRE_THAT(tx, WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(ty, WithinAbs(10.0f, 1e-5f));
}

TEST_CASE("View transform_matrix does not affect layout or hit testing",
          "[view][transform][issue-930]") {
    // Transforms are paint-only — Yoga and hit_test see the un-transformed
    // bounds. This is the contract called out in the issue's acceptance
    // criteria.
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({10, 10, 50, 50});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    root.set_transform_matrix(1.0f, 0.0f, 0.0f, 1.0f, 500.0f, 500.0f);

    // hit_test at the un-transformed bounds still finds the child.
    auto* hit = root.hit_test({30.0f, 30.0f});
    REQUIRE(hit == child_ptr);

    // hit_test where the transformed paint *would* land does NOT find anything
    // — confirming the transform is paint-only and hit-testing ignores it.
    auto* missed = root.hit_test({530.0f, 530.0f});
    REQUIRE(missed != child_ptr);
}

// ── pulp #972 — z-index paint-order ──────────────────────────────────────────

TEST_CASE("sorted_children_by_z_index returns insertion order at default z=0",
          "[view][issue-972]") {
    View parent;
    auto a = std::make_unique<View>(); auto* a_ptr = a.get();
    auto b = std::make_unique<View>(); auto* b_ptr = b.get();
    auto c = std::make_unique<View>(); auto* c_ptr = c.get();
    parent.add_child(std::move(a));
    parent.add_child(std::move(b));
    parent.add_child(std::move(c));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == a_ptr);
    REQUIRE(order[1] == b_ptr);
    REQUIRE(order[2] == c_ptr);
}

TEST_CASE("sorted_children_by_z_index sorts ascending — higher z comes last",
          "[view][issue-972]") {
    // Spectr's bandsMenu repro: insertion order is content, content, popover,
    // but popover has zIndex=20. Sorted order must paint popover last.
    View parent;
    auto content_a = std::make_unique<View>(); auto* a_ptr = content_a.get();
    auto popover  = std::make_unique<View>(); auto* p_ptr = popover.get();
    auto content_b = std::make_unique<View>(); auto* b_ptr = content_b.get();
    popover->set_z_index(20);

    parent.add_child(std::move(content_a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(content_b));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order.size() == 3);
    REQUIRE(order[0] == a_ptr);    // z=0, inserted 1st
    REQUIRE(order[1] == b_ptr);    // z=0, inserted 3rd — keeps insertion order at equal z
    REQUIRE(order[2] == p_ptr);    // z=20 — last → topmost
}

TEST_CASE("sorted_children_by_z_index is stable for equal z (insertion order)",
          "[view][issue-972]") {
    View parent;
    auto a = std::make_unique<View>(); auto* a_ptr = a.get();
    auto b = std::make_unique<View>(); auto* b_ptr = b.get();
    auto c = std::make_unique<View>(); auto* c_ptr = c.get();
    a->set_z_index(5);
    b->set_z_index(5);
    c->set_z_index(5);
    parent.add_child(std::move(a));
    parent.add_child(std::move(b));
    parent.add_child(std::move(c));

    auto order = parent.sorted_children_by_z_index();
    REQUIRE(order[0] == a_ptr);
    REQUIRE(order[1] == b_ptr);
    REQUIRE(order[2] == c_ptr);
}

TEST_CASE("View::paint_all paints higher-z child last so it lands on top",
          "[view][issue-972]") {
    using namespace pulp::canvas;

    // Three siblings stacked at the same bounds with distinct backgrounds.
    // Popover sits in the MIDDLE of insertion order with z_index=10. The
    // last full-bounds fill in the recorded stream must be popover's
    // colour — without #972 it would be content_b's (last by insertion).
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto content_a = std::make_unique<View>();
    content_a->set_bounds({0, 0, 100, 100});
    content_a->set_background_color(Color::rgba8(255, 0, 0, 255));   // red

    auto popover = std::make_unique<View>();
    popover->set_bounds({0, 0, 100, 100});
    popover->set_background_color(Color::rgba8(0, 200, 0, 255));     // green
    popover->set_z_index(10);

    auto content_b = std::make_unique<View>();
    content_b->set_bounds({0, 0, 100, 100});
    content_b->set_background_color(Color::rgba8(0, 0, 255, 255));   // blue

    parent.add_child(std::move(content_a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(content_b));

    RecordingCanvas rc;
    parent.paint_all(rc);

    // Walk full-bounds fill_rects and capture the last one's set_fill_color.
    Color last_fill{};
    Color last_full_bounds_fill{};
    bool saw_full_bounds = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 100.0f) {
            last_full_bounds_fill = last_fill;
            saw_full_bounds = true;
        }
    }
    REQUIRE(saw_full_bounds);
    INFO("last_full_bounds rgba=("
         << int(last_full_bounds_fill.r8()) << ","
         << int(last_full_bounds_fill.g8()) << ","
         << int(last_full_bounds_fill.b8()) << ","
         << int(last_full_bounds_fill.a8()) << ")");
    REQUIRE(last_full_bounds_fill.r8() == 0);
    REQUIRE(last_full_bounds_fill.g8() == 200);
    REQUIRE(last_full_bounds_fill.b8() == 0);
}

TEST_CASE("View::hit_test returns the highest-z child for overlapping bounds",
          "[view][issue-972]") {
    // Three siblings at the same bounds: content (z=0, inserted last),
    // popover (z=10, inserted middle), content (z=0, inserted first).
    // Click in the middle should hit the popover, not whichever content
    // happened to be inserted last.
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto a = std::make_unique<View>();
    a->set_bounds({0, 0, 100, 100});
    auto* a_ptr = a.get();

    auto popover = std::make_unique<View>();
    popover->set_bounds({0, 0, 100, 100});
    popover->set_z_index(10);
    auto* p_ptr = popover.get();

    auto b = std::make_unique<View>();
    b->set_bounds({0, 0, 100, 100});
    auto* b_ptr = b.get();

    parent.add_child(std::move(a));
    parent.add_child(std::move(popover));
    parent.add_child(std::move(b));

    auto* hit = parent.hit_test({50.0f, 50.0f});
    REQUIRE(hit == p_ptr);
    REQUIRE(hit != a_ptr);
    REQUIRE(hit != b_ptr);
}

TEST_CASE("View::hit_test falls back to insertion-order topmost at equal z",
          "[view][issue-972]") {
    // All siblings at same z=0 (default) — last inserted is visually topmost,
    // matching legacy behaviour. This locks in the no-regression contract.
    View parent;
    parent.set_bounds({0, 0, 100, 100});

    auto a = std::make_unique<View>();
    a->set_bounds({0, 0, 100, 100});
    parent.add_child(std::move(a));

    auto b = std::make_unique<View>();
    b->set_bounds({0, 0, 100, 100});
    auto* b_ptr = b.get();
    parent.add_child(std::move(b));

    auto* hit = parent.hit_test({50.0f, 50.0f});
    REQUIRE(hit == b_ptr);
}

// ── pulp #972 — overflow:visible default for absolute-positioned popovers ────
// Symptom on Spectr's bandsMenu: a `position:absolute; top:28; right:0`
// popover declared inside a 24px-tall flex parent renders nowhere because
// Pulp previously defaulted overflow to hidden and clipped paint to the
// parent's content rect. CSS default is `overflow: visible` — children
// that extend past the parent should still paint. Z-index sorting alone
// (above) is necessary but not sufficient; the clip was eating the
// popover's fill regardless of paint order.

TEST_CASE("View default overflow is visible (matches CSS default)",
          "[view][issue-972]") {
    View v;
    REQUIRE(v.overflow() == View::Overflow::visible);
}

TEST_CASE("View::paint_all does not emit clip_rect when overflow is visible",
          "[view][issue-972]") {
    using namespace pulp::canvas;
    View v;
    v.set_bounds({0, 0, 100, 24});
    REQUIRE(v.overflow() == View::Overflow::visible);

    RecordingCanvas rc;
    v.paint_all(rc);

    for (const auto& cmd : rc.commands()) {
        // The compositing-layer save_layer / save_backdrop_filter may
        // emit their own clip-shaped commands as part of layer setup,
        // but a plain clip_rect at the view's content bounds is the
        // overflow:hidden marker we explicitly want absent.
        if (cmd.type == DrawCommand::Type::clip_rect) {
            const bool covers_bounds = (cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
                                        cmd.f[2] == 100.0f && cmd.f[3] == 24.0f);
            INFO("Unexpected clip_rect at (" << cmd.f[0] << "," << cmd.f[1]
                 << "," << cmd.f[2] << "," << cmd.f[3] << ")");
            REQUIRE_FALSE(covers_bounds);
        }
    }
}

TEST_CASE("View::paint_all emits clip_rect when overflow is explicitly hidden",
          "[view][issue-972]") {
    using namespace pulp::canvas;
    View v;
    v.set_bounds({0, 0, 100, 24});
    v.set_overflow(View::Overflow::hidden);

    RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_bounds_clip = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 24.0f) {
            saw_bounds_clip = true;
            break;
        }
    }
    REQUIRE(saw_bounds_clip);
}

TEST_CASE("Absolute child positioned outside parent's bounds still paints",
          "[view][issue-972]") {
    // Spectr bandsMenu repro: 24px-tall parent with a popover-like child
    // at top:50, left:50 — completely outside the parent's content rect.
    // With overflow:visible default, the child's translate-from-bounds.x/y
    // and its own paint commands must reach the canvas without a parent
    // clip_rect blocking them.
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 24});

    auto popover = std::make_unique<View>();
    popover->set_bounds({50, 50, 100, 50});
    popover->set_background_color(Color::rgba8(255, 0, 255, 255));   // magenta
    parent.add_child(std::move(popover));

    RecordingCanvas rc;
    parent.paint_all(rc);

    // Verify a magenta fill_rect lands in the recorded stream — the
    // popover's bg fill is at the child's local origin, which becomes
    // (50, 50) in parent coords after the translate inside paint_all.
    Color last_fill{};
    bool saw_magenta_fill = false;
    bool saw_parent_clip = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::set_fill_color) {
            last_fill = cmd.color;
            continue;
        }
        if (cmd.type == DrawCommand::Type::fill_rect &&
            last_fill.r8() == 255 && last_fill.g8() == 0 &&
            last_fill.b8() == 255 && last_fill.a8() == 255) {
            saw_magenta_fill = true;
        }
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 24.0f) {
            saw_parent_clip = true;
        }
    }
    REQUIRE(saw_magenta_fill);
    REQUIRE_FALSE(saw_parent_clip);
}

// pulp #1409 paint-time probe series — empirical answer to the Spectr
// [I] "preset items render outside overlay" report. The framework
// already pushes a parent clip_rect before children paint when the
// parent has overflow:hidden, so the symptom is consumer-side, not
// framework-side. These tests stand as a regression guard — if any of
// them flips to a fail, paint-time clipping has regressed.
TEST_CASE("Probe: overflow:hidden parent clip-rect precedes child paint",
          "[view][probe][issue-overlay-clip]") {
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 100});
    parent.set_overflow(View::Overflow::hidden);

    auto child = std::make_unique<View>();
    child->set_bounds({0, 0, 200, 100});
    child->set_background_color(Color::rgba8(0, 255, 0, 255));
    parent.add_child(std::move(child));

    RecordingCanvas rc;
    parent.paint_all(rc);

    int parent_clip_idx = -1;
    int child_fill_idx = -1;
    Color last_fill{};
    for (int i = 0; i < (int)rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 100.0f &&
            parent_clip_idx == -1) {
            parent_clip_idx = i;
        }
        if (cmd.type == DrawCommand::Type::set_fill_color) last_fill = cmd.color;
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[2] == 200.0f && cmd.f[3] == 100.0f &&
            last_fill.g8() == 255 && last_fill.r8() == 0 &&
            child_fill_idx == -1) {
            child_fill_idx = i;
        }
    }
    INFO("parent_clip_idx=" << parent_clip_idx << " child_fill_idx=" << child_fill_idx
         << " total_cmds=" << rc.commands().size());
    REQUIRE(parent_clip_idx >= 0);
    REQUIRE(child_fill_idx >= 0);
    REQUIRE(parent_clip_idx < child_fill_idx);
}

// pulp #1409 paint-time probe — absolutely-positioned child translated
// past the parent's right edge. Mirrors a Spectr preset-menu item
// scenario where a row's `position: absolute; left: 80px` puts it past
// a 100×40 dropdown's content rect. The clip must STILL precede the
// child's paint command in the recording — translates do not reset the
// active clip.
TEST_CASE("Probe: overflow:hidden parent clips translated absolute child too",
          "[view][probe][issue-overlay-clip]") {
    using namespace pulp::canvas;

    View parent;
    parent.set_bounds({0, 0, 100, 40});
    parent.set_overflow(View::Overflow::hidden);

    auto child = std::make_unique<View>();
    // Bounds-x positions the child at parent-x=80, so its 60px width
    // extends 40px past the parent's right edge.
    child->set_bounds({80, 0, 60, 40});
    child->set_position(View::Position::absolute);
    child->set_background_color(Color::rgba8(0, 0, 255, 255));
    parent.add_child(std::move(child));

    RecordingCanvas rc;
    parent.paint_all(rc);

    int parent_clip_idx = -1;
    int child_fill_idx = -1;
    Color last_fill{};
    for (int i = 0; i < (int)rc.commands().size(); ++i) {
        const auto& cmd = rc.commands()[i];
        if (cmd.type == DrawCommand::Type::clip_rect &&
            cmd.f[0] == 0.0f && cmd.f[1] == 0.0f &&
            cmd.f[2] == 100.0f && cmd.f[3] == 40.0f &&
            parent_clip_idx == -1) {
            parent_clip_idx = i;
        }
        if (cmd.type == DrawCommand::Type::set_fill_color) last_fill = cmd.color;
        if (cmd.type == DrawCommand::Type::fill_rect &&
            cmd.f[2] == 60.0f && cmd.f[3] == 40.0f &&
            last_fill.b8() == 255 && last_fill.r8() == 0 && last_fill.g8() == 0 &&
            child_fill_idx == -1) {
            child_fill_idx = i;
        }
    }
    REQUIRE(parent_clip_idx >= 0);
    REQUIRE(child_fill_idx >= 0);
    REQUIRE(parent_clip_idx < child_fill_idx);
}

// ── pulp #1026: React Native pointerEvents 4-valued enum ────────────────────

TEST_CASE("View::hit_test honors pointerEvents == auto (default)",
          "[view][hit_test][issue-1026]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    // Default (auto_): both root and child are interactive.
    REQUIRE(root.pointer_events() == View::PointerEvents::auto_);
    REQUIRE(root.hit_test({75, 75}) == child_ptr);
    REQUIRE(root.hit_test({10, 10}) == &root);
}

TEST_CASE("View::hit_test honors pointerEvents == none",
          "[view][hit_test][issue-1026]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    root.add_child(std::move(child));

    // none: neither this view NOR descendants intercept events.
    root.set_pointer_events(View::PointerEvents::none);
    REQUIRE(root.hit_test({75, 75}) == nullptr);
    REQUIRE(root.hit_test({10, 10}) == nullptr);
}

TEST_CASE("View::hit_test honors pointerEvents == box-only",
          "[view][hit_test][issue-1026]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    root.add_child(std::move(child));

    // box_only: this view receives events; children do NOT — even when
    // the point lands directly on a child, hit_test returns the parent.
    root.set_pointer_events(View::PointerEvents::box_only);
    REQUIRE(root.hit_test({75, 75}) == &root);
    REQUIRE(root.hit_test({10, 10}) == &root);
}

TEST_CASE("View::hit_test honors pointerEvents == box-none",
          "[view][hit_test][issue-1026]") {
    View root;
    root.set_bounds({0, 0, 200, 200});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    auto* child_ptr = child.get();
    root.add_child(std::move(child));

    // box_none: this view does NOT receive events but children do —
    // the point on the child still resolves to the child, but a point
    // inside the parent's bounds (not on the child) returns nullptr.
    root.set_pointer_events(View::PointerEvents::box_none);
    REQUIRE(root.hit_test({75, 75}) == child_ptr);
    REQUIRE(root.hit_test({10, 10}) == nullptr);
}

// ── pulp #1026: backfaceVisibility plumbing ────────────────────────────────

TEST_CASE("View backface_visible defaults to true and round-trips",
          "[view][issue-1026]") {
    View v;
    REQUIRE(v.backface_visible());
    v.set_backface_visible(false);
    REQUIRE_FALSE(v.backface_visible());
    v.set_backface_visible(true);
    REQUIRE(v.backface_visible());
}

// ── pulp #1026: transform-origin applies to matrix path ────────────────────

TEST_CASE("View::paint_all applies transform-origin around concat_transform",
          "[view][transform][issue-1026]") {
    using namespace pulp::canvas;

    // When the View has a transform-origin offset and a transform-matrix,
    // paint_all should bracket the concat with translate(ox,oy) and
    // translate(-ox,-oy) so rotation/scale anchor at the requested point
    // — same behaviour as the CSS-transform path.
    RecordingCanvas rc;
    View v;
    v.set_bounds({0, 0, 100, 50});
    // Origin (0.25, 0.75): pivot at (25, 37.5).
    v.set_transform_origin(0.25f, 0.75f);
    v.set_transform_matrix(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);

    v.paint_all(rc);

    // Expected sequence around the concat:
    //   translate(0,0)    — bounds_.x/y at (0,0)
    //   translate(25, 37.5)
    //   concat_transform(2,0,0,2,0,0)
    //   translate(-25, -37.5)
    bool saw_pre_origin = false, saw_concat = false, saw_post_origin = false;
    int idx = 0;
    int concat_at = -1;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::concat_transform) {
            concat_at = idx;
            saw_concat = true;
        }
        ++idx;
    }
    REQUIRE(saw_concat);
    REQUIRE(concat_at > 0);

    // Search the immediate neighbours.
    auto& cmds = rc.commands();
    REQUIRE(cmds[(size_t)(concat_at - 1)].type == DrawCommand::Type::translate);
    REQUIRE_THAT(cmds[(size_t)(concat_at - 1)].f[0], WithinAbs(25.0f, 1e-4f));
    REQUIRE_THAT(cmds[(size_t)(concat_at - 1)].f[1], WithinAbs(37.5f, 1e-4f));
    saw_pre_origin = true;

    REQUIRE((size_t)(concat_at + 1) < cmds.size());
    REQUIRE(cmds[(size_t)(concat_at + 1)].type == DrawCommand::Type::translate);
    REQUIRE_THAT(cmds[(size_t)(concat_at + 1)].f[0], WithinAbs(-25.0f, 1e-4f));
    REQUIRE_THAT(cmds[(size_t)(concat_at + 1)].f[1], WithinAbs(-37.5f, 1e-4f));
    saw_post_origin = true;

    REQUIRE(saw_pre_origin);
    REQUIRE(saw_post_origin);
}

TEST_CASE("View::paint_all does NOT bracket concat when origin is unset (back-compat)",
          "[view][transform][issue-1026]") {
    using namespace pulp::canvas;

    // pulp #1026 — only an EXPLICIT setTransformOrigin call activates the
    // pre/post-origin translate bracket on the matrix path. Existing
    // setTransform() call sites (no origin) keep the pre-#1026 single
    // concat, preserving back-compat with the issue-930 contract.
    RecordingCanvas rc;
    View v;
    v.set_bounds({0, 0, 100, 50});
    REQUIRE_FALSE(v.transform_origin_explicit());
    v.set_transform_matrix(2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f);

    v.paint_all(rc);

    int translates = 0;
    int concats = 0;
    for (auto& cmd : rc.commands()) {
        if (cmd.type == DrawCommand::Type::translate) ++translates;
        if (cmd.type == DrawCommand::Type::concat_transform) ++concats;
    }
    // Only the 1 bounds translate; no origin bracket.
    REQUIRE(translates == 1);
    REQUIRE(concats == 1);
}

// ── pulp #1026: per-corner border-radius ──────────────────────────────────

TEST_CASE("View per-corner radii setters flip has_corner_radii",
          "[view][border][issue-1026]") {
    View v;
    REQUIRE_FALSE(v.has_corner_radii());

    v.set_corner_radius_tl(8.0f);
    REQUIRE(v.has_corner_radii());
    REQUIRE_THAT(v.corner_radius_tl(), WithinAbs(8.0f, 1e-5f));
    // pulp #1171 — when a per-corner setter is the FIRST setter (no
    // prior set_border_radius), there's no uniform radius to seed from,
    // so unset corners stay 0.
    REQUIRE_THAT(v.corner_radius_tr(), WithinAbs(0.0f, 1e-5f));

    v.set_corner_radius_tr(12.0f);
    v.set_corner_radius_bl(4.0f);
    v.set_corner_radius_br(2.0f);
    REQUIRE_THAT(v.corner_radius_tr(), WithinAbs(12.0f, 1e-5f));
    REQUIRE_THAT(v.corner_radius_bl(), WithinAbs(4.0f,  1e-5f));
    REQUIRE_THAT(v.corner_radius_br(), WithinAbs(2.0f,  1e-5f));
}

// pulp #1171 (Codex P2 on #1044) — uniform set_border_radius() followed
// by a single per-corner override must NOT zero the other three corners.
// Previously: set_border_radius(10); set_corner_radius_tl(2);
// rendered as {2, 0, 0, 0}, silently discarding the uniform 10.
TEST_CASE("set_border_radius + per-corner override seeds remaining corners",
          "[view][border][issue-1171][codex-p2]") {
    View v;
    v.set_border_radius(10.0f);
    REQUIRE_FALSE(v.has_corner_radii());

    v.set_corner_radius_tl(2.0f);
    REQUIRE(v.has_corner_radii());

    REQUIRE_THAT(v.corner_radius_tl(), WithinAbs(2.0f,  1e-5f));
    REQUIRE_THAT(v.corner_radius_tr(), WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(v.corner_radius_bl(), WithinAbs(10.0f, 1e-5f));
    REQUIRE_THAT(v.corner_radius_br(), WithinAbs(10.0f, 1e-5f));
}

TEST_CASE("set_border_radius + multiple per-corner overrides — promote runs once",
          "[view][border][issue-1171][codex-p2]") {
    View v;
    v.set_border_radius(8.0f);
    v.set_corner_radius_tr(4.0f);
    // After the first per-corner setter, has_corner_radii_ flips true
    // and subsequent setters MUST NOT re-promote (which would clobber
    // the already-overridden TR back to 8). promote_uniform_to_per_corner
    // is guarded on `!has_corner_radii_`.
    v.set_corner_radius_bl(0.0f);
    REQUIRE_THAT(v.corner_radius_tl(), WithinAbs(8.0f, 1e-5f));
    REQUIRE_THAT(v.corner_radius_tr(), WithinAbs(4.0f, 1e-5f));  // not 8
    REQUIRE_THAT(v.corner_radius_bl(), WithinAbs(0.0f, 1e-5f));
    REQUIRE_THAT(v.corner_radius_br(), WithinAbs(8.0f, 1e-5f));
}

TEST_CASE("simulate_click bubble does NOT walk past `this` receiver",
          "[view][simulate_click][issue-1171][codex-p2]") {
    // Build: outer (PARENT) > root (this) > child.
    // outer has on_click set. root + child have no on_click.
    // Calling root->simulate_click(...) MUST NOT bubble up to outer —
    // that would leak the synthetic click across component boundaries
    // (the bug Codex flagged on #1073).
    View outer;
    outer.set_bounds({0, 0, 200, 200});
    bool outer_clicked = false;
    outer.on_click = [&]() { outer_clicked = true; };

    auto root_owned = std::make_unique<View>();
    root_owned->set_bounds({10, 10, 100, 100});
    auto* root = root_owned.get();
    outer.add_child(std::move(root_owned));

    auto child_owned = std::make_unique<View>();
    child_owned->set_bounds({5, 5, 50, 50});
    root->add_child(std::move(child_owned));

    // Click at root-local {30,30} which is inside child's bounds.
    // hit_test resolves to child (no on_click). Bubble walks up to
    // root — and STOPS there per the fix (root == this). Without the
    // fix it would walk past root to outer and fire outer.on_click.
    root->simulate_click({30, 30});
    REQUIRE_FALSE(outer_clicked);
}

TEST_CASE("View::paint_all routes background through path API when per-corner radii set",
          "[view][border][issue-1026]") {
    using namespace pulp::canvas;

    // With a uniform corner_radius the bg uses fill_rounded_rect; with
    // any per-corner setter the path API takes over (begin_path /
    // line_to / cubic_to / close_path / fill_current_path).
    {
        RecordingCanvas rc;
        View v;
        v.set_bounds({0, 0, 100, 50});
        v.set_background_color(Color::rgba8(255, 0, 0));
        v.set_border(Color::rgba8(0, 0, 0), 1.0f, /*radius=*/8.0f);
        v.paint_all(rc);
        REQUIRE(rc.count(DrawCommand::Type::fill_rounded_rect) >= 1);
        REQUIRE(rc.count(DrawCommand::Type::stroke_rounded_rect) >= 1);
    }
    {
        RecordingCanvas rc;
        View v;
        v.set_bounds({0, 0, 100, 50});
        v.set_background_color(Color::rgba8(255, 0, 0));
        v.set_border(Color::rgba8(0, 0, 0), 1.0f);
        v.set_corner_radius_tl(8.0f);
        v.set_corner_radius_tr(8.0f);
        v.set_corner_radius_bl(0.0f);
        v.set_corner_radius_br(0.0f);
        v.paint_all(rc);
        // Per-corner path: at least begin_path + close_path appear, and
        // fill_rounded_rect / stroke_rounded_rect do NOT.
        REQUIRE(rc.count(DrawCommand::Type::fill_rounded_rect) == 0);
        REQUIRE(rc.count(DrawCommand::Type::stroke_rounded_rect) == 0);
        REQUIRE(rc.count(DrawCommand::Type::begin_path) >= 1);
        REQUIRE(rc.count(DrawCommand::Type::close_path) >= 1);
    }
}

// ═══════════════════════════════════════════════════════════════════
// pulp #1321: window resize triggers root View Yoga relayout
// ═══════════════════════════════════════════════════════════════════
//
// The macOS standalone window plumbs an NSWindow resize through to
// PulpView::setFrameSize: and the PulpWindowDelegate's windowDidResize:
// notification, both of which now call root.set_bounds(...) +
// root.layout_children(). This Catch2 test exercises the same model
// path that those AppKit hooks invoke, asserting that descendants
// reflow when the root is given a new size.

TEST_CASE("Window resize reflows Yoga layout (pulp #1321)",
          "[view][layout][issue-1321]") {
    View root;
    root.flex().direction = FlexDirection::row;

    auto child_a = std::make_unique<View>();
    child_a->flex().flex_grow = 1.0f;
    auto* child_a_ptr = child_a.get();
    root.add_child(std::move(child_a));

    auto child_b = std::make_unique<View>();
    child_b->flex().flex_grow = 1.0f;
    auto* child_b_ptr = child_b.get();
    root.add_child(std::move(child_b));

    // ── Initial layout at 1320×892 (Spectr default standalone size) ──
    root.set_bounds({0, 0, 1320, 892});
    root.layout_children();

    REQUIRE(child_a_ptr->bounds().width == Catch::Approx(660.0f));
    REQUIRE(child_b_ptr->bounds().width == Catch::Approx(660.0f));
    REQUIRE(child_a_ptr->bounds().height == Catch::Approx(892.0f));

    // ── Simulate the user dragging the window down to 800×500 ────────
    // (matches the osascript repro in the issue body).
    root.set_bounds({0, 0, 800, 500});
    root.layout_children();

    // Each flex:1 child must now occupy half of the new width and the
    // full new height — i.e. Yoga reflowed against the new constraints
    // rather than reusing the original 1320×892 root size.
    REQUIRE(child_a_ptr->bounds().width == Catch::Approx(400.0f));
    REQUIRE(child_b_ptr->bounds().width == Catch::Approx(400.0f));
    REQUIRE(child_a_ptr->bounds().height == Catch::Approx(500.0f));
    REQUIRE(child_b_ptr->bounds().height == Catch::Approx(500.0f));

    // ── Resize back up — content reflows again, no hysteresis ────────
    root.set_bounds({0, 0, 1600, 1000});
    root.layout_children();
    REQUIRE(child_a_ptr->bounds().width == Catch::Approx(800.0f));
    REQUIRE(child_b_ptr->bounds().width == Catch::Approx(800.0f));
    REQUIRE(child_a_ptr->bounds().height == Catch::Approx(1000.0f));
}

TEST_CASE("Window resize honors a fixed-size sibling next to a flex child "
          "(pulp #1321)",
          "[view][layout][issue-1321]") {
    using namespace pulp::view;

    // Mirrors a Spectr-style chrome layout: a stretchy content area
    // followed by a fixed-height bottom toolbar. The toolbar must keep
    // its 48px height regardless of the window's new height, and the
    // content area must absorb the rest.
    View root;
    root.flex().direction = FlexDirection::column;

    auto content = std::make_unique<View>();
    content->flex().flex_grow = 1.0f;
    auto* content_ptr = content.get();
    root.add_child(std::move(content));

    auto toolbar = std::make_unique<View>();
    toolbar->flex().preferred_height = 48.0f;
    auto* toolbar_ptr = toolbar.get();
    root.add_child(std::move(toolbar));

    root.set_bounds({0, 0, 1320, 892});
    root.layout_children();
    REQUIRE(toolbar_ptr->bounds().height == Catch::Approx(48.0f));
    REQUIRE(content_ptr->bounds().height == Catch::Approx(844.0f));

    // Shrink — toolbar still 48px, content absorbs the loss.
    root.set_bounds({0, 0, 800, 500});
    root.layout_children();
    REQUIRE(toolbar_ptr->bounds().height == Catch::Approx(48.0f));
    REQUIRE(content_ptr->bounds().height == Catch::Approx(452.0f));
    REQUIRE(toolbar_ptr->bounds().width == Catch::Approx(800.0f));

    // Grow — same invariants hold.
    root.set_bounds({0, 0, 1600, 1000});
    root.layout_children();
    REQUIRE(toolbar_ptr->bounds().height == Catch::Approx(48.0f));
    REQUIRE(content_ptr->bounds().height == Catch::Approx(952.0f));
}

// ── pulp #1148 slice (a) — symmetric overflow:visible hit-test extension ──
//
// PR #1297 added overflow:visible hit-test extension so that absolutely-
// positioned popovers / dropdowns whose content escapes their bounds box
// still receive clicks. The original implementation only extended the box
// 500px DOWNWARD, which broke left-extending popovers (e.g. Spectr's bands
// picker that opens to the left of its trigger). This suite locks the
// extension to ±500px on all four sides.
//
// Each direction places a "popover" grandchild positioned outside its
// overflow:visible parent in the tested direction — overflow:visible
// passes the click through to the parent's hit_test recursion, which
// then matches the grandchild's local_bounds. Without symmetric slack,
// the parent rejects the click before recursion happens for left/right/up.
namespace {
struct PopoverFixture {
    View root;
    View* container{nullptr};   // overflow:visible host
    View* popover{nullptr};     // grandchild positioned outside container

    // dx/dy are the popover's offset *relative to the container's local
    // origin* — negative values escape the container in the −x/−y direction.
    PopoverFixture(float dx, float dy) {
        root.set_bounds({0, 0, 2000, 2000});
        auto c = std::make_unique<View>();
        c->set_bounds({600, 600, 100, 100});
        c->set_overflow(View::Overflow::visible);
        container = c.get();

        auto p = std::make_unique<View>();
        // 50x50 popover anchored at (dx, dy) inside container's local space.
        p->set_bounds({dx, dy, 50, 50});
        popover = p.get();
        c->add_child(std::move(p));
        root.add_child(std::move(c));
    }
};
} // namespace

TEST_CASE("View::hit_test extends overflow:visible 500px to the LEFT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (-200, 25) → root-space (400..450, 625..675).
    // 100px to the LEFT of container.x=600 covers root.x = 425.
    PopoverFixture f(-200, 25);
    REQUIRE(f.root.hit_test({425, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px to the RIGHT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (200, 25) → root-space (800..850, 625..675).
    // 100px to the RIGHT of container.right=700 covers root.x = 825.
    PopoverFixture f(200, 25);
    REQUIRE(f.root.hit_test({825, 650}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px UPWARD",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (25, -200) → root-space (625..675, 400..450).
    // 100px ABOVE container.y=600 covers root.y = 425.
    PopoverFixture f(25, -200);
    REQUIRE(f.root.hit_test({650, 425}) == f.popover);
}

TEST_CASE("View::hit_test extends overflow:visible 500px DOWNWARD",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover at container-local (25, 200) → root-space (625..675, 800..850).
    // 100px BELOW container.bottom=700 covers root.y = 825.
    // This direction was already supported pre-#1148 — guards regression.
    PopoverFixture f(25, 200);
    REQUIRE(f.root.hit_test({650, 825}) == f.popover);
}

TEST_CASE("View::hit_test does NOT extend overflow:visible past 500px LEFT",
          "[view][hit_test][issue-1148][overflow-symmetric]") {
    // Popover anchored 600px LEFT of container — outside the symmetric
    // ±500px slack, so the click must miss the popover entirely. With
    // container x=600, popover at container-local x=-650 lands at
    // root.x = -50..0; we probe root.x = 0 → container-local x = -600,
    // beyond the -500 slack.
    PopoverFixture f(-650, 25);
    REQUIRE(f.root.hit_test({0, 650}) != f.popover);
}

// ── pulp #1368 round 2 — absolute children with inset:0 + explicit height ──
//
// Spectr filterbank repro per round-2 investigation. Two `<canvas>` siblings
// share the same wrap parent (1320×860) with `position: absolute; inset: 0`
// (top/right/bottom/left = 0), but each has an explicit setFlex height —
// pr_1 = 760, pr_2 = 860. The visible-rendering symptom on the live plugin
// is consistent with pr_1's bounds_.y landing above the visible window
// region, which would push every fillRect into the title-bar area via the
// parent's translate(bounds_.x, bounds_.y) in View::paint_all.
//
// These tests pin the contract: when `position: absolute; inset: 0` is
// combined with an explicit height that is SHORTER than the parent, the
// child's resolved bounds_ must place its origin at (0, 0) — i.e. inset:0
// wins over the explicit height for positioning. If a future Yoga / layout
// change instead resolves y as `parent.height - explicit_height` (anchoring
// the box to the bottom because inset top:0 + bottom:0 conflicts with the
// explicit height), the Spectr symptom reappears.
//
// As of v0.74.0 this test is expected to FAIL when the resolved y is
// non-zero — the FAIL is the round-2 reproduction.

TEST_CASE("absolute child with inset:0 + explicit height resolves to (0,0)",
          "[view][issue-1368][round2]") {
    using P = View::Position;

    View wrap;
    wrap.set_bounds({0, 0, 1320, 860});

    auto pr1 = std::make_unique<View>();
    pr1->set_id("pr_1");
    pr1->set_position(P::absolute);
    pr1->set_top(0);
    pr1->set_right(0);
    pr1->set_bottom(0);
    pr1->set_left(0);
    pr1->flex().preferred_height = 760.0f;
    auto* pr1_ptr = pr1.get();

    auto pr2 = std::make_unique<View>();
    pr2->set_id("pr_2");
    pr2->set_position(P::absolute);
    pr2->set_top(0);
    pr2->set_right(0);
    pr2->set_bottom(0);
    pr2->set_left(0);
    pr2->flex().preferred_height = 860.0f;
    auto* pr2_ptr = pr2.get();

    wrap.add_child(std::move(pr1));
    wrap.add_child(std::move(pr2));

    wrap.layout_children();

    INFO("pr_1 bounds=(" << pr1_ptr->bounds().x << "," << pr1_ptr->bounds().y
         << "," << pr1_ptr->bounds().width << "," << pr1_ptr->bounds().height << ")");
    INFO("pr_2 bounds=(" << pr2_ptr->bounds().x << "," << pr2_ptr->bounds().y
         << "," << pr2_ptr->bounds().width << "," << pr2_ptr->bounds().height << ")");

    // Both children must paint flush with the parent's top-left. Negative or
    // shifted y values reproduce the #1368 round-2 hypothesis: parent's
    // translate(bounds_.x, bounds_.y) in View::paint_all pushes the canvas
    // child off-window so fillRect at (50, 50) lands in the title-bar region.
    REQUIRE(pr1_ptr->bounds().x == 0.0f);
    REQUIRE(pr1_ptr->bounds().y == 0.0f);
    REQUIRE(pr2_ptr->bounds().x == 0.0f);
    REQUIRE(pr2_ptr->bounds().y == 0.0f);
}

TEST_CASE("absolute child with inset:0 fills parent ignoring explicit height",
          "[view][issue-1368][round2]") {
    // CSS spec: when both top:0 and bottom:0 are set on a position:absolute
    // box, the box must stretch to fill the parent height — explicit
    // `height: 760` is overconstrained and the spec says inset wins (the
    // box gets parent.height = 860). Confirm Pulp/Yoga match that contract.
    View wrap;
    wrap.set_bounds({0, 0, 1320, 860});

    auto pr1 = std::make_unique<View>();
    pr1->set_position(View::Position::absolute);
    pr1->set_top(0);
    pr1->set_right(0);
    pr1->set_bottom(0);
    pr1->set_left(0);
    pr1->flex().preferred_height = 760.0f;
    auto* pr1_ptr = pr1.get();

    wrap.add_child(std::move(pr1));
    wrap.layout_children();

    INFO("pr_1 bounds=(" << pr1_ptr->bounds().x << "," << pr1_ptr->bounds().y
         << "," << pr1_ptr->bounds().width << "," << pr1_ptr->bounds().height << ")");

    REQUIRE(pr1_ptr->bounds().y == 0.0f);
    // Width should match the parent — inset left:0 + right:0 fills horizontally.
    REQUIRE(pr1_ptr->bounds().width == 1320.0f);
}

// ── pulp #1737 / #1515 — CSS mask-image + mask-size paint slice ─────────
//
// Phase 1 ships linear-gradient mask shapes through the new
// Canvas::save_layer_with_mask virtual + SkiaCanvas's 2-saveLayer +
// kDstIn composite at restore() time. RecordingCanvas spy captures
// the API dispatch so we can pin the wiring without depending on Skia
// (which is the only backend that actually composites the mask alpha).
// Visual output is verified separately against the SkiaCanvas raster
// path in the [skia] tests below.

namespace {
struct MaskSpyCanvas : pulp::canvas::RecordingCanvas {
    struct MaskCall {
        float x, y, w, h, opacity;
        std::string mask_image;
        std::string mask_size;
    };
    std::vector<MaskCall> mask_calls;

    void save_layer_with_mask(float x, float y, float w, float h,
                               float opacity,
                               const std::string& mask_image,
                               const std::string& mask_size) override {
        mask_calls.push_back({x, y, w, h, opacity, mask_image, mask_size});
        // Don't fall through to save_layer — we only want to spy the
        // mask call, not double-record a save_layer command.
    }
};
}

TEST_CASE("View::paint_all routes through save_layer_with_mask when mask-image is set",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_mask_image("linear-gradient(to bottom, black, transparent)");
    v.set_mask_size("100% 100%");

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    REQUIRE(canvas.mask_calls.size() == 1);
    auto& m = canvas.mask_calls[0];
    REQUIRE(m.x == 0);
    REQUIRE(m.y == 0);
    REQUIRE(m.w == 100);
    REQUIRE(m.h == 50);
    REQUIRE(m.mask_image == "linear-gradient(to bottom, black, transparent)");
    REQUIRE(m.mask_size == "100% 100%");
}

TEST_CASE("View::paint_all does NOT route through save_layer_with_mask when mask-image is empty",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    // No mask_image set — paint_all should use the legacy path
    // (save_layer or just paint without a layer).

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    REQUIRE(canvas.mask_calls.empty());
}

TEST_CASE("View::paint_all does NOT route through save_layer_with_mask when mask-image is 'none'",
          "[view][issue-1515][issue-1737]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_mask_image("none");

    MaskSpyCanvas canvas;
    v.paint_all(canvas);

    // CSS `mask-image: none` is an explicit no-mask declaration —
    // treated as if no mask were set (no layer overhead, no composite).
    REQUIRE(canvas.mask_calls.empty());
}

// ── pulp #1731 — Panel & View paint must honor effective_corner_radius ─────
// Codex P1 #1: Panel had its own corner_radius_ field that paint() never
// read (it called View::effective_corner_radius which only sees View's
// slot). Codex P1 #2: View::paint_all used corner_radius_/corner_radii_[]
// directly, silently ignoring corner_radius_pct_ / corner_radii_pct_[].

TEST_CASE("pulp #1731 P1 — Panel default 8px corner radius actually paints",
          "[view][issue-1731][panel-radius]") {
    pulp::view::Panel p;
    p.set_bounds({0, 0, 100, 50});

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    // Panel always paints a filled background, so we expect at least one
    // fill_rounded_rect with the default 8.0f radius.
    bool saw_default = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 8.0f) < 1e-4f) {
            saw_default = true;
            break;
        }
    }
    REQUIRE(saw_default);
}

TEST_CASE("pulp #1731 P1 — Panel::set_corner_radius writes the View slot paint reads",
          "[view][issue-1731][panel-radius]") {
    pulp::view::Panel p;
    p.set_bounds({0, 0, 100, 50});
    p.set_corner_radius(16.0f);

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    bool saw_16 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 16.0f) < 1e-4f) {
            saw_16 = true;
            break;
        }
    }
    REQUIRE(saw_16);
    REQUIRE_THAT(p.corner_radius(), WithinAbs(16.0f, 1e-4f));
}

TEST_CASE("pulp #1731 P1 — View::paint_all honors percent uniform border-radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 100});
    v.set_background_color(pulp::canvas::Color::rgba8(10, 20, 30));
    v.set_border_radius_pct(50.0f);  // 50% of min(100,100) = 50px → circle

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_50 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 50.0f) < 1e-4f) {
            saw_50 = true;
            break;
        }
    }
    REQUIRE(saw_50);
}

TEST_CASE("pulp #1731 P1 — View border stroke honors percent radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 80, 80});
    v.set_border(pulp::canvas::Color::rgba8(255, 255, 255), 2.0f);
    v.set_border_radius_pct(50.0f);  // 50% of min(80,80) = 40px

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_40 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rounded_rect
                && std::abs(cmd.f[4] - 40.0f) < 1e-4f) {
            saw_40 = true;
            break;
        }
    }
    REQUIRE(saw_40);
}

TEST_CASE("pulp #1731 P1 — View outline honors percent radius",
          "[view][issue-1731][percent-radius]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 60, 60});
    v.set_border_radius_pct(50.0f);  // 50% of min(60,60) = 30px
    v.set_outline_width(2.0f);
    v.set_outline_color(pulp::canvas::Color::rgba8(255, 0, 0));
    v.set_outline_style(pulp::view::View::BorderStyle::solid);
    // Outline uses outline_offset + outline_width * 0.5f = 0 + 1 = 1 inflate.
    // Expected stroked radius = 30 + 1 = 31.

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_31 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::stroke_rounded_rect
                && std::abs(cmd.f[4] - 31.0f) < 1e-4f) {
            saw_31 = true;
            break;
        }
    }
    REQUIRE(saw_31);
}

TEST_CASE("pulp #1731 P1 — px radius still wins when pct is 0 (default)",
          "[view][issue-1731][regression-guard]") {
    // Make sure the percent-resolving paint path doesn't silently regress
    // the px-only case for views that never set a percent.
    pulp::view::View v;
    v.set_bounds({0, 0, 200, 200});
    v.set_background_color(pulp::canvas::Color::rgba8(0, 0, 0));
    v.set_border_radius(12.0f);

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    bool saw_12 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 12.0f) < 1e-4f) {
            saw_12 = true;
            break;
        }
    }
    REQUIRE(saw_12);
}

// ─────────────────────────────────────────────────────────────────────────────
// pulp #1731 codecov backfill — pin the effective_corner_radius* helper math
// and the per-corner percent paint integration. Pre-existing tests covered the
// uniform-pct paint sites; these add direct helper-level assertions plus
// per-corner % paint, box-shadow %, and bounds-coupling (resolved-px changes
// when min(width,height) changes) which Codecov flagged at 9.37% on #1731.
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pulp #1731 cov — effective_corner_radius uniform pct math",
          "[issue-1731][rn][borderRadius][coverage]") {
    pulp::view::View v;

    SECTION("50% on 100x60 → 30 (50% × min(100,60))") {
        v.set_bounds({0, 0, 100, 60});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
    }
    SECTION("50% on 60x100 → 30 (different min dimension, same result)") {
        v.set_bounds({0, 0, 60, 100});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(60.0f, 100.0f),
                     WithinAbs(30.0f, 1e-4f));
    }
    SECTION("50% on 200x200 → 100 (square: half the side)") {
        v.set_bounds({0, 0, 200, 200});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.effective_corner_radius(200.0f, 200.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("25% on 100x100 → 25") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(25.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(25.0f, 1e-4f));
    }
    SECTION("0% → 0 (falls through to px slot which is 0 by default)") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(0.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(0.0f, 1e-4f));
    }
    SECTION("100% on 100x100 → 100 (full min dimension; clamped to half by "
            "paint-time path builder, but helper returns raw resolved px)") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(100.0f);
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("set_border_radius(px) after set_border_radius_pct(pct) "
            "clears the pct slot and returns the new px value") {
        v.set_bounds({0, 0, 100, 100});
        v.set_border_radius_pct(50.0f);
        REQUIRE_THAT(v.corner_radius_pct(), WithinAbs(50.0f, 1e-4f));
        v.set_border_radius(8.0f);
        REQUIRE_THAT(v.corner_radius_pct(), WithinAbs(0.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius(100.0f, 100.0f),
                     WithinAbs(8.0f, 1e-4f));
    }
}

TEST_CASE("pulp #1731 cov — effective_corner_radius_* per-corner pct math",
          "[issue-1731][rn][borderRadius][coverage]") {
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 60});

    SECTION("each per-corner pct resolves against min(w,h) independently") {
        // min(100, 60) = 60 — every per-corner % multiplies that.
        v.set_corner_radius_tl_pct(50.0f);  // 30
        v.set_corner_radius_tr_pct(25.0f);  // 15
        v.set_corner_radius_bl_pct(10.0f);  // 6
        v.set_corner_radius_br_pct(75.0f);  // 45

        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(15.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_bl(100.0f, 60.0f),
                     WithinAbs(6.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_br(100.0f, 60.0f),
                     WithinAbs(45.0f, 1e-4f));
    }
    SECTION("per-corner px wins when its pct slot is 0") {
        v.set_corner_radius_tl(10.0f);
        v.set_corner_radius_tr(20.0f);
        v.set_corner_radius_bl(30.0f);
        v.set_corner_radius_br(40.0f);

        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(10.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(20.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_bl(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_br(100.0f, 60.0f),
                     WithinAbs(40.0f, 1e-4f));
    }
    SECTION("mixed px and pct: only the pct corner reflows on bounds change") {
        v.set_corner_radius_tl(8.0f);          // px — fixed
        v.set_corner_radius_tr_pct(50.0f);     // pct — reflows
        // 100x60 → tl=8, tr=30 (50% × 60)
        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(8.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(100.0f, 60.0f),
                     WithinAbs(30.0f, 1e-4f));
        // 200x200 → tl=8 (unchanged), tr=100 (50% × 200)
        REQUIRE_THAT(v.effective_corner_radius_tl(200.0f, 200.0f),
                     WithinAbs(8.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tr(200.0f, 200.0f),
                     WithinAbs(100.0f, 1e-4f));
    }
    SECTION("set_corner_radius_tl(px) after pct clears the pct slot") {
        v.set_corner_radius_tl_pct(50.0f);
        REQUIRE_THAT(v.corner_radius_tl_pct(), WithinAbs(50.0f, 1e-4f));
        v.set_corner_radius_tl(7.0f);
        REQUIRE_THAT(v.corner_radius_tl_pct(), WithinAbs(0.0f, 1e-4f));
        REQUIRE_THAT(v.effective_corner_radius_tl(100.0f, 60.0f),
                     WithinAbs(7.0f, 1e-4f));
    }
}

TEST_CASE("pulp #1731 cov — uniform percent reflows when bounds change",
          "[issue-1731][rn][borderRadius][coverage]") {
    // Same View, two different bounds — pct stays the same but resolved
    // px differs because min(w,h) differs.
    pulp::view::View v;
    v.set_border_radius_pct(50.0f);

    v.set_bounds({0, 0, 100, 60});
    REQUIRE_THAT(v.effective_corner_radius(100.0f, 60.0f),
                 WithinAbs(30.0f, 1e-4f));

    v.set_bounds({0, 0, 60, 100});
    REQUIRE_THAT(v.effective_corner_radius(60.0f, 100.0f),
                 WithinAbs(30.0f, 1e-4f));

    v.set_bounds({0, 0, 200, 200});
    REQUIRE_THAT(v.effective_corner_radius(200.0f, 200.0f),
                 WithinAbs(100.0f, 1e-4f));
}

TEST_CASE("pulp #1731 cov — per-corner pct paints as cubic_to with resolved px",
          "[issue-1731][rn][borderRadius][coverage]") {
    // Per-corner percent forces View::paint_all into the per-corner path
    // builder (build_per_corner_rounded_rect_path), which emits cubic_to
    // commands whose end-points encode each effective radius.
    //
    // From build_per_corner_rounded_rect_path (core/view/src/view.cpp):
    //   TR corner cubic_to ends at (w, tr)
    //
    // 100x60 view, TR = 25% → eff_tr = 0.25 * min(100,60) = 15.
    // So we expect a cubic_to whose final point is (100, 15).
    pulp::view::View v;
    v.set_bounds({0, 0, 100, 60});
    v.set_background_color(pulp::canvas::Color::rgba8(10, 20, 30));
    v.set_corner_radius_tr_pct(25.0f);  // 15 px on 100x60

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    // Verify the per-corner path was selected (presence of begin_path +
    // fill_current_path rather than fill_rounded_rect for the bg).
    bool saw_fill_current_path = false;
    bool saw_tr_cubic_end = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_current_path) {
            saw_fill_current_path = true;
        }
        if (cmd.type == pulp::canvas::DrawCommand::Type::cubic_to) {
            // cubic_to packs (cp1x, cp1y, cp2x, cp2y, x, y) in f[0..5].
            // The TR-corner cubic ends at (w, tr) = (100, 15).
            const float ex = cmd.f[4];
            const float ey = cmd.f[5];
            if (std::abs(ex - 100.0f) < 1e-3f
                    && std::abs(ey - 15.0f) < 1e-3f) {
                saw_tr_cubic_end = true;
            }
        }
    }
    REQUIRE(saw_fill_current_path);
    REQUIRE(saw_tr_cubic_end);
}

TEST_CASE("pulp #1731 cov — box-shadow path reads the percent-resolved radius",
          "[issue-1731][rn][borderRadius][coverage]") {
    // View::paint_all draws outset box-shadow before the background,
    // and the shadow's rounded-rect corner is computed from
    // effective_corner_radius(...). A 50% pct on a 200x200 view should
    // produce a draw_box_shadow whose corner-radius slot (f[+]) equals
    // 100 (50% × 200).
    pulp::view::View v;
    v.set_bounds({0, 0, 200, 200});
    v.set_background_color(pulp::canvas::Color::rgba8(0, 0, 0));
    v.set_border_radius_pct(50.0f);  // 100 px on 200x200

    v.set_box_shadow(0.0f, 4.0f, 8.0f, 0.0f,
                     pulp::canvas::Color::rgba8(0, 0, 0, 128),
                     /*inset=*/false);

    pulp::canvas::RecordingCanvas rc;
    v.paint_all(rc);

    // draw_box_shadow packs (x,y,w,h,blur,spread...) starting at f[0..]
    // and stores the rounded-corner radius as a trailing param. The
    // call site is view.cpp:300 — `draw_box_shadow(0, 0, w, h, ox, oy,
    // blur, spread, color, inset, eff_r)`. RecordingCanvas captures
    // the radius in `floats` (the variable-length payload).
    bool saw_shadow_with_pct_radius = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type != pulp::canvas::DrawCommand::Type::draw_box_shadow)
            continue;
        // The radius value 100 should appear in the command's stored
        // params (either f[] or floats[]). Scan both.
        for (float val : cmd.f) {
            if (std::abs(val - 100.0f) < 1e-3f) {
                saw_shadow_with_pct_radius = true;
                break;
            }
        }
        if (!saw_shadow_with_pct_radius) {
            for (float val : cmd.floats) {
                if (std::abs(val - 100.0f) < 1e-3f) {
                    saw_shadow_with_pct_radius = true;
                    break;
                }
            }
        }
        if (saw_shadow_with_pct_radius) break;
    }
    REQUIRE(saw_shadow_with_pct_radius);
}

TEST_CASE("pulp #1731 cov — Panel::paint reads pct via effective_corner_radius",
          "[issue-1731][rn][borderRadius][coverage]") {
    // widgets.cpp:1774 — Panel reads effective_corner_radius for its
    // own fill_rounded_rect. Pin the pct path explicitly.
    pulp::view::Panel p;
    p.set_bounds({0, 0, 120, 80});
    // 25% × min(120, 80) = 20
    p.set_border_radius_pct(25.0f);

    pulp::canvas::RecordingCanvas rc;
    p.paint(rc);

    bool saw_20 = false;
    for (const auto& cmd : rc.commands()) {
        if (cmd.type == pulp::canvas::DrawCommand::Type::fill_rounded_rect
                && std::abs(cmd.f[4] - 20.0f) < 1e-4f) {
            saw_20 = true;
            break;
        }
    }
    REQUIRE(saw_20);
}
