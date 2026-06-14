#pragma once

// Gap widgets — the short, finite list of native primitives the "Ink & Signal"
// design system needs that Pulp didn't already ship (Design-System-Import-Plan
// §3.2 / Phase 4): Badge, InlineBanner, Toast, EmptyState, Stepper, PanControl
// (1-D), Popover, InCanvasDialog, and ChannelStrip.
//
// Every widget paints entirely from theme tokens via View::resolve_color, so a
// token/theme swap restyles it with no code change (the reskin contract — see
// docs/guides/design-tokens.md).

#include <pulp/view/view.hpp>
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
// Full-width status message: tone-coloured left bar + bold label + message.
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
private:
    std::string message_ = "Nothing here yet";
    std::string action_;
};

// ── Stepper ───────────────────────────────────────────────────────────────
// [−] value [+] numeric stepper. Click the minus/plus zones to nudge by step.
class Stepper : public View {
public:
    void set_value(double v);
    double value() const { return value_; }
    void set_range(double lo, double hi) { min_ = lo; max_ = hi; }
    void set_step(double s) { step_ = s; }
    void set_suffix(std::string s) { suffix_ = std::move(s); }
    std::function<void(double)> on_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float delta_y) override { set_value(value_ + (delta_y > 0 ? -step_ : step_)); }
    float intrinsic_height() const override { return 36.0f; }
private:
    double value_ = 0.0, min_ = -24.0, max_ = 24.0, step_ = 1.0;
    std::string suffix_ = "";
};

// ── PanControl (1-D) ──────────────────────────────────────────────────────
// Bipolar horizontal pan: centre detent, accent fill from centre to thumb.
class PanControl : public View {
public:
    void set_value(float v);            // -1 (L) .. +1 (R)
    float value() const { return value_; }
    std::function<void(float)> on_change;
    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;
    void on_mouse_drag(Point pos) override;
    bool wants_wheel_value() const override { return true; }
    void on_wheel(float delta_y) override { set_value(value_ + (-delta_y) * 0.005f); }
    float intrinsic_height() const override { return 18.0f; }
private:
    float value_ = 0.0f;
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
private:
    std::string title_ = "Are you sure?";
    std::string message_;
    std::string confirm_ = "Confirm";
    std::string cancel_ = "Cancel";
    bool destructive_ = false;
};

// ── ChannelStrip ──────────────────────────────────────────────────────────
// Packaged mixer strip: label + level meter + vertical fader + 1-D pan.
class ChannelStrip : public View {
public:
    void set_label(std::string s) { label_ = std::move(s); }
    void set_level(float v) { level_ = v; }   // 0..1 fader/meter
    void set_pan(float v) { pan_ = v; }        // -1..1
    void paint(canvas::Canvas& canvas) override;
private:
    std::string label_ = "Ch";
    float level_ = 0.7f;
    float pan_ = 0.0f;
};

}  // namespace pulp::view
