// SceneText — retained text node.
//
// Item 6.1 / Pulp-native names. Holds the string + family/size + paint;
// at paint time emits `Canvas::set_font` + `Canvas::fill_text` (or
// `stroke_text`) so any backend with font support (Skia, CG) renders
// it natively. The widget side (`pulp::view::Label`) is the caller that
// drives the canonical TextShaper measure-once pipeline; SceneText
// stays intentionally thin so that a vector scene serialized from SVG
// or design-tool import has somewhere to land its `<text>` element.
#pragma once

#include <pulp/canvas/scene/scene_node.hpp>

#include <string>
#include <utility>

namespace pulp::canvas {

class SceneText : public SceneNode {
public:
    SceneText() : SceneNode(SceneNodeKind::text) {}

    void set_position(float x, float y) {
        if (x == x_ && y == y_) return;
        x_ = x; y_ = y;
        mark_dirty();
    }
    float x() const { return x_; }
    float y() const { return y_; }

    void set_text(std::string text) {
        if (text_ == text) return;
        text_ = std::move(text);
        mark_dirty();
    }
    const std::string& text() const { return text_; }

    void set_font(std::string family, float size) {
        if (font_family_ == family && font_size_ == size) return;
        font_family_ = std::move(family);
        font_size_ = size;
        mark_dirty();
    }
    const std::string& font_family() const { return font_family_; }
    float font_size() const { return font_size_; }

    /// Approximate text width used by `local_bounds()`. Callers that
    /// need pixel-accurate bounds should set this from a TextShaper
    /// measurement; the default is a cheap monospace estimate so the
    /// dirty-rect math has *some* extent to union.
    void set_measured_width(float w) {
        if (measured_width_ == w) return;
        measured_width_ = w;
        mark_dirty();
    }
    float measured_width() const { return measured_width_; }

    Color fill_color() const { return fill_color_; }
    void set_fill_color(Color c) {
        if (fill_color_ == c) return;
        fill_color_ = c;
        mark_dirty();
    }

    bool stroke_enabled() const { return stroke_enabled_; }
    void set_stroke_enabled(bool v) {
        if (stroke_enabled_ == v) return;
        stroke_enabled_ = v;
        mark_dirty();
    }
    Color stroke_color() const { return stroke_color_; }
    void set_stroke_color(Color c) {
        if (stroke_color_ == c) return;
        stroke_color_ = c;
        mark_dirty();
    }

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override;
    void paint_geometry(Canvas& canvas) const override;

private:
    float x_ = 0, y_ = 0;
    std::string text_;
    std::string font_family_ = "system";
    float font_size_ = 12.0f;
    float measured_width_ = 0.0f;  // optional override; otherwise estimated
    Color fill_color_ = Color::rgba(0, 0, 0, 1);
    Color stroke_color_ = Color::rgba(0, 0, 0, 1);
    bool stroke_enabled_ = false;
};

}  // namespace pulp::canvas
