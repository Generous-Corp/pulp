#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/live_constant_editor.hpp>
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

TEST_CASE("LiveConstantRegistry registers reuses clamps and resets constants",
          "[view][live-constant][coverage][phase3]") {
    auto& registry = LiveConstantRegistry::instance();
    registry.on_change = {};

    auto& gain = registry.register_constant(
        "phase3.live.gain", "test_view.cpp", 101, 0.25f, 0.0f, 1.0f);
    REQUIRE(gain == Catch::Approx(0.25f));

    auto& same_gain = registry.register_constant(
        "phase3.live.gain", "other.cpp", 202, 0.75f, -1.0f, 2.0f);
    REQUIRE(&same_gain == &gain);
    REQUIRE(same_gain == Catch::Approx(0.25f));

    int change_count = 0;
    LiveConstant last;
    registry.on_change = [&](const LiveConstant& c) {
        ++change_count;
        last = c;
    };

    registry.set("phase3.live.gain", 2.0f);
    REQUIRE(registry.get("phase3.live.gain") == Catch::Approx(1.0f));
    REQUIRE(change_count == 1);
    REQUIRE(last.name == "phase3.live.gain");
    REQUIRE(last.value == Catch::Approx(1.0f));

    registry.set("phase3.live.gain", -2.0f);
    REQUIRE(registry.get("phase3.live.gain") == Catch::Approx(0.0f));
    REQUIRE(change_count == 2);

    registry.reset("phase3.live.gain");
    REQUIRE(registry.get("phase3.live.gain") == Catch::Approx(0.25f));

    registry.set("phase3.live.gain", 0.75f);
    registry.reset_all();
    REQUIRE(registry.get("phase3.live.gain") == Catch::Approx(0.25f));

    registry.reset("phase3.live.missing");
    registry.set("phase3.live.missing", 0.5f);
    REQUIRE(registry.get("phase3.live.missing") == Catch::Approx(0.0f));

    registry.on_change = {};
}

TEST_CASE("LiveConstantEditor toggles visibility and ignores header drags",
          "[view][live-constant][coverage][phase3]") {
    auto& registry = LiveConstantRegistry::instance();
    registry.register_constant(
        "phase3.live.editor", "test_view.cpp", 303, 5.0f, 0.0f, 10.0f);

    LiveConstantEditor editor;
    REQUIRE_FALSE(editor.is_showing());
    editor.toggle();
    REQUIRE(editor.is_showing());
    editor.toggle();
    REQUIRE_FALSE(editor.is_showing());

    editor.set_bounds({0, 0, 200, 100});
    editor.set_row_height(20.0f);
    editor.on_mouse_down({80.0f, 10.0f});
    editor.on_mouse_drag({200.0f, 40.0f});
    REQUIRE(registry.get("phase3.live.editor") == Catch::Approx(5.0f));
}

// Phase 3d (inspector roadmap) — per-component paint timing.
// View::paint_all() now records nanoseconds spent inside paint() (self)
// and the recursive children walk (with_children) so the inspector can
// show per-view cost without adding new instrumentation per query.

// A View subclass whose paint() override does a fixed amount of work
// so we can assert non-zero timing.
namespace {
class CountingPaintView : public pulp::view::View {
public:
    void paint(pulp::canvas::Canvas&) override {
        // A small CPU-bound loop so the steady_clock delta is non-zero
        // even on fast hardware. ~10k iterations is far below any frame
        // budget but well above the steady_clock resolution.
        volatile std::uint32_t acc = 0;
        for (int i = 0; i < 10000; ++i) acc += static_cast<std::uint32_t>(i);
        last_acc_ = acc;
    }
    std::uint32_t last_acc_ = 0;
};
}  // namespace

TEST_CASE("View::last_paint_self_ns reports non-zero after a paint cycle",
          "[view][paint-timing][phase3d]") {
    auto v = std::make_unique<CountingPaintView>();
    v->set_bounds({0, 0, 100, 100});

    REQUIRE(v->last_paint_self_ns() == 0u);
    REQUIRE(v->last_paint_with_children_ns() == 0u);

    pulp::canvas::RecordingCanvas canvas;
    v->paint_all(canvas);

    // With work in paint(), both timers must record > 0.
    REQUIRE(v->last_paint_self_ns() > 0u);
    REQUIRE(v->last_paint_with_children_ns() >= v->last_paint_self_ns());
}

TEST_CASE("View::last_paint_with_children_ns covers child cost too",
          "[view][paint-timing][phase3d]") {
    // Parent with two CountingPaintView children — parent's
    // with_children must be >= sum of children's self timings.
    auto parent = std::make_unique<pulp::view::View>();
    parent->set_bounds({0, 0, 200, 200});

    auto child_a = std::make_unique<CountingPaintView>();
    child_a->set_bounds({0, 0, 100, 100});
    auto* a_ptr = child_a.get();
    parent->add_child(std::move(child_a));

    auto child_b = std::make_unique<CountingPaintView>();
    child_b->set_bounds({0, 100, 100, 100});
    auto* b_ptr = child_b.get();
    parent->add_child(std::move(child_b));

    pulp::canvas::RecordingCanvas canvas;
    parent->paint_all(canvas);

    // Parent's own paint() is the no-op base class — self timing is
    // effectively zero, but it's allowed to be a tiny non-zero blip.
    // The key invariant: with_children covers the child timing chain.
    REQUIRE(parent->last_paint_with_children_ns() > 0u);
    REQUIRE(parent->last_paint_with_children_ns() >=
            (a_ptr->last_paint_self_ns() + b_ptr->last_paint_self_ns()));

    // And every child must have recorded its own self time too.
    REQUIRE(a_ptr->last_paint_self_ns() > 0u);
    REQUIRE(b_ptr->last_paint_self_ns() > 0u);
}

TEST_CASE("View::last_paint_*_ns updates on every paint cycle",
          "[view][paint-timing][phase3d]") {
    // Two paint cycles should produce two recorded samples (the second
    // overwrites the first — we don't keep history).
    auto v = std::make_unique<CountingPaintView>();
    v->set_bounds({0, 0, 100, 100});

    pulp::canvas::RecordingCanvas canvas;
    v->paint_all(canvas);
    auto first = v->last_paint_self_ns();
    REQUIRE(first > 0u);

    v->paint_all(canvas);
    // Second sample exists and is non-zero. Exact equality with first
    // is not asserted because steady_clock jitter on fast hardware can
    // produce different values frame-to-frame.
    REQUIRE(v->last_paint_self_ns() > 0u);
}

// Codex P2 follow-up on #2338: paint timing must cover framework-owned
// drawing (background fills, borders, gradients, layer setup) — not
// just the paint(canvas) override. A styled View with no paint()
// override should still report non-zero last_paint_self_ns because
// background_color painting + layer setup runs inside paint_all().
TEST_CASE("View::last_paint_self_ns covers framework drawing on no-override views "
          "(codex P2 #2338 regression)",
          "[view][paint-timing][phase3d][regression]") {
    // Use the BASE pulp::view::View (no paint() override) but set a
    // background color so the framework's background fill runs in
    // paint_all() between save() and the (no-op) paint() call. The
    // pre-fix implementation timed only the paint() override and
    // reported ~0ns; the post-fix implementation times the whole
    // paint_all body, so we should see > 0ns even for a no-override
    // styled View.
    auto v = std::make_unique<pulp::view::View>();
    v->set_bounds({0, 0, 200, 200});
    v->set_background_color(pulp::canvas::Color::rgba(0.5f, 0.3f, 0.7f, 1.0f));

    pulp::canvas::RecordingCanvas canvas;
    v->paint_all(canvas);

    // The whole paint_all body has run on the way out — self_ns must
    // be > 0 even though paint() was a no-op. Pre-fix this was ~0.
    REQUIRE(v->last_paint_self_ns() > 0u);
    REQUIRE(v->last_paint_with_children_ns() >= v->last_paint_self_ns());
}
