#pragma once

/// @file scroll_bar.hpp
/// Standalone ScrollBar widget (pulp #6.3 in macos plugin-authoring plan).
///
/// The existing `core/view::ScrollView` ships its own intrinsic, hover-fade
/// scroll affordance — but that one is *coupled* to the ScrollView container
/// and isn't reusable on a developer-drawn canvas. `ScrollBar` is the
/// **standalone** widget developers reach for when they need a parameter-
/// style scroll affordance on a custom layout: range editors, timeline
/// rulers, panes that aren't `ScrollView` subclasses, or anywhere a host
/// wants a scrollbar wired to a `RangedAudioParameter`-like model.
///
/// Surface:
///   - Horizontal or vertical orientation.
///   - Holds a `value` in `[min, max]` plus a `page_size` (the visible window
///     length) and an `arrow_step` / `page_step` for keyboard navigation.
///   - Mouse: drag thumb, click track to page, click arrow buttons to step.
///   - Keys (when focused): arrow left/right or up/down → arrow_step;
///     page up / page down → page_step; home / end → min / max.
///   - Headless-friendly: zero platform dependencies, paint via canvas API
///     only, drives `on_change` callback for binding to a parameter store.

#include <algorithm>
#include <functional>
#include <pulp/view/view.hpp>

namespace pulp::view {

/// Standalone, parameter-style scrollbar.
class ScrollBar : public View {
public:
    enum class Orientation { vertical, horizontal };

    ScrollBar() {
        // No dedicated `scroll_bar` role in `AccessRole` yet — Knob/Fader/
        // RangeSlider all use `slider` for the same screen-reader contract
        // (focusable value-bearing range). Re-use that until the role enum
        // grows a scrollbar literal; the announce text below carries the
        // semantic distinction.
        set_access_role(AccessRole::slider);
        set_focusable(true);
    }

    void set_orientation(Orientation o) { orientation_ = o; }
    Orientation orientation() const { return orientation_; }

    void set_range(float min_v, float max_v) {
        min_ = min_v;
        max_ = std::max(min_v, max_v);
        clamp_value();
    }
    float min_value() const { return min_; }
    float max_value() const { return max_; }

    /// Length of the visible window in `value` units (caller's coords).
    /// The thumb's pixel length is proportional to `page_size / (max - min + page_size)`,
    /// matching the W3C `<input type=range>` + ARIA scrollbar contract.
    void set_page_size(float page) { page_size_ = std::max(0.0f, page); }
    float page_size() const { return page_size_; }

    void set_arrow_step(float s) { arrow_step_ = std::max(0.0f, s); }
    float arrow_step() const { return arrow_step_; }

    void set_page_step(float s) { page_step_ = std::max(0.0f, s); }
    float page_step() const { return page_step_; }

    /// Set the scroll value, clamped to [min, max]. Returns true if the
    /// stored value changed (after clamp), so callers can avoid emitting
    /// `on_change` for no-op writes (mirrors Knob::set_value's guard).
    bool set_value(float v) {
        float clamped = std::clamp(v, min_, max_);
        if (clamped == value_) return false;
        value_ = clamped;
        if (on_change) on_change(value_);
        return true;
    }
    float value() const { return value_; }

    /// Called whenever the value changes (user or programmatic). Programmatic
    /// `set_value` calls that don't change the stored value DO NOT fire — the
    /// guard above avoids tight-loop redundant emission (same pattern Knob
    /// uses for the StateStore sync path).
    std::function<void(float)> on_change;

    // ── Input ────────────────────────────────────────────────────────────

    void on_mouse_event(const MouseEvent& event) override;
    void on_mouse_drag(Point pos) override;
    bool on_key_event(const KeyEvent& event) override;

    // ── Paint ────────────────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override;

    // ── Test hooks (headless) ────────────────────────────────────────────

    /// Thumb extent along the bar's primary axis, in pixels. Exposed so
    /// headless tests can assert proportional sizing without round-tripping
    /// through paint(). Returns 0 if bounds are empty.
    float thumb_pixel_length() const;

    /// Thumb's leading-edge position along the primary axis, in pixels
    /// (excluding arrow-button reservations). Exposed for the same reason.
    float thumb_pixel_offset() const;

private:
    void clamp_value() { value_ = std::clamp(value_, min_, max_); }
    float primary_axis_length() const;
    void apply_track_click(Point local);
    void apply_drag(Point local);
    bool point_in_thumb(Point local) const;

    Orientation orientation_ = Orientation::vertical;
    float min_ = 0.0f;
    float max_ = 1.0f;
    float value_ = 0.0f;
    float page_size_ = 0.1f;
    float arrow_step_ = 0.05f;
    float page_step_ = 0.2f;

    bool dragging_ = false;
    float drag_grab_offset_ = 0.0f;  // distance from thumb leading edge to grab point
};

} // namespace pulp::view
