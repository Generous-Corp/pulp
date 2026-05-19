// test_widget_bridge_yoga_border.cpp — extracted from
// test_widget_bridge.cpp in the 2026-05 Phase 5 (P5-1 follow-up) refactor.
//
// pulp #1543 — Yoga borderWidth wiring. Pulp's borders were already
// painted as a Skia stroke via `View::set_border_*`, but Yoga never knew
// about them. `apply_border_widths` in `core/view/src/yoga_layout.cpp`
// now calls `YGNodeStyleSetBorder` so the layout engine subtracts the
// border from the declared dimension the same way it subtracts padding.
//
// Yoga 3.x's default box-sizing is `border-box`, which is Pulp's
// pre-#1516 implicit behavior; the companion content-box test lives
// in #1516 once `setBoxSizing` is wired.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/widgets.hpp>

using namespace pulp::view;
using namespace pulp::state;
using Catch::Matchers::WithinAbs;

// pulp #1543 — Yoga borderWidth wiring. Pulp's borders were already
// painted as a Skia stroke via `View::set_border_*`, but Yoga never
// knew about them. Now `apply_border_widths` in
// `core/view/src/yoga_layout.cpp` calls `YGNodeStyleSetBorder` so the
// layout engine subtracts the border from the declared dimension the
// same way it already subtracts padding. Yoga 3.x's default
// box-sizing is `border-box`, which is also Pulp's pre-#1516 implicit
// behavior — so a border-box load-bearing test passes without any
// box-sizing plumbing. The companion content-box test lives in #1516
// once `setBoxSizing` is wired.
//
// Layout test 1: width=100, padding=0, borderWidth=10. Yoga 3.x's
// default border-box: outer (declared) = 100, border=10 each side →
// content area = 100 - 2*10 = 80. We assert the parent's outer width
// stays 100 (the declared dimension under border-box) and that a
// 100%-width child occupies only the inner content area = 80.
TEST_CASE("borderWidth shrinks content area (Yoga border-box default, #1543)",
          "[view][bridge][yoga][layout][issue-1543]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderWidth('parent', 10);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // border-box (Yoga 3.x default): outer == declared.
    REQUIRE_THAT(parent->bounds().width,  WithinAbs(100.0f, 0.5f));
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Content area = 100 - 2*10 = 80. The child fills 100% of the
    // CONTENT box (Yoga's content-box, post-border, post-padding).
    // Pre-#1543, Yoga saw 0 border and the child would have been sized
    // to 100, leaking under the painted stroke.
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

// Layout test 3: per-edge border variants. borderTopWidth=5,
// borderBottomWidth=5, padding=0 → inside the parent the top inset is
// 5 and the bottom inset is 5. We pin a 100%-height child and assert
// its absolute Y position is 5 (top border) and its height is 90
// (parent height 100 - top 5 - bottom 5).
TEST_CASE("per-edge borderTopWidth / borderBottomWidth set Yoga insets",
          "[view][bridge][yoga][layout][issue-1543]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderTopWidth('parent', 5);
        setBorderBottomWidth('parent', 5);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // Parent outer == declared 100x100 (border-box default).
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Child sits below the top border (y inset = 5, relative to
    // parent, which is what apply_yoga_results stores via
    // YGNodeLayoutGetTop), is 90 tall (parent 100 - top 5 - bottom 5),
    // full-width (no left/right border so width is unchanged at 100).
    REQUIRE_THAT(child->bounds().y,      WithinAbs(5.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(90.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(100.0f, 0.5f));
}

// pulp #1566 (Codex P2 follow-up to #1543) — an explicit per-edge
// `borderTopWidth: 0` MUST override the uniform `borderWidth: 10`
// shorthand on that edge. Pre-#1566, apply_border_widths treated a
// per-side value of 0 as "unset" and silently fell back to the
// uniform value, so the painted top stroke kept its 10px inset and
// child positioning remained shrunk by 10px. CSS and React Native
// both treat the longhand as overriding the shorthand even when
// the longhand is 0.
TEST_CASE("explicit per-edge borderWidth=0 overrides uniform shorthand",
          "[view][bridge][yoga][layout][issue-1543][issue-1566]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        // Uniform shorthand applies 10px to all four edges, then the
        // per-edge longhand zeroes the top edge (and only the top).
        setBorderWidth('parent', 10);
        setBorderTopWidth('parent', 0);
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // Parent outer == declared 100x100 (border-box default).
    REQUIRE_THAT(parent->bounds().width,  WithinAbs(100.0f, 0.5f));
    REQUIRE_THAT(parent->bounds().height, WithinAbs(100.0f, 0.5f));
    // Top inset is 0 (explicit 0 wins over the 10 shorthand). The
    // bottom / left / right inset stays at the shorthand 10. So the
    // child sits at y=0, height = 100 - 0(top) - 10(bottom) = 90,
    // x=10, width = 100 - 10 - 10 = 80.
    REQUIRE_THAT(child->bounds().y,      WithinAbs(0.0f,  0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(90.0f, 0.5f));
    REQUIRE_THAT(child->bounds().x,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
}

// pulp #1566 — color-only setters MUST NOT mark the per-edge width as
// explicitly set. If `setBorderTopColor` flipped the width's `set` flag
// to true (with the stored width still 0), the uniform `borderWidth: 10`
// shorthand would silently drop to 0 on the top edge. Equivalent to the
// CSS rule that `border-top-color` and `border-top-width` are
// independent longhands.
TEST_CASE("setBorderTopColor preserves uniform borderWidth shorthand",
          "[view][bridge][yoga][layout][issue-1543][issue-1566]") {
    ScriptEngine engine;
    View root;
    root.set_bounds({0, 0, 400, 400});
    StateStore store;
    WidgetBridge bridge(engine, root, store);
    bridge.load_script(R"(
        createPanel('parent', '');
        createPanel('child', 'parent');
        setFlex('parent', 'width',  100);
        setFlex('parent', 'height', 100);
        setBorderWidth('parent', 10);
        // Color-only — width on every edge must still be 10.
        setBorderTopColor('parent', '#ff0000ff');
        setFlex('child', 'width',  '100%');
        setFlex('child', 'height', '100%');
    )");
    root.layout_children();
    auto* parent = bridge.widget("parent");
    auto* child  = bridge.widget("child");
    REQUIRE(parent != nullptr);
    REQUIRE(child  != nullptr);
    // All four edges still 10 → child is 80x80, offset (10,10).
    REQUIRE_THAT(child->bounds().x,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().y,      WithinAbs(10.0f, 0.5f));
    REQUIRE_THAT(child->bounds().width,  WithinAbs(80.0f, 0.5f));
    REQUIRE_THAT(child->bounds().height, WithinAbs(80.0f, 0.5f));
}

