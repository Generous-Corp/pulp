#pragma once

// Extended button types for Pulp UI.
// TextButton, HyperlinkButton, ArrowButton, ShapeButton, ImageButton.

#include <pulp/view/view.hpp>
#include <pulp/view/widget_painter.hpp>
#include <pulp/view/animation.hpp>
#include <string>
#include <functional>

namespace pulp::view {

// ── TextButton ──────────────────────────────────────────────────────────
// Push button with text label. Triggers an action on click (not a toggle).

class TextButton : public View {
public:
    // Visual variants matching the Ink & Signal design language. `secondary`
    // (the default) is the historical neutral filled+border face, so existing
    // UIs are unchanged; `primary` is an accent-filled call-to-action; `ghost`
    // is transparent with accent text.
    enum class Style { primary, secondary, ghost };

    // A TextButton is a push button, not a checkbox: it fires on_click and
    // carries no checked state. It announced itself as AccessRole::toggle
    // (NSAccessibilityCheckBoxRole / AT-SPI TOGGLE_BUTTON) until the role
    // vocabulary grew a `button` literal, so every screen reader told the user
    // "checkbox" and offered a state it does not have.
    TextButton() { set_access_role(AccessRole::button); set_focusable(true); }
    explicit TextButton(std::string label) : label_(std::move(label)) {
        set_access_role(AccessRole::button);
        set_derived_access_label(label_);
        set_focusable(true);
    }

    void set_label(std::string text) { label_ = std::move(text); set_derived_access_label(label_); }
    const std::string& label() const { return label_; }

    void set_style(Style s) { style_ = s; }
    Style style() const { return style_; }

    void set_enabled(bool e) { enabled_ = e; }
    bool is_enabled() const { return enabled_; }

    /// Called when the button is clicked.
    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

    bool is_pressed() const { return pressed_; }

    float intrinsic_height() const override { return 36.0f; }

    /// The stock label rendering, split out of paint() so a delegate that skins
    /// only the button FACE still gets the default text drawn over it.
    void paint_label(canvas::Canvas& canvas);

private:
    std::string label_;
    Style style_ = Style::secondary;
    bool enabled_ = true;
    bool hovered_ = false;
    bool pressed_ = false;
};

// ── HyperlinkButton ─────────────────────────────────────────────────────
// Button that opens a URL in the system browser.

class HyperlinkButton : public View {
public:
    HyperlinkButton() { set_access_role(AccessRole::link); set_focusable(true); }
    HyperlinkButton(std::string text, std::string url)
        : text_(std::move(text)), url_(std::move(url)) {
        set_access_role(AccessRole::link);
        set_derived_access_label(text_);
        set_focusable(true);
    }

    void set_text(std::string text) { text_ = std::move(text); set_derived_access_label(text_); }
    const std::string& text() const { return text_; }

    void set_url(std::string url) { url_ = std::move(url); }
    const std::string& url() const { return url_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

private:
    std::string text_;
    std::string url_;
    bool hovered_ = false;
};

// ── ArrowButton ─────────────────────────────────────────────────────────
// Small button with a directional arrow (for steppers, scrolling, etc.)

enum class ArrowDirection { up, down, left, right };

// ArrowButton / ShapeButton / ImageButton carry no text, so nothing can be
// derived as an accessible name. They claim AccessRole::button — which is what
// they ARE — but the shared exposure gate (is_accessibility_element, in
// accessibility.hpp) keeps a nameless button OUT of the tree: announcing an unnamed
// "button" for every transport arrow and icon in a UI is a WCAG 4.1.2 failure,
// not an improvement. Call set_access_label("Play") and the button appears,
// correctly roled, on every platform. No name is invented for you: "Down" or
// "star.png" would be a plausible-sounding lie about what the control does.
class ArrowButton : public View {
public:
    ArrowButton() { set_access_role(AccessRole::button); set_focusable(true); }
    explicit ArrowButton(ArrowDirection dir) : direction_(dir) {
        set_access_role(AccessRole::button);
        set_focusable(true);
    }

    void set_direction(ArrowDirection dir) { direction_ = dir; }
    ArrowDirection direction() const { return direction_; }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }

private:
    ArrowDirection direction_ = ArrowDirection::right;
};

// ── ShapeButton ─────────────────────────────────────────────────────────
// Button that draws a custom shape (via path callback).

class ShapeButton : public View {
public:
    ShapeButton() { set_access_role(AccessRole::button); set_focusable(true); }

    /// Set the shape drawing function. Called during paint with the button's bounds.
    using ShapeDrawFn = std::function<void(canvas::Canvas&, float width, float height, bool hovered, bool pressed)>;
    void set_shape(ShapeDrawFn fn) { draw_fn_ = std::move(fn); }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

private:
    ShapeDrawFn draw_fn_;
    bool hovered_ = false;
    bool pressed_ = false;
};

// ── ImageButton ─────────────────────────────────────────────────────────
// Button that displays an image, with separate images for normal/hover/down states.

class ImageButton : public View {
public:
    ImageButton() { set_access_role(AccessRole::button); set_focusable(true); }

    void set_image(std::string path) { normal_path_ = std::move(path); }
    void set_hover_image(std::string path) { hover_path_ = std::move(path); }
    void set_pressed_image(std::string path) { pressed_path_ = std::move(path); }

    std::function<void()> on_click;

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    bool wants_mouse_input() const override { return true; }

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
    bool wants_mouse_input() const override { return true; }

    float intrinsic_height() const override { return 16.0f; }

private:
    float drag_start_x_ = 0;
    float drag_start_y_ = 0;
};

}  // namespace pulp::view
