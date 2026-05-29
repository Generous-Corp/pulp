// W3C Flexbox parity tests — validates layout engine against CSS spec behavior
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/canvas/canvas.hpp>

using namespace pulp::view;
using pulp::canvas::RecordingCanvas;

// Helper: create a view with fixed preferred size
static std::unique_ptr<View> make_box(float w, float h) {
    auto v = std::make_unique<View>();
    v->flex().preferred_width = w;
    v->flex().preferred_height = h;
    return v;
}

// ═══════════════════════════════════════════════════════════════════
// justify-content
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: justify-content start (default)", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().justify_content = FlexJustify::start;
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.layout_children();

    REQUIRE(root.child_at(0)->bounds().x == 0.0f);
    REQUIRE(root.child_at(1)->bounds().x == 50.0f);
}

TEST_CASE("Layout: justify-content center", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().justify_content = FlexJustify::center;
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.layout_children();

    // 300 - 100 = 200 free space, centered → start at 100
    REQUIRE(root.child_at(0)->bounds().x == 100.0f);
    REQUIRE(root.child_at(1)->bounds().x == 150.0f);
}

TEST_CASE("Layout: justify-content end", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().justify_content = FlexJustify::end_;
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.layout_children();

    // 300 - 100 = 200 free → start at 200
    REQUIRE(root.child_at(0)->bounds().x == 200.0f);
    REQUIRE(root.child_at(1)->bounds().x == 250.0f);
}

TEST_CASE("Layout: justify-content space-between", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().justify_content = FlexJustify::space_between;
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.layout_children();

    // 300 - 150 = 150 free, 2 gaps → 75 each
    REQUIRE(root.child_at(0)->bounds().x == 0.0f);
    REQUIRE_THAT(root.child_at(1)->bounds().x, Catch::Matchers::WithinAbs(125.0f, 1.0f));
    REQUIRE_THAT(root.child_at(2)->bounds().x, Catch::Matchers::WithinAbs(250.0f, 1.0f));
}

TEST_CASE("Layout: justify-content space-evenly", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 400, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().justify_content = FlexJustify::space_evenly;
    root.add_child(make_box(50, 50));
    root.add_child(make_box(50, 50));
    root.layout_children();

    // 400 - 100 = 300 free, 3 slots → 100 each
    REQUIRE_THAT(root.child_at(0)->bounds().x, Catch::Matchers::WithinAbs(100.0f, 1.0f));
    REQUIRE_THAT(root.child_at(1)->bounds().x, Catch::Matchers::WithinAbs(250.0f, 1.0f));
}

// ═══════════════════════════════════════════════════════════════════
// max-width / max-height
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: max-width constrains growth", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 500, 50});
    root.flex().direction = FlexDirection::row;

    auto child = make_box(0, 50);
    child->flex().flex_grow = 1;
    child->flex().max_width = 200;
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE(root.child_at(0)->bounds().width <= 200.0f);
}

// ═══════════════════════════════════════════════════════════════════
// flex-shrink
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: flex-shrink prevents overflow", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 100, 50});
    root.flex().direction = FlexDirection::row;

    // Two 80px children in a 100px container — must shrink
    auto c1 = make_box(80, 50);
    c1->flex().flex_shrink = 1;
    auto c2 = make_box(80, 50);
    c2->flex().flex_shrink = 1;
    root.add_child(std::move(c1));
    root.add_child(std::move(c2));
    root.layout_children();

    // Both should shrink to fit
    float total = root.child_at(0)->bounds().width + root.child_at(1)->bounds().width;
    REQUIRE(total <= 100.0f);
}

// ═══════════════════════════════════════════════════════════════════
// Per-side padding
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: per-side padding", "[layout][w3c]") {
    View root;
    root.set_bounds({0, 0, 300, 200});
    root.flex().direction = FlexDirection::column;
    root.flex().padding_top = 10;
    root.flex().padding_left = 20;
    root.flex().padding_bottom = 30;
    root.flex().padding_right = 40;

    auto child = make_box(0, 50);
    child->flex().flex_grow = 1;
    root.add_child(std::move(child));
    root.layout_children();

    // Child should be inset by padding
    REQUIRE(root.child_at(0)->bounds().x == 20.0f);
    REQUIRE(root.child_at(0)->bounds().y == 10.0f);
    // Width = 300 - 20 - 40 = 240 (stretch)
    REQUIRE_THAT(root.child_at(0)->bounds().width, Catch::Matchers::WithinAbs(240.0f, 1.0f));
}

// ═══════════════════════════════════════════════════════════════════
// Visual properties
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("View: opacity default is 1.0", "[view][w3c]") {
    View v;
    REQUIRE(v.opacity() == 1.0f);
}

TEST_CASE("View: set_opacity clamps to 0-1", "[view][w3c]") {
    View v;
    v.set_opacity(-0.5f);
    REQUIRE(v.opacity() == 0.0f);
    v.set_opacity(2.0f);
    REQUIRE(v.opacity() == 1.0f);
    v.set_opacity(0.5f);
    REQUIRE(v.opacity() == 0.5f);
}

TEST_CASE("View: background color", "[view][w3c]") {
    View v;
    REQUIRE_FALSE(v.has_background_color());
    v.set_background_color(Color::rgba8(255, 0, 0));
    REQUIRE(v.has_background_color());
    v.clear_background_color();
    REQUIRE_FALSE(v.has_background_color());
}

TEST_CASE("View: border", "[view][w3c]") {
    View v;
    v.set_border(Color::rgba8(0, 255, 0), 2.0f, 8.0f);
    REQUIRE(v.corner_radius() == 8.0f);
}

TEST_CASE("View: overflow default is visible (CSS default, pulp #972)", "[view][w3c][issue-972]") {
    // pulp #972 — Pulp previously defaulted overflow to `hidden`, which
    // clipped absolutely-positioned children (popovers, dropdowns) to
    // their parent's content bounds and made them invisible. CSS default
    // is `overflow: visible`. Plugins that intentionally want clipping
    // must call set_overflow(Overflow::hidden) explicitly.
    View v;
    REQUIRE(v.overflow() == View::Overflow::visible);
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: margin
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: margin adds space around child", "[layout][w3c][margin]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;

    auto child = make_box(50, 50);
    child->flex().margin = 10;
    auto* cp = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    // Child should be offset by margin on all sides
    REQUIRE(cp->bounds().x == 10);
    REQUIRE(cp->bounds().y == 10);
}

TEST_CASE("Layout: per-side margin overrides uniform", "[layout][w3c][margin]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;

    auto child = make_box(50, 50);
    child->flex().margin = 5;
    child->flex().margin_left = 20;
    auto* cp = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE(cp->bounds().x == 20);  // margin_left overrides
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: align-self
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: align-self center overrides parent stretch", "[layout][w3c][align-self]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;
    root.flex().align_items = FlexAlign::stretch;

    auto child = make_box(50, 30);
    child->flex().align_self = FlexAlign::center;
    auto* cp = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    // Should be vertically centered, not stretched
    REQUIRE(cp->bounds().height == 30);
    REQUIRE(cp->bounds().y == 35.0f);  // (100-30)/2
}

TEST_CASE("Layout: align-self auto inherits parent", "[layout][w3c][align-self]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;
    root.flex().align_items = FlexAlign::stretch;

    auto child = std::make_unique<View>();
    child->flex().preferred_width = 50;
    // No preferred_height — cross-size is auto, so stretch should apply
    child->flex().align_self = FlexAlign::auto_;
    auto* cp = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    // auto = inherit stretch from parent (only when cross-size is auto)
    REQUIRE(cp->bounds().height == 100);
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: flex-basis
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: flex-basis overrides preferred width", "[layout][w3c][flex-basis]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.flex().direction = FlexDirection::row;

    auto child = make_box(50, 50);
    child->flex().flex_basis = 100;  // Override preferred_width of 50
    auto* cp = child.get();
    root.add_child(std::move(child));
    root.layout_children();

    REQUIRE(cp->bounds().width == 100);
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: order
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: order changes rendering sequence", "[layout][w3c][order]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;

    auto a = make_box(50, 50); a->set_id("a"); a->flex().order = 2;
    auto b = make_box(50, 50); b->set_id("b"); b->flex().order = 1;
    auto* ap = a.get();
    auto* bp = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.layout_children();

    // b (order=1) should come before a (order=2)
    REQUIRE(bp->bounds().x < ap->bounds().x);
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: directional gap
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: column_gap for row direction", "[layout][w3c][gap]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    root.flex().gap = 5;
    root.flex().column_gap = 20;  // overrides gap for row direction

    auto a = make_box(50, 50);
    auto b = make_box(50, 50);
    auto* bp = b.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.layout_children();

    REQUIRE(bp->bounds().x == 70);  // 50 + 20 gap
}

// ═══════════════════════════════════════════════════════════════════
// Phase 13.1: intrinsic sizing with Label
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: Label intrinsic height in column", "[layout][w3c][intrinsic]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.flex().direction = FlexDirection::column;

    auto label = std::make_unique<Label>("Hello");
    label->set_font_size(14);
    label->set_line_height(19.6f);
    auto* lp = label.get();
    root.add_child(std::move(label));
    root.layout_children();

    // Label should get intrinsic height (explicit 19.6, Yoga may round)
    REQUIRE_THAT(lp->bounds().height, Catch::Matchers::WithinAbs(19.6f, 1.0f));
}

TEST_CASE("View: paint_all with background renders without crash", "[view][w3c]") {
    RecordingCanvas rc;
    View v;
    v.set_bounds({0, 0, 100, 50});
    v.set_background_color(Color::rgba8(30, 30, 46));
    v.set_border(Color::rgba8(80, 80, 100), 1.0f, 4.0f);
    v.set_opacity(0.8f);
    v.paint_all(rc);
    REQUIRE(rc.commands().size() > 0);
}

// ═══════════════════════════════════════════════════════════════════
// CSS Transforms Level 1
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Transform: translate offsets rendering position", "[layout][transform]") {
    View v;
    v.set_bounds({10, 20, 100, 50});
    v.set_translate(5, 10);
    REQUIRE(v.translate_x() == 5.0f);
    REQUIRE(v.translate_y() == 10.0f);
    // Bounds unchanged — translate is visual only
    REQUIRE(v.bounds().x == 10.0f);
}

TEST_CASE("Transform: rotation stores degrees", "[layout][transform]") {
    View v;
    v.set_rotation(45);
    REQUIRE(v.rotation() == 45.0f);
}

TEST_CASE("Transform: origin default is center", "[layout][transform]") {
    View v;
    REQUIRE(v.transform_origin_x() == 0.5f);
    REQUIRE(v.transform_origin_y() == 0.5f);
}

// ═══════════════════════════════════════════════════════════════════
// CSS Animations Level 1
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("KeyframeAnimation: basic linear interpolation", "[animation]") {
    KeyframeAnimation anim;
    anim.set_keyframes({{0.0f, 0.0f}, {1.0f, 100.0f}});
    anim.set_duration(1.0f);
    anim.set_iterations(1);
    anim.set_fill_mode(KeyframeAnimation::FillMode::forwards);
    anim.start();

    // Advance to 50%
    anim.advance(0.5f);
    REQUIRE(anim.value() == 50.0f);

    // Advance to 100% — fill: forwards retains final value
    anim.advance(0.5f);
    REQUIRE(anim.value() == 100.0f);
    REQUIRE(anim.is_finished());
}

TEST_CASE("KeyframeAnimation: infinite iterations", "[animation]") {
    KeyframeAnimation anim;
    anim.set_keyframes({{0.0f, 0.0f}, {1.0f, 360.0f}});
    anim.set_duration(1.0f);
    anim.set_iterations(0);  // infinite
    anim.start();

    // Advance past 2 full iterations
    anim.advance(2.5f);
    REQUIRE(anim.is_running());
    REQUIRE(!anim.is_finished());
}

TEST_CASE("KeyframeAnimation: alternate direction", "[animation]") {
    KeyframeAnimation anim;
    anim.set_keyframes({{0.0f, 0.0f}, {1.0f, 100.0f}});
    anim.set_duration(1.0f);
    anim.set_iterations(2);
    anim.set_direction(KeyframeAnimation::Direction::alternate);
    anim.start();

    // First iteration goes 0→100
    anim.advance(0.5f);
    float v1 = anim.value();
    REQUIRE(v1 == 50.0f);

    // Second iteration goes 100→0
    anim.advance(1.0f);
    float v2 = anim.value();
    REQUIRE(v2 == 50.0f);  // Halfway back
}

TEST_CASE("KeyframeAnimation: multi-step keyframes", "[animation]") {
    KeyframeAnimation anim;
    anim.set_keyframes({{0.0f, 0.0f}, {0.5f, 200.0f}, {1.0f, 100.0f}});
    anim.set_duration(1.0f);
    anim.set_iterations(1);
    anim.start();

    // At 25%: interpolate between 0→200 at t=0.5 local
    anim.advance(0.25f);
    REQUIRE(anim.value() == 100.0f);

    // At 75%: interpolate between 200→100 at t=0.5 local
    anim.advance(0.5f);
    REQUIRE(anim.value() == 150.0f);
}

// ═══════════════════════════════════════════════════════════════════
// CSS Grid Layout Level 1
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("Layout: max-width 0% clamps a growing child to zero (issue-2704)",
          "[layout][w3c][issue-2704]") {
    // A flex-grow child with max-width:0% must be clamped to 0 — the percent
    // max path also used the >0 guard that dropped 0%.
    View root;
    root.set_bounds({0, 0, 400, 50});
    root.flex().direction = FlexDirection::row;
    auto child = std::make_unique<View>();
    child->flex().flex_grow = 1;
    child->flex().dim_max_width = Dimension{0.0f, DimensionUnit::percent};
    root.add_child(std::move(child));
    root.layout_children();
    REQUIRE(root.child_at(0)->bounds().width == 0.0f);
}

TEST_CASE("Grid: parse_template 'none' is empty and never throws (issue-2704)",
          "[layout][grid][issue-2704]") {
    // `none` is the CSS initial value for grid-template-* — std::stof("none")
    // used to throw out of WidgetBridge::load_script. It must yield no tracks.
    REQUIRE_NOTHROW(GridStyle::parse_template("none"));
    REQUIRE(GridStyle::parse_template("none").empty());
    // Unparseable tokens are skipped, not thrown; valid tracks still parse.
    REQUIRE_NOTHROW(GridStyle::parse_template("none auto 1fr garbage"));
    auto mixed = GridStyle::parse_template("none auto 1fr garbage");
    REQUIRE(mixed.size() == 2);
    REQUIRE(mixed[0].type == GridTrack::Type::auto_);
    REQUIRE(mixed[1].type == GridTrack::Type::fr);
}

TEST_CASE("Grid: parse_template expands simple repeat tracks",
          "[layout][grid]") {
    auto repeated = GridStyle::parse_template("repeat(4, 1fr)");
    REQUIRE(repeated.size() == 4);
    for (const auto& track : repeated) {
        REQUIRE(track.type == GridTrack::Type::fr);
        REQUIRE(track.value == 1.0f);
    }

    auto mixed = GridStyle::parse_template("70px repeat(3, 1fr)");
    REQUIRE(mixed.size() == 4);
    REQUIRE(mixed[0].type == GridTrack::Type::fixed);
    REQUIRE(mixed[0].value == 70.0f);
    for (std::size_t i = 1; i < mixed.size(); ++i) {
        REQUIRE(mixed[i].type == GridTrack::Type::fr);
        REQUIRE(mixed[i].value == 1.0f);
    }
}

TEST_CASE("Grid: parse_template caps recursion on pathologically nested repeat",
          "[layout][grid]") {
    // Semi-trusted design-tool input must not be able to overflow the stack via
    // deeply nested repeat(...) bodies. Build a 4000-deep nest and require the
    // parser to return (depth-capped) rather than recurse without bound.
    std::string nested = "1fr";
    for (int i = 0; i < 4000; ++i)
        nested = "repeat(2, " + nested + ")";
    auto tracks = GridStyle::parse_template(nested);
    // Past the depth cap the inner body yields no tracks, so the doubling
    // collapses to empty — the contract under test is "returns without
    // crashing", not a specific track count.
    SUCCEED("parse_template returned without unbounded recursion");
    (void)tracks;

    // A shallow nest still expands normally (2 * 3 = 6 fractional tracks).
    auto shallow = GridStyle::parse_template("repeat(2, repeat(3, 1fr))");
    REQUIRE(shallow.size() == 6);
}

TEST_CASE("Layout: width 0% lays out at zero, not intrinsic (issue-2704)",
          "[layout][w3c][issue-2704]") {
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    auto child = std::make_unique<View>();
    child->flex().preferred_width = 100;  // intrinsic fallback
    child->flex().dim_width = Dimension{0.0f, DimensionUnit::percent};  // width: 0%
    root.add_child(std::move(child));
    root.layout_children();
    // 0% must win over the intrinsic 100 (previously the >0 guard dropped 0%).
    REQUIRE(root.child_at(0)->bounds().width == 0.0f);
}

TEST_CASE("Layout: flex-basis 0% is honored, not treated as auto (issue-2704)",
          "[layout][w3c][issue-2704]") {
    // Two flex:1 children with different intrinsic widths but flex-basis:0%
    // should split the row EQUALLY (the classic `flex: 1` behavior). With the
    // old >0 guard, 0% basis was dropped and the unequal intrinsics leaked in.
    View root;
    root.set_bounds({0, 0, 300, 50});
    root.flex().direction = FlexDirection::row;
    for (int i = 0; i < 2; ++i) {
        auto c = std::make_unique<View>();
        c->flex().preferred_width = 40.0f + static_cast<float>(i) * 80.0f;
        c->flex().flex_grow = 1;
        c->flex().dim_flex_basis = Dimension{0.0f, DimensionUnit::percent};
        root.add_child(std::move(c));
    }
    root.layout_children();
    REQUIRE(root.child_at(0)->bounds().width == root.child_at(1)->bounds().width);
    REQUIRE(root.child_at(0)->bounds().width > 0.0f);
}

TEST_CASE("Grid: parse_template '1fr 2fr auto 100px'", "[layout][grid]") {
    auto tracks = GridStyle::parse_template("1fr 2fr auto 100");
    REQUIRE(tracks.size() == 4);
    REQUIRE(tracks[0].type == GridTrack::Type::fr);
    REQUIRE(tracks[0].value == 1.0f);
    REQUIRE(tracks[1].type == GridTrack::Type::fr);
    REQUIRE(tracks[1].value == 2.0f);
    REQUIRE(tracks[2].type == GridTrack::Type::auto_);
    REQUIRE(tracks[3].type == GridTrack::Type::fixed);
    REQUIRE(tracks[3].value == 100.0f);
}

TEST_CASE("Grid: 3-column layout with fr units", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 300, 100});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr 1fr");

    auto a = make_box(0, 0); auto* ap = a.get();
    auto b = make_box(0, 0); auto* bp = b.get();
    auto c = make_box(0, 0); auto* cp = c.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.add_child(std::move(c));
    root.layout_children();

    // Each column should be 100px wide (300 / 3)
    REQUIRE(ap->bounds().width == 100.0f);
    REQUIRE(bp->bounds().width == 100.0f);
    REQUIRE(cp->bounds().width == 100.0f);
    // Positioned left to right
    REQUIRE(ap->bounds().x == 0.0f);
    REQUIRE(bp->bounds().x == 100.0f);
    REQUIRE(cp->bounds().x == 200.0f);
}

TEST_CASE("Grid: mixed fixed + fr columns", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 400, 100});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("100 1fr 2fr");

    auto a = make_box(0, 0); auto* ap = a.get();
    auto b = make_box(0, 0); auto* bp = b.get();
    auto c = make_box(0, 0); auto* cp = c.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.add_child(std::move(c));
    root.layout_children();

    // 100px fixed, then 300px remaining split 1:2 = 100px + 200px
    REQUIRE(ap->bounds().width == 100.0f);
    REQUIRE(bp->bounds().width == 100.0f);
    REQUIRE(cp->bounds().width == 200.0f);
}

TEST_CASE("Grid: column gap", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 320, 100});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr 1fr");
    root.grid().column_gap = 10;

    auto a = make_box(0, 0); auto* ap = a.get();
    auto b = make_box(0, 0); auto* bp = b.get();
    auto c = make_box(0, 0); auto* cp = c.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.add_child(std::move(c));
    root.layout_children();

    // 320 - 20 (2 gaps) = 300, split 3 ways = 100 each
    REQUIRE(ap->bounds().width == 100.0f);
    REQUIRE(bp->bounds().x == 110.0f);  // 100 + 10 gap
    REQUIRE(cp->bounds().x == 220.0f);  // 100 + 10 + 100 + 10
}

TEST_CASE("Grid: auto row wrapping with 2 columns", "[layout][grid]") {
    View root;
    root.set_bounds({0, 0, 200, 200});
    root.set_layout_mode(LayoutMode::grid);
    root.grid().template_columns = GridStyle::parse_template("1fr 1fr");

    // 4 children = 2 rows of 2
    auto a = make_box(0, 0); auto* ap = a.get();
    auto b = make_box(0, 0); auto* bp = b.get();
    auto c = make_box(0, 0); auto* cp = c.get();
    auto d = make_box(0, 0); auto* dp = d.get();
    root.add_child(std::move(a));
    root.add_child(std::move(b));
    root.add_child(std::move(c));
    root.add_child(std::move(d));
    root.layout_children();

    // Row 1: a at (0,0), b at (100,0)
    REQUIRE(ap->bounds().x == 0.0f);
    REQUIRE(bp->bounds().x == 100.0f);
    REQUIRE(ap->bounds().y == bp->bounds().y);

    // Row 2: c at (0,30), d at (100,30) — default auto row height 30
    REQUIRE(cp->bounds().x == 0.0f);
    REQUIRE(dp->bounds().x == 100.0f);
    REQUIRE(cp->bounds().y > 0.0f);
    REQUIRE(cp->bounds().y == dp->bounds().y);
}
