// Position layout tests — validates CSS positioned layout (absolute, relative, z-index)

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "test_helpers.hpp"

using namespace pulp::test;
using namespace pulp::view;
using Catch::Matchers::WithinAbs;

// ═══════════════════════════════════════════════════════════════════════════════
// Position modes
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: default is static", "[layout][position]") {
    View v;
    REQUIRE(v.position() == View::Position::static_);
}

TEST_CASE("Position: set absolute", "[layout][position]") {
    View v;
    v.set_position(View::Position::absolute);
    REQUIRE(v.position() == View::Position::absolute);
}

TEST_CASE("Position: set relative", "[layout][position]") {
    View v;
    v.set_position(View::Position::relative);
    REQUIRE(v.position() == View::Position::relative);
}

TEST_CASE("Position: set fixed", "[layout][position]") {
    View v;
    v.set_position(View::Position::fixed);
    REQUIRE(v.position() == View::Position::fixed);
}

TEST_CASE("Position: set sticky", "[layout][position]") {
    View v;
    v.set_position(View::Position::sticky);
    REQUIRE(v.position() == View::Position::sticky);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Absolute positioning with TRBL offsets
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: absolute with top/left compiles and sets", "[layout][position]") {
    View v;
    v.set_position(View::Position::absolute);
    v.set_top(10.0f);
    v.set_left(20.0f);
    // Setters exist and don't crash; no public getters for the values,
    // so we verify the position mode was set correctly.
    REQUIRE(v.position() == View::Position::absolute);
}

TEST_CASE("Position: absolute with right/bottom compiles and sets", "[layout][position]") {
    View v;
    v.set_position(View::Position::absolute);
    v.set_right(15.0f);
    v.set_bottom(25.0f);
    REQUIRE(v.position() == View::Position::absolute);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Z-index
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: z-index default is 0", "[layout][position]") {
    View v;
    REQUIRE(v.z_index() == 0);
}

TEST_CASE("Position: set z-index", "[layout][position]") {
    View v;
    v.set_z_index(10);
    REQUIRE(v.z_index() == 10);
}

TEST_CASE("Position: negative z-index", "[layout][position]") {
    View v;
    v.set_z_index(-5);
    REQUIRE(v.z_index() == -5);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Relative offset
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: relative with translate offset", "[layout][position]") {
    View v;
    v.set_position(View::Position::relative);
    v.set_translate(5.0f, 10.0f);
    REQUIRE(v.translate_x() == 5.0f);
    REQUIRE(v.translate_y() == 10.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Overflow
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: overflow hidden", "[layout][position]") {
    View v;
    v.set_overflow(View::Overflow::hidden);
    REQUIRE(v.overflow() == View::Overflow::hidden);
}

TEST_CASE("Position: overflow visible", "[layout][position]") {
    View v;
    v.set_overflow(View::Overflow::visible);
    REQUIRE(v.overflow() == View::Overflow::visible);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hit test with positioned children
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: hit_test finds child at position", "[layout][position]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    auto* cp = child.get();
    root.add_child(std::move(child));

    REQUIRE(root.hit_test({75, 75}) == cp);
    REQUIRE(root.hit_test({200, 200}) == &root);
}

TEST_CASE("Position: hit_test returns topmost child at overlapping position", "[layout][position]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto a = std::make_unique<View>();
    a->set_bounds({50, 50, 100, 100});
    auto b = std::make_unique<View>();
    b->set_bounds({80, 80, 100, 100}); // overlaps with a
    auto* bp = b.get();

    root.add_child(std::move(a));
    root.add_child(std::move(b)); // b on top (later child)

    // At overlap point (90, 90), b should win (painted later)
    REQUIRE(root.hit_test({90, 90}) == bp);
}

TEST_CASE("Position: hit_test misses invisible child", "[layout][position]") {
    View root;
    root.set_bounds({0, 0, 400, 400});

    auto child = std::make_unique<View>();
    child->set_bounds({50, 50, 100, 100});
    child->set_visible(false);
    root.add_child(std::move(child));

    // Invisible child should not be hit
    REQUIRE(root.hit_test({75, 75}) == &root);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Bounds and geometry
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Position: Rect contains point", "[layout][geometry]") {
    Rect r{10, 20, 100, 50};
    REQUIRE(r.contains({50, 40}));
    REQUIRE_FALSE(r.contains({5, 40}));
    REQUIRE_FALSE(r.contains({50, 5}));
}

TEST_CASE("Position: Rect right and bottom", "[layout][geometry]") {
    Rect r{10, 20, 100, 50};
    REQUIRE(r.right() == 110.0f);
    REQUIRE(r.bottom() == 70.0f);
}

TEST_CASE("Position: Rect center", "[layout][geometry]") {
    Rect r{0, 0, 100, 50};
    auto c = r.center();
    REQUIRE(c.x == 50.0f);
    REQUIRE(c.y == 25.0f);
}

TEST_CASE("Position: Rect inset", "[layout][geometry]") {
    Rect r{10, 10, 100, 80};
    auto inset = r.inset(5);
    REQUIRE(inset.x == 15.0f);
    REQUIRE(inset.y == 15.0f);
    REQUIRE(inset.width == 90.0f);
    REQUIRE(inset.height == 70.0f);
}

TEST_CASE("Position: Rect is_empty", "[layout][geometry]") {
    Rect empty{0, 0, 0, 0};
    REQUIRE(empty.is_empty());
    Rect valid{0, 0, 100, 50};
    REQUIRE_FALSE(valid.is_empty());
}

TEST_CASE("Position: Point arithmetic", "[layout][geometry]") {
    Point a{10, 20};
    Point b{5, 15};
    auto sum = a + b;
    REQUIRE(sum.x == 15.0f);
    REQUIRE(sum.y == 35.0f);
    auto diff = a - b;
    REQUIRE(diff.x == 5.0f);
    REQUIRE(diff.y == 5.0f);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Regression: pulp #1379 / #998 — absolute children must fill parent regardless
// of flex siblings. Before the fix, flex_shrink:1 (the FlexStyle default) leaked
// into Yoga for absolute children, so an absolute `inset:0` child was placed
// AFTER preceding column-flow siblings and shrunk to fit the remainder.
//
// CSS spec: an out-of-flow box (position:absolute or fixed) is taken out of the
// flex line — its grow/shrink/basis must not participate, and `top/left/right/
// bottom` are interpreted against the containing block's content box (here,
// the parent's full local rect since there's no padding).
// ═══════════════════════════════════════════════════════════════════════════════

TEST_CASE("Yoga: absolute child with inset:0 fills parent regardless of flex siblings",
          "[layout][yoga][absolute][issue-1379][issue-998]") {
    View parent;
    parent.set_bounds({0, 0, 1320, 860});
    parent.flex().direction = FlexDirection::column;

    auto wrap = std::make_unique<View>();
    wrap->set_position(View::Position::absolute);
    wrap->set_top(0);
    wrap->set_left(0);
    wrap->set_right(0);
    wrap->set_bottom(0);
    auto* wrapPtr = wrap.get();
    parent.add_child(std::move(wrap));

    // A normal flow header sibling — pre-fix this was eating the wrap's main
    // axis space and pushing the absolute wrap to y=44, height=816.
    auto header = std::make_unique<View>();
    header->flex().preferred_height = 44;
    parent.add_child(std::move(header));

    parent.layout_children();

    REQUIRE(wrapPtr->bounds().x == 0.0f);
    REQUIRE(wrapPtr->bounds().y == 0.0f);
    REQUIRE(wrapPtr->bounds().width == 1320.0f);
    REQUIRE(wrapPtr->bounds().height == 860.0f);
}

TEST_CASE("Yoga: absolute child with explicit width/height + top:0/left:0 fills parent",
          "[layout][yoga][absolute][issue-1379][issue-998]") {
    // Variant for when the user uses preferred_width/height instead of inset
    // edges. Spectr's dom-adapter currently sets BOTH (defensive) — verify
    // either path lands at (0,0,W,H) without the flex sibling stealing space.
    View parent;
    parent.set_bounds({0, 0, 1320, 860});
    parent.flex().direction = FlexDirection::column;

    auto wrap = std::make_unique<View>();
    wrap->set_position(View::Position::absolute);
    wrap->set_top(0);
    wrap->set_left(0);
    wrap->flex().preferred_width = 1320;
    wrap->flex().preferred_height = 860;
    auto* wrapPtr = wrap.get();
    parent.add_child(std::move(wrap));

    auto header = std::make_unique<View>();
    header->flex().preferred_height = 44;
    parent.add_child(std::move(header));

    auto tweaks = std::make_unique<View>();
    tweaks->flex().preferred_height = 200;
    parent.add_child(std::move(tweaks));

    parent.layout_children();

    REQUIRE(wrapPtr->bounds().x == 0.0f);
    REQUIRE(wrapPtr->bounds().y == 0.0f);
    REQUIRE(wrapPtr->bounds().width == 1320.0f);
    REQUIRE(wrapPtr->bounds().height == 860.0f);
}

TEST_CASE("Yoga: layered absolute canvases (filterbank+overlay) both fill parent",
          "[layout][yoga][absolute][issue-1379]") {
    // FilterBank's structural shape: an absolute wrap with two absolute
    // <canvas> children (main + overlay), both inset:0. The overlay sibling
    // must not push the main canvas out of position even though they share
    // a flex line conceptually.
    View parent;
    parent.set_bounds({0, 0, 1320, 860});

    auto wrap = std::make_unique<View>();
    wrap->set_position(View::Position::absolute);
    wrap->set_top(0); wrap->set_left(0);
    wrap->set_right(0); wrap->set_bottom(0);
    auto* wrapPtr = wrap.get();

    auto canvas = std::make_unique<View>();
    canvas->set_position(View::Position::absolute);
    canvas->set_top(0); canvas->set_left(0);
    canvas->set_right(0); canvas->set_bottom(0);
    auto* canvasPtr = canvas.get();
    wrap->add_child(std::move(canvas));

    auto overlay = std::make_unique<View>();
    overlay->set_position(View::Position::absolute);
    overlay->set_top(0); overlay->set_left(0);
    overlay->set_right(0); overlay->set_bottom(0);
    auto* overlayPtr = overlay.get();
    wrap->add_child(std::move(overlay));

    parent.add_child(std::move(wrap));
    parent.layout_children();

    REQUIRE(wrapPtr->bounds().width == 1320.0f);
    REQUIRE(wrapPtr->bounds().height == 860.0f);

    REQUIRE(canvasPtr->bounds().x == 0.0f);
    REQUIRE(canvasPtr->bounds().y == 0.0f);
    REQUIRE(canvasPtr->bounds().width == 1320.0f);
    REQUIRE(canvasPtr->bounds().height == 860.0f);

    REQUIRE(overlayPtr->bounds().x == 0.0f);
    REQUIRE(overlayPtr->bounds().y == 0.0f);
    REQUIRE(overlayPtr->bounds().width == 1320.0f);
    REQUIRE(overlayPtr->bounds().height == 860.0f);
}

// pulp #1899 — documents Spectr's actual mount shape. The App component
// in `spectr-editor-extracted.js:4140` is a literal-CSS hardcoded
// `position:absolute; top:0; left:0; width:1320; height:860`. When pulp-
// screenshot renders into a smaller viewport (e.g. the default 1280x800
// at @2x to match the captured webview baseline), Spectr's App
// overflows by 40 horizontally and 60 vertically, clipping the bottom
// action rail (App-y=804..860 → viewport-y=804..860 → off-screen).
//
// This test pins the current Yoga behaviour so a future fix that
// changes viewport-vs-content reconciliation (e.g. flex-centering body-
// equivalent, or auto-clamping oversize absolute children to viewport)
// has a regression anchor. Today the App is placed at (0,0,1320,860)
// inside a 1280x800 viewport — overflowing, not centered.
TEST_CASE("Yoga: Spectr-shape — hardcoded-size App inside smaller viewport overflows",
          "[layout][yoga][absolute][spectr][issue-1899]") {
    View viewport;
    viewport.set_bounds({0, 0, 1280, 800});

    // Spectr's App: position:absolute, top:0, left:0, hardcoded 1320x860
    auto app = std::make_unique<View>();
    app->set_position(View::Position::absolute);
    app->set_top(0);
    app->set_left(0);
    app->flex().preferred_width = 1320;
    app->flex().preferred_height = 860;
    auto* appPtr = app.get();

    // Bottom action rail inside the App
    auto rail = std::make_unique<View>();
    rail->set_position(View::Position::absolute);
    rail->set_bottom(0);
    rail->set_left(0);
    rail->set_right(0);
    rail->flex().preferred_height = 56;
    auto* railPtr = rail.get();
    app->add_child(std::move(rail));

    viewport.add_child(std::move(app));
    viewport.layout_children();

    // App lands at viewport (0,0,1320,860) — overflowing
    REQUIRE(appPtr->bounds().x == 0.0f);
    REQUIRE(appPtr->bounds().y == 0.0f);
    REQUIRE(appPtr->bounds().width == 1320.0f);
    REQUIRE(appPtr->bounds().height == 860.0f);

    // Rail is correctly anchored at bottom:0 of the App (App-y=804) —
    // proves Yoga DOES propagate position:absolute + bottom:0 + height
    // correctly. The visual bug is purely the viewport mismatch.
    REQUIRE(railPtr->bounds().x == 0.0f);
    REQUIRE(railPtr->bounds().y == 804.0f);
    REQUIRE(railPtr->bounds().width == 1320.0f);
    REQUIRE(railPtr->bounds().height == 56.0f);
}

TEST_CASE("Yoga: absolute child does not consume flex-line space from siblings",
          "[layout][yoga][absolute][issue-1379]") {
    // Inverse check: if absolute children leak into the flex line, the
    // remaining flex sibling will have its size reduced. Verify the in-flow
    // sibling gets the FULL parent height it asked for.
    View parent;
    parent.set_bounds({0, 0, 1320, 860});
    parent.flex().direction = FlexDirection::column;

    auto abs = std::make_unique<View>();
    abs->set_position(View::Position::absolute);
    abs->set_top(0); abs->set_left(0);
    abs->set_right(0); abs->set_bottom(0);
    parent.add_child(std::move(abs));

    auto header = std::make_unique<View>();
    header->flex().preferred_height = 44;
    auto* headerPtr = header.get();
    parent.add_child(std::move(header));

    auto body = std::make_unique<View>();
    body->flex().flex_grow = 1;
    auto* bodyPtr = body.get();
    parent.add_child(std::move(body));

    parent.layout_children();

    // Header keeps its 44 px regardless of the absolute child.
    REQUIRE(headerPtr->bounds().y == 0.0f);
    REQUIRE(headerPtr->bounds().height == 44.0f);
    // Body gets the remaining 816 px — proving the absolute child was NOT
    // counted in the flex line.
    REQUIRE(bodyPtr->bounds().y == 44.0f);
    REQUIRE(bodyPtr->bounds().height == 816.0f);
}
