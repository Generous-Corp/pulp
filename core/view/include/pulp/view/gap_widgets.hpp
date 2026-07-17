#pragma once

// Gap widgets — the short, finite list of native primitives the "Ink & Signal"
// design system needs that Pulp didn't already ship: Badge, InlineBanner,
// Toast, EmptyState, Stepper, PanControl
// (1-D), Popover, InCanvasDialog, and ChannelStrip.
//
// Every widget paints entirely from theme tokens via View::resolve_color, so a
// token/theme swap restyles it with no code change (the reskin contract — see
// docs/guides/design-tokens.md).

#include <pulp/view/view.hpp>
#include <pulp/view/animation.hpp>
#include <algorithm>
#include <functional>
#include <string>

namespace pulp::view {

// Shared semantic tone for status-carrying widgets. Maps to the status.* /
// accent token family at paint time.
enum class Tone { neutral, info, success, warning, danger };

// ── Badge ───────────────────────────────────────────────────────────────
// Compact pill label — counts, statuses, unit chips ("VST3", "48 kHz").
class Badge : public View {
public:
    Badge() = default;
    explicit Badge(std::string text, Tone tone = Tone::neutral)
        : text_(std::move(text)), tone_(tone) {}
    void set_text(std::string t) { text_ = std::move(t); }
    void set_tone(Tone t) { tone_ = t; }
    const std::string& text() const { return text_; }
    Tone tone() const { return tone_; }
    void paint(canvas::Canvas& canvas) override;
    float intrinsic_height() const override { return 22.0f; }
private:
    std::string text_ = "Badge";
    Tone tone_ = Tone::neutral;
};

// ── InlineBanner ──────────────────────────────────────────────────────────
// Full-width status message: tone-colored left bar + bold label + message.
class InlineBanner : public View {
public:
    void set_tone(Tone t) { tone_ = t; }
    void set_label(std::string s) { label_ = std::move(s); }
    void set_message(std::string s) { message_ = std::move(s); }
    void paint(canvas::Canvas& canvas) override;
    float intrinsic_height() const override { return 46.0f; }
private:
    Tone tone_ = Tone::info;
    std::string label_ = "Heads up.";
    std::string message_ = "";
};

// ── Toast ─────────────────────────────────────────────────────────────────
// Transient raised card: accent left bar + title + subtitle + optional action.
class Toast : public View {
public:
    void set_title(std::string s) { title_ = std::move(s); }
    void set_subtitle(std::string s) { subtitle_ = std::move(s); }
    void set_action(std::string s) { action_ = std::move(s); }
    std::function<void()> on_action;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }
    float intrinsic_height() const override { return 64.0f; }
private:
    std::string title_ = "Saved";
    std::string subtitle_;
    std::string action_;
};

// ── EmptyState ──────────────────────────────────────────────────────────
// Dashed-border placeholder with a muted message + accent call to action.
class EmptyState : public View {
public:
    void set_message(std::string s) { message_ = std::move(s); }
    void set_action(std::string s) { action_ = std::move(s); }
    std::function<void()> on_action;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }
private:
    std::string message_ = "Nothing here yet";
    std::string action_;
};

// ── Stepper ───────────────────────────────────────────────────────────────
// [−] value [+] numeric stepper. Click the −/+ zones to nudge by step; click
// the center value to type a number (Enter commits, Esc cancels); scroll-wheel
// over it to nudge.
class Stepper : public View {
public:
    // ↕ resize cursor advertises the click-and-drag-vertically scrub gesture.
    Stepper() { set_focusable(true); set_cursor(CursorStyle::vertical_resize); }
    void set_value(double v);
    double value() const { return value_; }
    void set_range(double lo, double hi) { min_ = lo; max_ = hi; }
    void set_step(double s) { step_ = s; }
    void set_suffix(std::string s) { suffix_ = std::move(s); }
    bool is_editing() const { return editing_; }
    /// Which −/+ zone is currently hovered / pressed (0 = minus, 1 = plus,
    /// -1 = none). Exposed for interaction tests.
    int hovered_zone() const { return hover_zone_; }
    int pressed_zone() const { return pressed_zone_; }
    std::function<void(double)> on_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_hover_move(Point local_pos) override;
    void on_mouse_leave() override;
    void on_text_input(const TextInputEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    void on_focus_changed(bool gained) override;
    bool wants_wheel_value() const override { return true; }
    bool wants_mouse_input() const override { return true; }
    void on_wheel(float delta_y) override {
        // Nudge by the typed decimal precision (wheel_step_) when set, else step_.
        double s = wheel_step_ > 0.0 ? wheel_step_ : step_;
        set_value(value_ + (delta_y > 0 ? -s : s));
    }
    float intrinsic_height() const override { return 36.0f; }
private:
    void commit_edit_();
    int zone_at_(float x) const;   ///< 0 = minus, 1 = plus, -1 = center/none
    double value_ = 0.0, min_ = -24.0, max_ = 24.0, step_ = 1.0;
    double wheel_step_ = 0.0;   ///< 0 = use step_; set from typed decimal precision
    std::string suffix_ = "";
    bool editing_ = false;
    std::string edit_buffer_;
    int hover_zone_ = -1;       ///< −/+ zone under the pointer (hover tint)
    int pressed_zone_ = -1;     ///< −/+ zone held down (press tint)
    float press_y_ = 0.0f;      ///< y at mouse-down, for drag-scrub delta
    double drag_start_value_ = 0.0;
    bool scrubbing_ = false;    ///< true once a press turns into a vertical drag
};

// ── PanControl (1-D) ──────────────────────────────────────────────────────
// Bipolar horizontal pan: center detent, accent fill from center to thumb.
class PanControl : public View {
public:
    void set_value(float v);            // -1 (L) .. +1 (R)
    float value() const { return value_; }
    std::function<void(float)> on_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_enter() override;
    void on_mouse_leave() override;
    void advance_animations(float dt) override { hover_scale_.advance(dt); }
    float hover_scale() const { return hover_scale_.value(); }
    bool wants_wheel_value() const override { return true; }
    bool wants_mouse_input() const override { return true; }
    void on_wheel(float delta_y) override { set_value(value_ + (-delta_y) * 0.005f); }
    float intrinsic_height() const override { return 18.0f; }
private:
    float value_ = 0.0f;
    ValueAnimation hover_scale_{1.0f};  ///< thumb grows on hover (matches Fader / RangeSlider)
    void set_from_x(float x);
};

// ── Popover ───────────────────────────────────────────────────────────────
// Floating overlay panel (title + body) with an upward tail. A container —
// children are laid out by the host; this draws the panel chrome + title.
class Popover : public View {
public:
    void set_title(std::string s) { title_ = std::move(s); }
    void paint(canvas::Canvas& canvas) override;
private:
    std::string title_;
};

// ── InCanvasDialog ──────────────────────────────────────────────────────
// Modal alert rendered in-canvas (distinct from the OS-level DialogWindow):
// scrim + raised panel + title + body + confirm (accent/danger) / cancel.
class InCanvasDialog : public View {
public:
    void set_title(std::string s) { title_ = std::move(s); }
    void set_message(std::string s) { message_ = std::move(s); }
    void set_confirm_label(std::string s) { confirm_ = std::move(s); }
    void set_cancel_label(std::string s) { cancel_ = std::move(s); }
    void set_destructive(bool d) { destructive_ = d; }
    std::function<void()> on_confirm;
    std::function<void()> on_cancel;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_mouse_input() const override { return true; }
private:
    std::string title_ = "Are you sure?";
    std::string message_;
    std::string confirm_ = "Confirm";
    std::string cancel_ = "Cancel";
    bool destructive_ = false;
};

// ── ChannelStrip ──────────────────────────────────────────────────────────
// Packaged mixer strip: label + level meter + vertical fader + 1-D pan.
// Interactive: dragging the fader column sets the level; dragging the pan row
// sets the pan. Both are draggable and fire their callbacks, so the strip is a
// real control, not a static readout.
class ChannelStrip : public View {
public:
    ChannelStrip() { set_focusable(true); }
    void set_label(std::string s) { label_ = std::move(s); }
    void set_level(float v) { level_ = std::clamp(v, 0.0f, 1.0f); }   // 0..1 fader/meter
    void set_pan(float v) { pan_ = std::clamp(v, -1.0f, 1.0f); }       // -1..1
    float level() const { return level_; }
    float pan() const { return pan_; }
    std::function<void(float)> on_level_change;
    std::function<void(float)> on_pan_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    // Scroll-wheel over the strip nudges the level (the most common gesture).
    bool wants_wheel_value() const override { return true; }
    bool wants_mouse_input() const override { return true; }
    void on_wheel(float delta_y) override;
private:
    // Map a pointer position to whichever zone it falls in (fader column or
    // pan row) and update that value. Shared by mouse-down and drag.
    void handle_pointer_(Point pos);
    std::string label_ = "Ch";
    float level_ = 0.7f;
    float pan_ = 0.0f;
};

// ── Spinner ─────────────────────────────────────────────────────────────
// Loading spinner: a faint full-circle track ring with an accent arc.
// Indeterminate by default — the arc sweeps around as advance_animations()
// is driven; set_progress(p in [0,1]) pins a determinate arc covering that
// fraction. Paints entirely from theme tokens (control.track + accent.primary).
class Spinner : public View {
public:
    void set_progress(float p) { progress_ = p; }   ///< <0 = indeterminate (spins)
    float progress() const { return progress_; }
    void advance_animations(float dt) override { phase_ += dt; }
    float phase() const { return phase_; }
    void paint(canvas::Canvas& canvas) override;
    float intrinsic_height() const override { return 24.0f; }
private:
    float progress_ = -1.0f;  ///< <0 = indeterminate
    float phase_ = 0.0f;      ///< seconds; drives the indeterminate rotation
};

// ── NumberBox ───────────────────────────────────────────────────────────
// Compact numeric field shaped as a rounded pill: ‹ value › with chevrons.
// Click the ‹ / › end zones to step; scroll to nudge; the value + suffix sit
// centered. Distinct from Stepper (square −/+ zones, taller) — NumberBox is
// the inline pill form used in the Figma "Buttons & inputs" row.
class NumberBox : public View {
public:
    // ↕ resize cursor advertises the click-and-drag-vertically scrub gesture.
    NumberBox() { set_focusable(true); set_cursor(CursorStyle::vertical_resize); }
    void set_value(double v);
    double value() const { return value_; }
    void set_range(double lo, double hi) { min_ = lo; max_ = hi; }
    void set_step(double s) { step_ = s; }
    void set_suffix(std::string s) { suffix_ = std::move(s); }
    bool is_editing() const { return editing_; }
    /// Which ‹/› zone is currently hovered / pressed (0 = down, 1 = up,
    /// -1 = none). Exposed for interaction tests.
    int hovered_zone() const { return hover_zone_; }
    int pressed_zone() const { return pressed_zone_; }
    std::function<void(double)> on_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    void on_mouse_up(Point pos) override;
    void on_hover_move(Point local_pos) override;
    void on_mouse_leave() override;
    void on_text_input(const TextInputEvent& event) override;
    bool on_key_event(const KeyEvent& event) override;
    void on_focus_changed(bool gained) override;
    bool wants_wheel_value() const override { return true; }
    bool wants_mouse_input() const override { return true; }
    void on_wheel(float delta_y) override { set_value(value_ + (delta_y > 0 ? -step_ : step_)); }
    float intrinsic_height() const override { return 32.0f; }
private:
    void commit_edit_();
    int zone_at_(float x) const;   ///< 0 = ‹ decrement, 1 = › increment, -1 = center/none
    double value_ = 0.0, min_ = -24.0, max_ = 24.0, step_ = 1.0;
    std::string suffix_;
    bool editing_ = false;
    std::string edit_buffer_;
    int hover_zone_ = -1;       ///< ‹/› zone under the pointer (hover tint)
    int pressed_zone_ = -1;     ///< ‹/› zone held down (press tint)
    float press_y_ = 0.0f;      ///< y at mouse-down, for drag-scrub delta
    double drag_start_value_ = 0.0;
    bool scrubbing_ = false;    ///< true once a press turns into a vertical drag
};

}  // namespace pulp::view
