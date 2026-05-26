// ComponentDragger drag-to-move helper tests
// (closes the gap-doc P2 row "Drag & drop ComponentDragger utility").
//
// Validates the snapshot + translate contract:
//   - start_dragging snapshots mouse origin + view bounds,
//   - drag_view translates the bounds by the cursor delta,
//   - end_dragging makes subsequent drag_view a no-op,
//   - constrain_to_parent (default on) clamps inside the parent's local bounds,
//   - constrain_to_parent off lets the view escape its parent.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <memory>
#include <pulp/view/component_dragger.hpp>
#include <pulp/view/view.hpp>

using namespace pulp::view;

namespace {
// Minimal concrete View for the harness — View is abstract-friendly but
// not abstract; we just need an instantiable subclass to set bounds on.
class TestView : public View {};

// Parent/child pair set up at known coordinates. Returns the raw
// pointer to the child for assertions; the parent owns it.
struct ParentChild {
    std::unique_ptr<TestView> parent;
    TestView* child = nullptr;
};

ParentChild make_pair(Rect parent_bounds, Rect child_bounds) {
    ParentChild pc;
    pc.parent = std::make_unique<TestView>();
    pc.parent->set_bounds(parent_bounds);
    auto child = std::make_unique<TestView>();
    child->set_bounds(child_bounds);
    pc.child = child.get();
    pc.parent->add_child(std::move(child));
    return pc;
}
} // namespace

TEST_CASE("ComponentDragger: defaults are idle and constrain-on",
          "[component-dragger]") {
    ComponentDragger d;
    REQUIRE_FALSE(d.is_dragging());
    REQUIRE(d.constrain_to_parent());
}

TEST_CASE("ComponentDragger: start_dragging snapshots origin and bounds",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 400, 300}, {50, 60, 100, 80});
    ComponentDragger d;
    d.set_constrain_to_parent(false);
    d.start_dragging(*pc.child, {10, 12});

    REQUIRE(d.is_dragging());
    REQUIRE(d.mouse_start().x == Catch::Approx(10));
    REQUIRE(d.mouse_start().y == Catch::Approx(12));
    REQUIRE(d.bounds_start() == Rect{50, 60, 100, 80});
}

TEST_CASE("ComponentDragger: drag_view translates bounds by delta",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 400, 300}, {50, 60, 100, 80});
    ComponentDragger d;
    d.set_constrain_to_parent(false);
    d.start_dragging(*pc.child, {10, 12});
    d.drag_view(*pc.child, {30, 50});  // delta = (+20, +38)

    REQUIRE(pc.child->bounds() == Rect{70, 98, 100, 80});
}

TEST_CASE("ComponentDragger: drag_view is a no-op when not dragging",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 400, 300}, {50, 60, 100, 80});
    ComponentDragger d;
    d.drag_view(*pc.child, {999, 999});  // no start_dragging
    REQUIRE(pc.child->bounds() == Rect{50, 60, 100, 80});
}

TEST_CASE("ComponentDragger: end_dragging freezes subsequent drags",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 400, 300}, {50, 60, 100, 80});
    ComponentDragger d;
    d.set_constrain_to_parent(false);
    d.start_dragging(*pc.child, {0, 0});
    d.drag_view(*pc.child, {10, 10});
    REQUIRE(pc.child->bounds() == Rect{60, 70, 100, 80});

    d.end_dragging();
    REQUIRE_FALSE(d.is_dragging());
    d.drag_view(*pc.child, {100, 100});  // ignored
    REQUIRE(pc.child->bounds() == Rect{60, 70, 100, 80});
}

TEST_CASE("ComponentDragger: constrain_to_parent clamps origin inside parent",
          "[component-dragger]") {
    // Parent is 400x300, child 100x80. Constraining means child.x stays
    // in [0, 400-100=300] and child.y stays in [0, 300-80=220].
    auto pc = make_pair({0, 0, 400, 300}, {10, 10, 100, 80});
    ComponentDragger d;
    REQUIRE(d.constrain_to_parent());
    d.start_dragging(*pc.child, {0, 0});

    // Try to drag far past the right + bottom edges.
    d.drag_view(*pc.child, {9999, 9999});
    REQUIRE(pc.child->bounds().x == Catch::Approx(300));
    REQUIRE(pc.child->bounds().y == Catch::Approx(220));

    // And far past the top-left.
    d.drag_view(*pc.child, {-9999, -9999});
    REQUIRE(pc.child->bounds().x == Catch::Approx(0));
    REQUIRE(pc.child->bounds().y == Catch::Approx(0));
}

TEST_CASE("ComponentDragger: constrain off lets child escape parent",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 100, 100}, {0, 0, 50, 50});
    ComponentDragger d;
    d.set_constrain_to_parent(false);
    d.start_dragging(*pc.child, {0, 0});
    d.drag_view(*pc.child, {500, 500});
    REQUIRE(pc.child->bounds() == Rect{500, 500, 50, 50});
}

TEST_CASE("ComponentDragger: on_drag fires with the new bounds",
          "[component-dragger]") {
    auto pc = make_pair({0, 0, 400, 300}, {0, 0, 50, 50});
    ComponentDragger d;
    d.set_constrain_to_parent(false);
    Rect last{};
    int calls = 0;
    d.on_drag = [&](Rect r) { last = r; ++calls; };

    d.start_dragging(*pc.child, {0, 0});
    d.drag_view(*pc.child, {25, 30});
    REQUIRE(calls == 1);
    REQUIRE(last == Rect{25, 30, 50, 50});
}

TEST_CASE("ComponentDragger: constrain with no parent is a safe no-op",
          "[component-dragger]") {
    // Lone view without a parent — clamp branch must not crash.
    TestView orphan;
    orphan.set_bounds({10, 10, 50, 50});
    ComponentDragger d;  // constrain-on by default
    d.start_dragging(orphan, {0, 0});
    d.drag_view(orphan, {20, 20});
    REQUIRE(orphan.bounds() == Rect{30, 30, 50, 50});
}
