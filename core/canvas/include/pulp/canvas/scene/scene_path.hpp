// ScenePath — a scene-graph NODE that draws a path.
//
// The geometry itself lives in `pulp::canvas::Path` (see
// <pulp/canvas/path.hpp>) — the retained path VALUE type. ScenePath adds what
// a scene node needs on top of that value: paint style (fill / stroke colors,
// widths, fill rule), dirty tracking, and a place in the node hierarchy.
//
// Path and ScenePath are deliberately not the same thing, and the split matters:
// a Path is a value you can copy, transform, compare, and hand around freely,
// with no identity and no parent. Before this split, ScenePath carried its own
// private command buffer, which meant `pulp::canvas` had two incompatible
// representations of "a path" and no way to move geometry between them.
#pragma once

#include <pulp/canvas/path.hpp>
#include <pulp/canvas/scene/scene_node.hpp>

#include <utility>
#include <vector>

namespace pulp::canvas {

class ScenePath : public SceneNode {
public:
    ScenePath() : SceneNode(SceneNodeKind::path) {}

    /// Whether this path should be filled (default true).
    bool fill_enabled() const { return fill_enabled_; }
    void set_fill_enabled(bool v) {
        if (fill_enabled_ == v) return;
        fill_enabled_ = v;
        mark_dirty();
    }

    /// Whether this path should be stroked (default false).
    bool stroke_enabled() const { return stroke_enabled_; }
    void set_stroke_enabled(bool v) {
        if (stroke_enabled_ == v) return;
        stroke_enabled_ = v;
        mark_dirty();
    }

    Color fill_color() const { return fill_color_; }
    void set_fill_color(Color c) {
        if (fill_color_ == c) return;
        fill_color_ = c;
        mark_dirty();
    }

    Color stroke_color() const { return stroke_color_; }
    void set_stroke_color(Color c) {
        if (stroke_color_ == c) return;
        stroke_color_ = c;
        mark_dirty();
    }

    float stroke_width() const { return stroke_width_; }
    void set_stroke_width(float w) {
        if (stroke_width_ == w) return;
        stroke_width_ = w;
        mark_dirty();
    }

    FillRule fill_rule() const { return fill_rule_; }
    void set_fill_rule(FillRule r) {
        if (fill_rule_ == r) return;
        fill_rule_ = r;
        mark_dirty();
    }

    // ── Path building ────────────────────────────────────────────────────
    // These forward to the underlying Path value. Kept as members (rather than
    // making callers reach through `path()`) so existing call sites and the
    // SVG importer keep working unchanged.
    void clear() {
        path_.clear();
        bounds_dirty_ = true;
        bounds_cache_ = SceneRect{};
        mark_dirty();
    }

    void move_to(float x, float y) {
        path_.move_to(x, y);
        bounds_dirty_ = true;
        mark_dirty();
    }

    void line_to(float x, float y) {
        path_.line_to(x, y);
        bounds_dirty_ = true;
        mark_dirty();
    }

    void quad_to(float cpx, float cpy, float x, float y) {
        path_.quad_to(cpx, cpy, x, y);
        bounds_dirty_ = true;
        mark_dirty();
    }

    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
        path_.cubic_to(cp1x, cp1y, cp2x, cp2y, x, y);
        bounds_dirty_ = true;
        mark_dirty();
    }

    void close_path() {
        path_.close();
        // close doesn't add new geometry; no bounds invalidation needed.
        mark_dirty();
    }

    /// The node's geometry, as a value. Copying it out is O(1) (see Path).
    const Path& path() const { return path_; }

    /// Replace the geometry wholesale — the drop-in for callers that built a
    /// Path elsewhere (an SVG parse, a shape helper, a transformed copy).
    void set_path(Path p) {
        path_ = std::move(p);
        bounds_dirty_ = true;
        mark_dirty();
    }

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override;
    void paint_geometry(Canvas& canvas) const override;

private:
    Path path_;
    Color fill_color_ = Color::rgba(0, 0, 0, 1);
    Color stroke_color_ = Color::rgba(0, 0, 0, 1);
    float stroke_width_ = 1.0f;
    FillRule fill_rule_ = FillRule::nonzero;
    bool fill_enabled_ = true;
    bool stroke_enabled_ = false;

    mutable bool bounds_dirty_ = true;
    mutable SceneRect bounds_cache_{};
};

}  // namespace pulp::canvas
