// ScenePath — retained cubic-Bezier path node.
//
// Item 6.1 / Pulp-native names. Replaces "stream path commands at every
// paint" with "build once, paint many" by holding the path operations
// in a flat command buffer keyed off of `core/canvas::Canvas`'s existing
// `begin_path` / `move_to` / `line_to` / `quad_to` / `cubic_to` /
// `close_path` calls. The buffer feeds back to the same Canvas API at
// paint time, so any backend (Skia, CoreGraphics, RecordingCanvas) sees
// the identical command stream it would have seen from an immediate-mode
// caller.
#pragma once

#include <pulp/canvas/scene/scene_node.hpp>

#include <utility>
#include <vector>

namespace pulp::canvas {

class ScenePath : public SceneNode {
public:
    enum class Op : uint8_t {
        move_to,
        line_to,
        quad_to,
        cubic_to,
        close,
    };

    /// Flat command entry — `op` selects which trailing floats are valid.
    /// move_to/line_to use (x, y); quad_to uses (cpx, cpy, x, y) packed in
    /// floats 0..3; cubic_to fills all 6; close uses none.
    struct Command {
        Op op;
        float f0 = 0, f1 = 0, f2 = 0, f3 = 0, f4 = 0, f5 = 0;
    };

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
    void clear() {
        commands_.clear();
        bounds_dirty_ = true;
        bounds_cache_ = SceneRect{};
        mark_dirty();
    }

    void move_to(float x, float y) {
        commands_.push_back({Op::move_to, x, y});
        bounds_dirty_ = true;
        mark_dirty();
    }

    void line_to(float x, float y) {
        commands_.push_back({Op::line_to, x, y});
        bounds_dirty_ = true;
        mark_dirty();
    }

    void quad_to(float cpx, float cpy, float x, float y) {
        commands_.push_back({Op::quad_to, cpx, cpy, x, y});
        bounds_dirty_ = true;
        mark_dirty();
    }

    void cubic_to(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y) {
        commands_.push_back({Op::cubic_to, cp1x, cp1y, cp2x, cp2y, x, y});
        bounds_dirty_ = true;
        mark_dirty();
    }

    void close_path() {
        commands_.push_back({Op::close});
        // close doesn't add new geometry; no bounds invalidation needed.
        mark_dirty();
    }

    /// Drop-in helper for callers that already have a command buffer.
    void set_commands(std::vector<Command> cmds) {
        commands_ = std::move(cmds);
        bounds_dirty_ = true;
        mark_dirty();
    }

    const std::vector<Command>& commands() const { return commands_; }

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override;
    void paint_geometry(Canvas& canvas) const override;

private:
    std::vector<Command> commands_;
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
