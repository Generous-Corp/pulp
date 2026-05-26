// Item 6.1 of `planning/2026-05-24-macos-plugin-authoring-plan.md`:
// VectorScene retained scene graph. Acceptance criteria:
//   1. SVG loads as a VectorScene / SceneGroup.
//   2. Mutating a child's opacity re-paints only that sub-tree's
//      bounding box.
//   3. Renders through the existing Canvas API (RecordingCanvas here
//      exercises the command stream without requiring Skia).
//
// Pulp-native names — no reference-framework class-name lineage.

#include <catch2/catch_test_macros.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/canvas/scene/scene.hpp>

#include <cmath>
#include <memory>

using namespace pulp::canvas;

namespace {

constexpr float kEps = 1e-4f;

bool approx_eq(float a, float b, float eps = kEps) {
    return std::fabs(a - b) <= eps;
}

bool approx_eq(const SceneRect& a, const SceneRect& b, float eps = kEps) {
    return approx_eq(a.x, b.x, eps) && approx_eq(a.y, b.y, eps) &&
           approx_eq(a.w, b.w, eps) && approx_eq(a.h, b.h, eps);
}

size_t count_op(const RecordingCanvas& rc, DrawCommand::Type t) {
    return rc.count(t);
}

}  // namespace

TEST_CASE("SceneRect united / map_rect math", "[canvas][scene][issue-2700]") {
    SceneRect empty{};
    REQUIRE(empty.empty());

    SceneRect a{10, 10, 20, 20};
    REQUIRE_FALSE(a.empty());
    // Empty + a = a
    REQUIRE(approx_eq(empty.united(a), a));
    REQUIRE(approx_eq(a.united(empty), a));

    // Disjoint union covers both.
    SceneRect b{40, 50, 5, 5};
    SceneRect u = a.united(b);
    REQUIRE(approx_eq(u, SceneRect{10, 10, 35, 45}));

    // Identity transform leaves rect alone.
    SceneTransform id = SceneTransform::identity();
    REQUIRE(approx_eq(id.map_rect(a), a));

    // Pure translation moves rect.
    SceneTransform t = SceneTransform::translation(100, -5);
    REQUIRE(approx_eq(t.map_rect(a), SceneRect{110, 5, 20, 20}));

    // 90° rotation (a=cos, b=sin, c=-sin, d=cos) — AABB shape preserved
    // (square stays square at 90°).
    SceneTransform r90{0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    SceneRect square{0, 0, 10, 10};
    SceneRect rotated = r90.map_rect(square);
    REQUIRE(approx_eq(rotated.w, 10.0f));
    REQUIRE(approx_eq(rotated.h, 10.0f));
}

TEST_CASE("SceneShape local_bounds for each kind",
          "[canvas][scene][issue-2700]") {
    auto rect = SceneShape::make_rect(5, 10, 30, 40);
    REQUIRE(approx_eq(rect->local_bounds(), SceneRect{5, 10, 30, 40}));

    auto rr = SceneShape::make_rounded_rect(0, 0, 50, 20, 5);
    REQUIRE(approx_eq(rr->local_bounds(), SceneRect{0, 0, 50, 20}));

    auto circ = SceneShape::make_circle(100, 100, 25);
    REQUIRE(approx_eq(circ->local_bounds(), SceneRect{75, 75, 50, 50}));

    auto ell = SceneShape::make_ellipse(50, 50, 30, 10);
    REQUIRE(approx_eq(ell->local_bounds(), SceneRect{20, 40, 60, 20}));

    auto line = SceneShape::make_line(10, 20, 30, 40);
    REQUIRE(approx_eq(line->local_bounds(), SceneRect{10, 20, 20, 20}));

    // Stroked rect expands bounds by half stroke width on every side.
    rect->set_stroke_enabled(true);
    rect->set_stroke_width(4.0f);
    REQUIRE(approx_eq(rect->local_bounds(), SceneRect{3, 8, 34, 44}));
}

TEST_CASE("ScenePath bounds and command playback",
          "[canvas][scene][issue-2700]") {
    ScenePath path;
    path.move_to(0, 0);
    path.line_to(100, 0);
    path.line_to(100, 50);
    path.cubic_to(80, 60, 20, 60, 0, 50);
    path.close_path();
    path.set_fill_color(Color::rgba8(255, 0, 0));

    // AABB over all control points: x in [0,100], y in [0,60].
    auto bounds = path.local_bounds();
    REQUIRE(approx_eq(bounds, SceneRect{0, 0, 100, 60}));

    RecordingCanvas rc;
    path.paint_geometry(rc);

    REQUIRE(count_op(rc, DrawCommand::Type::begin_path) == 1);
    REQUIRE(count_op(rc, DrawCommand::Type::move_to) == 1);
    REQUIRE(count_op(rc, DrawCommand::Type::line_to) == 2);
    REQUIRE(count_op(rc, DrawCommand::Type::cubic_to) == 1);
    REQUIRE(count_op(rc, DrawCommand::Type::close_path) == 1);
}

TEST_CASE("SceneGroup union of child bounds + visibility",
          "[canvas][scene][issue-2700]") {
    SceneGroup g;
    auto* a = static_cast<SceneShape*>(g.add_child(SceneShape::make_rect(0, 0, 10, 10)));
    auto* b = static_cast<SceneShape*>(g.add_child(SceneShape::make_rect(50, 60, 5, 5)));

    // Union covers both children.
    REQUIRE(approx_eq(g.local_bounds(), SceneRect{0, 0, 55, 65}));

    // Hide one child → union shrinks to the visible one.
    b->set_visible(false);
    REQUIRE(approx_eq(g.local_bounds(), SceneRect{0, 0, 10, 10}));

    // Hide both → empty union.
    a->set_visible(false);
    REQUIRE(g.local_bounds().empty());
}

TEST_CASE("VectorScene::from_svg_string populates a SceneGroup",
          "[canvas][scene][issue-2700]") {
    // Acceptance criterion #1: "an SVG loads as a VectorScene (or SceneGroup)".
    const char* svg = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"
     width="100" height="100">
  <circle cx="50" cy="50" r="40" fill="#ff0000"/>
  <rect x="10" y="10" width="30" height="30" fill="#00ff00"/>
</svg>
)";
    auto scene = VectorScene::from_svg_string(svg);
    auto& root = scene.root();
    REQUIRE(root.child_count() == 2);

    // Both children should be ScenePath nodes (nanosvg lowers everything to
    // cubic-Bezier paths).
    REQUIRE(root.child_at(0)->kind() == SceneNodeKind::path);
    REQUIRE(root.child_at(1)->kind() == SceneNodeKind::path);

    auto* p0 = static_cast<ScenePath*>(root.child_at(0));
    REQUIRE(p0->fill_enabled());
    // Fill RGBA — red.
    REQUIRE(p0->fill_color().r > 0.9f);
    REQUIRE(p0->fill_color().g < 0.1f);
    REQUIRE(p0->fill_color().b < 0.1f);

    // Render through RecordingCanvas — should emit at least a begin_path
    // command per ScenePath, plus the move/cubic/close stream.
    RecordingCanvas rc;
    scene.paint(rc);
    REQUIRE(count_op(rc, DrawCommand::Type::begin_path) == 2);
    REQUIRE(count_op(rc, DrawCommand::Type::move_to) >= 2);
    REQUIRE(count_op(rc, DrawCommand::Type::cubic_to) >= 8);
}

TEST_CASE("Mutating a child's opacity yields a sub-tree-sized dirty rect",
          "[canvas][scene][issue-2700]") {
    // Acceptance criterion #2: "mutating one child's opacity re-paints
    // only that sub-tree's bounding box".
    VectorScene scene;
    auto* group = &scene.root();

    // Two disjoint children. Painting clears the dirty rect; we then
    // mutate ONE child's opacity and assert the reported dirty rect
    // matches the mutated child's bounds — NOT the union of both.
    auto* left  = static_cast<SceneShape*>(group->add_child(SceneShape::make_rect(0,   0,   20, 20)));
    auto* right = static_cast<SceneShape*>(group->add_child(SceneShape::make_rect(100, 100, 30, 30)));
    left->set_fill_color(Color::rgba8(255, 0, 0));
    right->set_fill_color(Color::rgba8(0, 0, 255));

    // First paint flushes dirty state and snapshots last_painted_bounds.
    RecordingCanvas rc;
    scene.paint(rc);

    // Sanity: nothing pending after a fresh paint.
    REQUIRE(scene.peek_dirty_rect().empty());

    // Mutate only the right child's opacity.
    right->set_opacity(0.5f);

    // Inform the scene of the mutation. (Callers wire this through their
    // own widget infra; the test does it explicitly to assert the rect.)
    scene.note_node_dirtied(*right);

    SceneRect dirty = scene.take_dirty_rect();
    // Dirty rect should cover the right child (100,100,30,30) — NOT the
    // left child (0,0,20,20). The whole point of a retained scene graph.
    REQUIRE(dirty.x >= 99.0f);
    REQUIRE(dirty.y >= 99.0f);
    REQUIRE(dirty.x + dirty.w <= 131.0f);
    REQUIRE(dirty.y + dirty.h <= 131.0f);
    // And it must NOT cover the left rect.
    const bool covers_left = (dirty.x <= 20.0f) && (dirty.y <= 20.0f);
    REQUIRE_FALSE(covers_left);

    // take_dirty_rect resets the pending tracker.
    REQUIRE(scene.peek_dirty_rect().empty());
}

TEST_CASE("Translating a child shifts the dirty rect to cover old + new",
          "[canvas][scene][issue-2700]") {
    // The "old + new" union is what a host actually needs to clear the
    // previous pixels AND paint the new ones. Make sure we capture both.
    VectorScene scene;
    auto* shape = static_cast<SceneShape*>(
        scene.root().add_child(SceneShape::make_rect(0, 0, 10, 10)));
    shape->set_fill_color(Color::rgba8(0, 255, 0));

    RecordingCanvas rc;
    scene.paint(rc);

    // Move shape way to the right via transform.
    shape->set_transform(SceneTransform::translation(200, 0));
    scene.note_node_dirtied(*shape);

    SceneRect dirty = scene.take_dirty_rect();
    // Should span from old origin (0..10) to new position (200..210).
    REQUIRE(dirty.x <= 0.0f + kEps);
    REQUIRE(dirty.x + dirty.w >= 210.0f - kEps);
}

TEST_CASE("SceneGroup add/remove child keeps parent pointer correct",
          "[canvas][scene][issue-2700]") {
    SceneGroup g;
    auto* a = static_cast<SceneShape*>(
        g.add_child(SceneShape::make_rect(0, 0, 10, 10)));
    REQUIRE(a->parent() == &g);
    REQUIRE(g.child_count() == 1);
    REQUIRE(g.remove_child(a));
    REQUIRE(g.child_count() == 0);
    REQUIRE_FALSE(g.remove_child(static_cast<SceneNode*>(nullptr)));
}

TEST_CASE("SceneText emits set_font + fill_text",
          "[canvas][scene][issue-2700]") {
    SceneText text;
    text.set_text("hello");
    text.set_position(10, 20);
    text.set_font("system", 14.0f);
    text.set_fill_color(Color::rgba8(0, 0, 0));

    RecordingCanvas rc;
    text.paint_geometry(rc);

    REQUIRE(count_op(rc, DrawCommand::Type::set_font) == 1);
    REQUIRE(count_op(rc, DrawCommand::Type::fill_text) == 1);

    // Estimated bounds — non-empty for non-empty text.
    REQUIRE_FALSE(text.local_bounds().empty());
}

TEST_CASE("SceneImage emits draw_image_from_file when given a path",
          "[canvas][scene][issue-2700]") {
    SceneImage img;
    img.set_file_path("/nonexistent.png");  // RecordingCanvas just records.
    img.set_rect(10, 10, 50, 50);

    RecordingCanvas rc;
    img.paint_geometry(rc);
    REQUIRE(count_op(rc, DrawCommand::Type::draw_image) == 1);
}

TEST_CASE("Nested groups walk in depth-first order during paint",
          "[canvas][scene][issue-2700]") {
    // Walker contract: child group with one rect paints inside parent
    // group's transform. We verify by counting fill_rect commands.
    VectorScene scene;
    auto* outer = scene.root().emplace_child<SceneGroup>();
    auto* inner = outer->emplace_child<SceneGroup>();
    inner->add_child(SceneShape::make_rect(0, 0, 5, 5));
    inner->add_child(SceneShape::make_rect(10, 10, 5, 5));
    outer->add_child(SceneShape::make_rect(20, 20, 5, 5));

    RecordingCanvas rc;
    scene.paint(rc);
    REQUIRE(count_op(rc, DrawCommand::Type::fill_rect) == 3);
}

TEST_CASE("paint clears dirty + snapshots last_painted_bounds",
          "[canvas][scene][issue-2700]") {
    VectorScene scene;
    auto* shape = static_cast<SceneShape*>(
        scene.root().add_child(SceneShape::make_rect(5, 5, 10, 10)));

    REQUIRE(shape->dirty());  // fresh node is dirty
    RecordingCanvas rc;
    scene.paint(rc);
    REQUIRE_FALSE(shape->dirty());
    REQUIRE(approx_eq(shape->last_painted_bounds(),
                       SceneRect{5, 5, 10, 10}));

    // No mutation → no pending dirty rect.
    REQUIRE(scene.peek_dirty_rect().empty());
}
