#pragma once

/// @file side_panel.hpp
/// SidePanel widget (pulp #6.3 in macos plugin-authoring plan).
///
/// A `View` subclass that animates in/out from one of its parent's four edges
/// using `core/view::Tween`. Hosts a single content view that's swapped on
/// open() and resized to fill the panel.
///
/// Surface:
///   - `set_edge(Edge)` — which side it slides from.
///   - `set_extent(float)` — the panel's pixel extent perpendicular to the
///     edge (height for top/bottom, width for left/right).
///   - `open()` / `close()` / `toggle()` — kick off slide tween.
///   - `is_open()` — true once fully visible; `is_open_or_opening()` true
///     while opening or open.
///   - `advance_animations(float dt)` — drives the underlying Tween,
///     mirrors the convention Knob/Fader/ScrollView use.
///   - Default duration honors `MotionPreferences` (Tween's built-in
///     short-circuit handles Reduced/Off).
///
/// Headless-friendly: position is computed every advance pass, so a test
/// can poll `slide_offset()` between ticks without a window host.

#include <algorithm>
#include <functional>
#include <pulp/view/animation.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

class SidePanel : public View {
public:
    enum class Edge { left, right, top, bottom };
    enum class State { closed, opening, open, closing };

    SidePanel() = default;

    void set_edge(Edge e) { edge_ = e; }
    Edge edge() const { return edge_; }

    /// Pixel extent perpendicular to the edge. For top/bottom this is the
    /// height; for left/right the width. Defaults to 240px.
    void set_extent(float px) { extent_ = std::max(0.0f, px); }
    float extent() const { return extent_; }

    /// Tween duration in seconds. Honored at the next open()/close(). Default
    /// 0.25s.
    void set_animation_duration(float seconds) {
        animation_duration_ = std::max(0.0f, seconds);
    }
    float animation_duration() const { return animation_duration_; }

    void set_easing(EasingFunction ef) { easing_ = ef; }

    /// Slide-in tween. Idempotent — calling open() while already opening or
    /// open is a no-op.
    void open() {
        if (state_ == State::opening || state_ == State::open) return;
        tween_ = Tween{progress_, 1.0f, animation_duration_, easing_};
        state_ = State::opening;
        set_visible(true);
        if (on_state_change) on_state_change(state_);
    }

    /// Slide-out tween. Idempotent.
    void close() {
        if (state_ == State::closing || state_ == State::closed) return;
        tween_ = Tween{progress_, 0.0f, animation_duration_, easing_};
        state_ = State::closing;
        if (on_state_change) on_state_change(state_);
    }

    void toggle() {
        if (state_ == State::open || state_ == State::opening) close();
        else open();
    }

    State state() const { return state_; }
    bool is_open() const { return state_ == State::open; }
    bool is_open_or_opening() const {
        return state_ == State::open || state_ == State::opening;
    }
    bool is_animating() const {
        return state_ == State::opening || state_ == State::closing;
    }

    /// 0 = fully off-screen (closed), 1 = fully on-screen (open).
    float progress() const { return progress_; }

    /// Pixel distance the panel is slid in from the edge (0 → fully hidden,
    /// extent → fully visible). Useful for paint() positioning or tests.
    float slide_offset() const { return progress_ * extent_; }

    /// Step the slide animation. Returns the new progress in [0, 1].
    /// Hides the view (visible = false) once a closing tween completes, so a
    /// hidden panel paints nothing and contributes no hit-testable area.
    float advance_animations(float dt) {
        if (!is_animating()) return progress_;
        progress_ = tween_.advance(dt);
        if (tween_.finished()) {
            if (state_ == State::opening) {
                state_ = State::open;
                progress_ = 1.0f;
            } else {
                state_ = State::closed;
                progress_ = 0.0f;
                set_visible(false);
            }
            if (on_state_change) on_state_change(state_);
        }
        return progress_;
    }

    /// Fires on every State transition (closed→opening, opening→open,
    /// open→closing, closing→closed).
    std::function<void(State)> on_state_change;

private:
    Edge edge_ = Edge::right;
    float extent_ = 240.0f;
    float animation_duration_ = 0.25f;
    EasingFunction easing_ = easing::ease_out_cubic;

    State state_ = State::closed;
    float progress_ = 0.0f;  // 0 = closed, 1 = open
    Tween tween_;
};

} // namespace pulp::view
