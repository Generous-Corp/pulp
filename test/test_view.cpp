#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
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
    void repaint() override {}
    void set_close_callback(std::function<void()>) override {}
    void run_event_loop() override {}
};

class DummyPluginViewHost final : public PluginViewHost {
public:
    NativeViewHandle native_handle() override { return nullptr; }
    void attach_to_parent(NativeViewHandle) override {}
    void detach() override {}
    void repaint() override {}
    void set_size(uint32_t width, uint32_t height) override { size_ = {width, height}; }
    Size get_size() const override { return size_; }

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
