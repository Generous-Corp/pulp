#pragma once

// Extended button types for Pulp UI.
// TextButton, HyperlinkButton, ArrowButton, ShapeButton, ImageButton.

#include <pulp/view/view.hpp>
#include <pulp/view/animation.hpp>
#include <string>
#include <functional>

namespace pulp::view {

// ── TextButton ──────────────────────────────────────────────────────────
// Push button with text label. Triggers an action on click (not a toggle).

class TextButton : public View {
public:
    TextButton() { set_access_role(AccessRole::toggle); set_focusable(true); }
    explicit TextButton(std::string label) : label_(std::move(label)) {
        set_access_role(AccessRole::toggle);
        set_access_label(label_);
        set_focusable(true);
    }

    void set_label(std::string text) { label_ = std::move(text); set_access_label(label_); }
    const std::string& label() const { return label_; }

    void set_enabled(bool e) { enabled_ = e; }
    bool is_enabled() const { return enabled_; }

    /// Called when the button is clicked.
    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

    float intrinsic_height() const override { return 36.0f; }

private:
    std::string label_;
    bool enabled_ = true;
    bool hovered_ = false;
    bool pressed_ = false;
};

// ── HyperlinkButton ─────────────────────────────────────────────────────
// Button that opens a URL in the system browser.

class HyperlinkButton : public View {
public:
    HyperlinkButton() { set_focusable(true); }
    HyperlinkButton(std::string text, std::string url)
        : text_(std::move(text)), url_(std::move(url)) {
        set_focusable(true);
    }

    void set_text(std::string text) { text_ = std::move(text); }
    const std::string& text() const { return text_; }

    void set_url(std::string url) { url_ = std::move(url); }
    const std::string& url() const { return url_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

private:
    std::string text_;
    std::string url_;
    bool hovered_ = false;
};

// ── ArrowButton ─────────────────────────────────────────────────────────
// Small button with a directional arrow (for steppers, scrolling, etc.)

enum class ArrowDirection { up, down, left, right };

class ArrowButton : public View {
public:
    ArrowButton() { set_focusable(true); }
    explicit ArrowButton(ArrowDirection dir) : direction_(dir) { set_focusable(true); }

    void set_direction(ArrowDirection dir) { direction_ = dir; }
    ArrowDirection direction() const { return direction_; }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

private:
    ArrowDirection direction_ = ArrowDirection::right;
};

// ── ShapeButton ─────────────────────────────────────────────────────────
// Button that draws a custom shape (via path callback).

class ShapeButton : public View {
public:
    ShapeButton() { set_focusable(true); }

    /// Set the shape drawing function. Called during paint with the button's bounds.
    using ShapeDrawFn = std::function<void(canvas::Canvas&, float width, float height, bool hovered, bool pressed)>;
    void set_shape(ShapeDrawFn fn) { draw_fn_ = std::move(fn); }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

private:
    ShapeDrawFn draw_fn_;
    bool hovered_ = false;
    bool pressed_ = false;
};

// ── ImageButton ─────────────────────────────────────────────────────────
// Button that displays an image, with separate images for normal/hover/down states.

class ImageButton : public View {
public:
    ImageButton() { set_focusable(true); }

    void set_image(std::string path) { normal_path_ = std::move(path); }
    void set_hover_image(std::string path) { hover_path_ = std::move(path); }
    void set_pressed_image(std::string path) { pressed_path_ = std::move(path); }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;

private:
    std::string normal_path_;
    std::string hover_path_;
    std::string pressed_path_;
    bool hovered_ = false;
    bool pressed_ = false;
};

// ── ResizableCorner ─────────────────────────────────────────────────────
// Drag handle for resizing a parent view or window.

class ResizableCorner : public View {
public:
    ResizableCorner() { set_focusable(false); }

    /// Called during drag with (dx, dy) delta from the drag start.
    std::function<void(float dx, float dy)> on_resize;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;

    float intrinsic_height() const override { return 16.0f; }

private:
    float drag_start_x_ = 0;
    float drag_start_y_ = 0;
};

}  // namespace pulp::view
