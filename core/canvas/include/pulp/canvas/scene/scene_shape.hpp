// SceneShape — retained primitive-shape node (rect, rounded-rect,
// circle, ellipse, line). Anything more complex composes into ScenePath.
//
// Item 6.1 / Pulp-native names.
#pragma once

#include <pulp/canvas/scene/scene_node.hpp>

#include <memory>

namespace pulp::canvas {

class SceneShape : public SceneNode {
public:
    enum class Kind {
        rect,
        rounded_rect,
        circle,
        ellipse,
        line,
    };

    SceneShape() : SceneNode(SceneNodeKind::shape) {}

    // SceneNode disables copy; the factory functions return owning
    // `std::unique_ptr<SceneShape>` so callers can hand them straight to
    // `SceneGroup::add_child(...)`.
    static std::unique_ptr<SceneShape> make_rect(float x, float y,
                                                 float w, float h);
    static std::unique_ptr<SceneShape> make_rounded_rect(float x, float y,
                                                         float w, float h,
                                                         float radius);
    static std::unique_ptr<SceneShape> make_circle(float cx, float cy,
                                                    float radius);
    static std::unique_ptr<SceneShape> make_ellipse(float cx, float cy,
                                                    float rx, float ry);
    static std::unique_ptr<SceneShape> make_line(float x0, float y0,
                                                  float x1, float y1);

    Kind shape_kind() const { return shape_kind_; }

    // ── Geometry accessors / mutators ────────────────────────────────────
    // For rect / rounded_rect: x,y,w,h. radius extra for rounded.
    // For circle: cx,cy,radius (uses x=cx, y=cy, w=h=radius*2 internally).
    // For ellipse: cx,cy,rx,ry.
    // For line: x0,y0 in (x,y), x1,y1 in (w,h).
    float x() const { return x_; }
    float y() const { return y_; }
    float w() const { return w_; }
    float h() const { return h_; }
    float radius() const { return radius_; }

    void set_rect(float x, float y, float w, float h);
    void set_rounded_rect(float x, float y, float w, float h, float radius);
    void set_circle(float cx, float cy, float radius);
    void set_ellipse(float cx, float cy, float rx, float ry);
    void set_line(float x0, float y0, float x1, float y1);

    // ── Paint ────────────────────────────────────────────────────────────
    bool fill_enabled() const { return fill_enabled_; }
    void set_fill_enabled(bool v) {
        if (fill_enabled_ == v) return;
        fill_enabled_ = v;
        mark_dirty();
    }

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

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override;
    void paint_geometry(Canvas& canvas) const override;

private:
    Kind shape_kind_ = Kind::rect;
    float x_ = 0, y_ = 0, w_ = 0, h_ = 0, radius_ = 0;
    Color fill_color_ = Color::rgba(0, 0, 0, 1);
    Color stroke_color_ = Color::rgba(0, 0, 0, 1);
    float stroke_width_ = 1.0f;
    bool fill_enabled_ = true;
    bool stroke_enabled_ = false;
};

}  // namespace pulp::canvas
