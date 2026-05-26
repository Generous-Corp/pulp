#pragma once

/// @file tooltip_window.hpp
/// TooltipWindow — transient floating tooltip near the cursor
/// (closes the gap-doc P2 row "TooltipWindow").
///
/// A `TooltipWindow` is a tiny stateful holder that drives a tooltip
/// "popup" near a pointer:
///   - `show(text, anchor)` — start a hover-delay timer; once elapsed
///     the tooltip is `visible()` and `position()` is anchored to the
///     last `anchor`. Fade-in honors `MotionPreferences` via
///     `ValueAnimation` (motion off → snap to full opacity).
///   - `move_to(anchor)` — update the anchor position while the
///     tooltip is already showing (mouse moved within the same hover).
///   - `hide()` — fade out; once elapsed the tooltip is no longer
///     visible.
///   - `tick(dt)` — drives the hover-delay timer + auto-hide timer +
///     fade animations. Headless tests can call this directly.
///
/// The "window" half of the name is conceptual — `TooltipWindow` does
/// not own an OS window. It owns the *state* of a floating tooltip
/// that a host (the editor's overlay layer, a window manager, a
/// design-tool inspector) is free to render however it likes. Hosts
/// observe `visible()`, `opacity()`, `position()`, and `text()` and
/// paint accordingly.
///
/// Headless-friendly: no scheduler, no OS coupling. Everything time-
/// related is driven by `tick(dt)`, so unit tests can drain timers
/// without sleeping.
///
/// License-lineage note: the name is Pulp-native per the gap-doc rule.

#include <algorithm>
#include <functional>
#include <string>
#include <pulp/view/animation.hpp>
#include <pulp/view/geometry.hpp>
#include <pulp/view/motion_preferences.hpp>

namespace pulp::view {

class TooltipWindow {
public:
    enum class Phase {
        idle,        ///< Nothing showing, no pending show.
        pending,     ///< Hover-delay timer running, not yet visible.
        showing,     ///< Visible (possibly still fading in).
        hiding,      ///< Fade-out in progress.
    };

    TooltipWindow() = default;

    /// Begin showing a tooltip near `anchor`. Anchor is in whatever
    /// coordinate space the host wants (typically window-local or
    /// screen-local — `TooltipWindow` stores it verbatim).
    ///
    /// The tooltip is not immediately visible: it waits `hover_delay()`
    /// seconds (default 0.6s) before fading in. Calling `show()` again
    /// with the same `anchor` is a no-op; calling it with a different
    /// `anchor` while pending resets the timer; calling it while
    /// already showing updates the anchor (`move_to` behavior).
    void show(std::string text, Point anchor) {
        last_anchor_ = anchor;
        if (phase_ == Phase::showing || phase_ == Phase::hiding) {
            // Already up — replace text + anchor and re-anchor.
            text_ = std::move(text);
            position_ = compute_position(anchor);
            if (phase_ == Phase::hiding) {
                phase_ = Phase::showing;
                opacity_.animate_to(1.0f, fade_duration_);
                elapsed_visible_ = 0;
            }
            return;
        }
        text_ = std::move(text);
        position_ = compute_position(anchor);
        elapsed_pending_ = 0;
        elapsed_visible_ = 0;
        phase_ = Phase::pending;
    }

    /// Update the anchor position. Useful while the pointer is moving
    /// over the same target — the tooltip follows it. No-op when idle.
    void move_to(Point anchor) {
        last_anchor_ = anchor;
        if (phase_ == Phase::idle) return;
        position_ = compute_position(anchor);
    }

    /// Begin fade-out. No-op when already idle/hiding. From `pending`
    /// (never appeared) jumps straight to idle without a fade.
    void hide() {
        if (phase_ == Phase::idle || phase_ == Phase::hiding) return;
        if (phase_ == Phase::pending) {
            phase_ = Phase::idle;
            elapsed_pending_ = 0;
            return;
        }
        opacity_.animate_to(0.0f, fade_duration_);
        phase_ = Phase::hiding;
    }

    /// Cancel any pending hover-delay without showing. Cheap "user
    /// moved off the target before the tooltip appeared" path.
    void cancel_pending() {
        if (phase_ == Phase::pending) {
            phase_ = Phase::idle;
            elapsed_pending_ = 0;
        }
    }

    /// Advance time. Drives hover-delay, fade animations, and
    /// auto-hide. Returns true while any work remains (delay running,
    /// animation running, or visible+auto_hide armed).
    bool tick(float dt) {
        switch (phase_) {
            case Phase::idle:
                return false;

            case Phase::pending:
                elapsed_pending_ += dt;
                if (elapsed_pending_ >= hover_delay_) {
                    phase_ = Phase::showing;
                    opacity_.set(0.0f);
                    opacity_.animate_to(1.0f, fade_duration_);
                    elapsed_visible_ = 0;
                }
                return true;

            case Phase::showing:
                opacity_.advance(dt);
                elapsed_visible_ += dt;
                if (auto_hide_ > 0 && elapsed_visible_ >= auto_hide_) {
                    hide();
                }
                return opacity_.animating() || (auto_hide_ > 0);

            case Phase::hiding:
                opacity_.advance(dt);
                if (!opacity_.animating()) {
                    phase_ = Phase::idle;
                    return false;
                }
                return true;
        }
        return false;
    }

    // ── State accessors ──────────────────────────────────────────────

    /// True once the tooltip is past the hover-delay (showing or
    /// hiding). Hosts should paint when this is true and opacity > 0.
    bool visible() const {
        return phase_ == Phase::showing || phase_ == Phase::hiding;
    }
    bool pending() const { return phase_ == Phase::pending; }
    Phase phase() const { return phase_; }

    float opacity() const { return opacity_.value(); }
    Point position() const { return position_; }
    const std::string& text() const { return text_; }
    Point last_anchor() const { return last_anchor_; }

    // ── Configuration ────────────────────────────────────────────────

    /// Time in seconds the pointer must hover before the tooltip
    /// appears. Default 0.6s.
    void set_hover_delay(float seconds) { hover_delay_ = std::max(0.0f, seconds); }
    float hover_delay() const { return hover_delay_; }

    /// Fade-in / fade-out duration in seconds. Honored by
    /// `MotionPreferences` via the embedded `ValueAnimation`. Default
    /// 0.12s.
    void set_fade_duration(float seconds) { fade_duration_ = std::max(0.0f, seconds); }
    float fade_duration() const { return fade_duration_; }

    /// When > 0, the tooltip auto-hides this many seconds after it
    /// becomes visible. Default 0 (no auto-hide; the caller drives
    /// `hide()` on mouse-leave).
    void set_auto_hide(float seconds) { auto_hide_ = std::max(0.0f, seconds); }
    float auto_hide() const { return auto_hide_; }

    /// Pixel offset applied to `anchor` when computing the tooltip's
    /// rendered position. Default (12, 18) — below + right of cursor.
    void set_cursor_offset(Point offset) { cursor_offset_ = offset; }
    Point cursor_offset() const { return cursor_offset_; }

    /// Optional observer fired on every phase transition.
    std::function<void(Phase)> on_phase_change;

private:
    Point compute_position(Point anchor) const {
        return {anchor.x + cursor_offset_.x, anchor.y + cursor_offset_.y};
    }

    Phase phase_ = Phase::idle;
    std::string text_;
    Point last_anchor_{};
    Point position_{};

    float hover_delay_ = 0.6f;
    float fade_duration_ = 0.12f;
    float auto_hide_ = 0.0f;
    Point cursor_offset_{12, 18};

    float elapsed_pending_ = 0;
    float elapsed_visible_ = 0;
    ValueAnimation opacity_{0.0f};
};

} // namespace pulp::view
