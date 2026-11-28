#include <catch2/catch_test_macros.hpp>
#include <pulp/vellum_components/overlay_geometry.hpp>

#include <vellum/view/view.hpp>

#include <memory>
#include <unordered_set>

using namespace pulp::vellum_components;
using namespace pulp::vellum_components::overlay;

namespace {

// root -> panel (10,20) -> child (3,4) 50x60
struct Tree {
    View root;
    View* panel = nullptr;
    View* child = nullptr;
    Tree() {
        root.set_bounds({5, 7, 400, 300});
        auto p = std::make_unique<View>();
        p->set_bounds({10, 20, 200, 200});
        auto c = std::make_unique<View>();
        c->set_bounds({3, 4, 50, 60});
        child = c.get();
        p->add_child(std::move(c));
        panel = p.get();
        root.add_child(std::move(p));
    }
};

const Rect kSel{100, 100, 80, 40};

} // namespace

TEST_CASE("bounds_in_root sums the chain below the root and stops before it",
          "[pulp-vellum-components][overlay-geometry]") {
    Tree t;
    const auto r = bounds_in_root(t.child, t.root);
    // The root's own origin is not part of its children's root space.
    REQUIRE(r.x == 13);
    REQUIRE(r.y == 24);
    REQUIRE(r.width == 50);
    REQUIRE(r.height == 60);
    REQUIRE(bounds_in_root(&t.root, t.root).x == 0);
    REQUIRE(bounds_in_root(&t.root, t.root).width == 400);
    REQUIRE(bounds_in_root(nullptr, t.root).width == 0);
}

TEST_CASE("effective_scale_in_root multiplies the scales below the root",
          "[pulp-vellum-components][overlay-geometry]") {
    Tree t;
    t.panel->set_scale(2.0f);
    t.child->set_scale(1.5f);
    t.root.set_scale(10.0f);
    REQUIRE(effective_scale_in_root(t.child, t.root) == 3.0f);
    REQUIRE(effective_scale_in_root(&t.root, t.root) == 1.0f);
}

TEST_CASE("is_self_or_ancestor and containing_block_of walk the parent chain",
          "[pulp-vellum-components][overlay-geometry]") {
    Tree t;
    REQUIRE(is_self_or_ancestor(t.child, t.child));
    REQUIRE(is_self_or_ancestor(&t.root, t.child));
    REQUIRE_FALSE(is_self_or_ancestor(t.child, &t.root));

    Rect block{};
    // Everything is static, so the root is the containing block.
    REQUIRE(containing_block_of(t.child, t.root, block) == &t.root);
    REQUIRE(block.width == 400);

    t.panel->set_position(View::Position::relative);
    REQUIRE(containing_block_of(t.child, t.root, block) == t.panel);
    REQUIRE(block.x == 10);
    REQUIRE(block.y == 20);
    REQUIRE(block.width == 200);
}

TEST_CASE("hit_test_handle finds each corner and edge midpoint within the grab box",
          "[pulp-vellum-components][overlay-geometry]") {
    REQUIRE(hit_test_handle(kSel, {100, 100}) == DragHandle::nw);
    REQUIRE(hit_test_handle(kSel, {180, 100}) == DragHandle::ne);
    REQUIRE(hit_test_handle(kSel, {100, 140}) == DragHandle::sw);
    REQUIRE(hit_test_handle(kSel, {180, 140}) == DragHandle::se);
    REQUIRE(hit_test_handle(kSel, {140, 100}) == DragHandle::n);
    REQUIRE(hit_test_handle(kSel, {140, 140}) == DragHandle::s);
    REQUIRE(hit_test_handle(kSel, {180, 120}) == DragHandle::e);
    REQUIRE(hit_test_handle(kSel, {100, 120}) == DragHandle::w);
    // Inside the 12 px box (6 from the centre) and just outside it.
    REQUIRE(hit_test_handle(kSel, {106, 106}) == DragHandle::nw);
    REQUIRE(hit_test_handle(kSel, {106.5f, 100}) == DragHandle::none);
    REQUIRE(hit_test_handle(kSel, {140, 120}) == DragHandle::none);
}

TEST_CASE("a corner wins over the edge midpoints it overlaps on a thin selection",
          "[pulp-vellum-components][overlay-geometry]") {
    // 8 px wide: the north-middle handle's box (98 to 110 at x=104) overlaps
    // both top corner boxes. Where they overlap, the corner is reported.
    const Rect thin{100, 100, 8, 40};
    REQUIRE(hit_test_handle(thin, {96, 100}) == DragHandle::nw);
    REQUIRE(hit_test_handle(thin, {104, 100}) == DragHandle::nw);
    REQUIRE(hit_test_handle(thin, {110, 100}) == DragHandle::ne);
}

TEST_CASE("hit_test_body is the selection minus its handles",
          "[pulp-vellum-components][overlay-geometry]") {
    REQUIRE(hit_test_body(kSel, {140, 120}));
    REQUIRE_FALSE(hit_test_body(kSel, {100, 100})); // on a handle
    REQUIRE_FALSE(hit_test_body(kSel, {80, 120}));
    REQUIRE_FALSE(hit_test_body(kSel, {300, 300}));
    // The top edge, away from any handle, is inside.
    REQUIRE(hit_test_body(kSel, {130, 100}));
}

TEST_CASE("affordances map handles, body and outside to a cursor",
          "[pulp-vellum-components][overlay-geometry]") {
    REQUIRE(affordance_at(kSel, {100, 100}) == CursorAffordance::resize_nw_se);
    REQUIRE(affordance_at(kSel, {180, 140}) == CursorAffordance::resize_nw_se);
    REQUIRE(affordance_at(kSel, {180, 100}) == CursorAffordance::resize_ne_sw);
    REQUIRE(affordance_at(kSel, {100, 140}) == CursorAffordance::resize_ne_sw);
    REQUIRE(affordance_at(kSel, {140, 100}) == CursorAffordance::resize_ns);
    REQUIRE(affordance_at(kSel, {140, 140}) == CursorAffordance::resize_ns);
    REQUIRE(affordance_at(kSel, {180, 120}) == CursorAffordance::resize_ew);
    REQUIRE(affordance_at(kSel, {100, 120}) == CursorAffordance::resize_ew);
    REQUIRE(affordance_at(kSel, {140, 120}) == CursorAffordance::move);
    REQUIRE(affordance_at(kSel, {10, 10}) == CursorAffordance::none);
    REQUIRE(affordance_for_handle(DragHandle::none) == CursorAffordance::none);
}

TEST_CASE("cursor_style_value maps every affordance to its View cursor",
          "[pulp-vellum-components][overlay-geometry]") {
    using CS = View::CursorStyle;
    REQUIRE(cursor_style_value(CursorAffordance::move) ==
            static_cast<int>(CS::multi_directional_resize));
    REQUIRE(cursor_style_value(CursorAffordance::resize_nw_se) ==
            static_cast<int>(CS::top_left_resize));
    REQUIRE(cursor_style_value(CursorAffordance::resize_ne_sw) ==
            static_cast<int>(CS::top_right_resize));
    REQUIRE(cursor_style_value(CursorAffordance::resize_ns) ==
            static_cast<int>(CS::vertical_resize));
    REQUIRE(cursor_style_value(CursorAffordance::resize_ew) ==
            static_cast<int>(CS::horizontal_resize));
    REQUIRE(cursor_style_value(CursorAffordance::none) == -1);
}

TEST_CASE("flatten_tree lists depth first and skips collapsed subtrees",
          "[pulp-vellum-components][overlay-geometry]") {
    Tree t;
    t.panel->add_child(std::make_unique<View>());
    auto all = flatten_tree(t.root, {});
    REQUIRE(all.size() == 4); // root, panel, child, extra
    REQUIRE(all[0].view == &t.root);
    REQUIRE(all[0].depth == 0);
    REQUIRE(all[1].view == t.panel);
    REQUIRE(all[1].depth == 1);
    REQUIRE(all[2].view == t.child);
    REQUIRE(all[2].depth == 2);

    // A collapsed view stays listed but its children do not.
    const auto folded = flatten_tree(t.root, {t.panel});
    REQUIRE(folded.size() == 2);
    REQUIRE(folded[1].view == t.panel);
}

TEST_CASE("tree_item_at maps a y to a row and rejects anything outside the list",
          "[pulp-vellum-components][overlay-geometry]") {
    Tree t;
    const auto items = flatten_tree(t.root, {});
    REQUIRE(items.size() == 3);
    REQUIRE(tree_item_at(items, 0.0f, 20.0f) == &items[0]);
    REQUIRE(tree_item_at(items, 19.9f, 20.0f) == &items[0]);
    REQUIRE(tree_item_at(items, 20.0f, 20.0f) == &items[1]);
    REQUIRE(tree_item_at(items, 59.0f, 20.0f) == &items[2]);
    REQUIRE(tree_item_at(items, 60.0f, 20.0f) == nullptr);
    // Above the first row is not row 0.
    REQUIRE(tree_item_at(items, -5.0f, 20.0f) == nullptr);
}

TEST_CASE("point_in_panel is the docked right strip, and nothing in the bare layer",
          "[pulp-vellum-components][overlay-geometry]") {
    REQUIRE(point_in_panel({700, 10}, 1000, 300, false));
    REQUIRE(point_in_panel({1000, 10}, 1000, 300, false));
    REQUIRE_FALSE(point_in_panel({699.9f, 10}, 1000, 300, false));
    REQUIRE_FALSE(point_in_panel({900, 10}, 1000, 300, true));
}
