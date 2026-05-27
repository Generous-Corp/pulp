#pragma once

/// @file bubble_message.hpp
/// BubbleMessageComponent — small floating message bubble anchored to a
/// source View with auto-dismiss + transient lifetime (closes the
/// gap-doc Phase 3 row
/// "TooltipWindow + BubbleMessageComponent + AlertWindow styled" for
/// the BubbleMessage half).
///
/// A `BubbleMessageComponent` is conceptually a tooltip with a stronger
/// caller-driven story:
///   - the bubble is shown by `show_for(view, text, dt_lifetime)`,
///   - it stays anchored to the *source view*: position updates with
///     anchor changes via `move_to(anchor)`, OR (when the caller wants
///     auto-tracking) the host can call `recompute_anchor()` on every
///     tick to re-derive the position from the source view's bounds,
///   - it auto-dismisses after `lifetime()` seconds, then fades out
///     and finally goes idle,
///   - it is transient: dismissing it nullifies the source-view pointer
///     so subsequent `tick(dt)` calls do not chase a deleted view.
///
/// The widget is headless-friendly: time is driven by `tick(dt)`, no
/// scheduler, no OS coupling. Hosts observe `visible()`, `opacity()`,
/// `position()`, `text()`, and `source_view()` and paint accordingly.
///
/// License-lineage note: the type, accessors, and lifetime contract
/// are Pulp-native per the gap-doc rule. The "anchored bubble" idea is
/// generic UI.

#include <algorithm>
#include <functional>
#include <string>

#include <pulp/view/animation.hpp>
#include <pulp/view/geometry.hpp>

namespace pulp::view {

class View;  // fwd

class BubbleMessageComponent {
public:
    enum class Phase {
        idle,     ///< Not currently shown.
        showing,  ///< Visible (possibly still fading in).
        hiding,   ///< Fade-out in progress.
    };

    /// Anchor edge — where the bubble is placed relative to the source
    /// view's bounds. The caller can pick a side and the bubble's
    /// position will be derived from `source_view->bounds()` whenever
    /// `recompute_anchor()` runs.
    enum class Side {
        above,   ///< Bubble sits above the top edge of the source.
        below,   ///< Bubble sits below the bottom edge of the source.
        left,    ///< Bubble sits to the left of the source.
        right,   ///< Bubble sits to the right of the source.
    };

    BubbleMessageComponent() = default;

    /// Begin showing the bubble anchored to `source`. If `source` is
    /// not null, `recompute_anchor()` will project the source's
    /// `bounds().center()` (offset by `side`) into the position. The
    /// bubble is visible immediately and fades out after `lifetime`
    /// seconds.
    ///
    /// Calling `show_for` while already visible swaps the text, source,
    /// and resets the lifetime timer.
    void show_for(View* source, std::string text, float lifetime = 3.0f) {
        source_ = source;
        text_ = std::move(text);
        lifetime_ = std::max(0.0f, lifetime);
        elapsed_ = 0.0f;
        recompute_anchor();
        if (phase_ != Phase::showing) {
            phase_ = Phase::showing;
            opacity_.set(0.0f);
            opacity_.animate_to(1.0f, fade_duration_);
        }
    }

    /// Update the anchor explicitly (skip the source-view lookup).
    /// Useful when the caller wants to position the bubble at a
    /// pointer or arbitrary screen coordinate.
    void move_to(Point anchor) {
        explicit_anchor_ = anchor;
        has_explicit_anchor_ = true;
        position_ = compute_offset(anchor);
    }

    /// Re-derive the position from `source_view()->bounds()` and the
    /// configured `side()`. Safe to call every tick. No-op if the
    /// source is null or an explicit anchor is in force.
    void recompute_anchor();

    /// Begin fade-out. From `showing` jumps to `hiding`; from `idle`
    /// or already-`hiding` is a no-op.
    void hide() {
        if (phase_ != Phase::showing) return;
        opacity_.animate_to(0.0f, fade_duration_);
        phase_ = Phase::hiding;
    }

    /// Advance time. Drives the auto-dismiss timer + fade animations.
    /// Returns `true` while the bubble is still doing work (visible,
    /// fading, or counting down).
    bool tick(float dt) {
        switch (phase_) {
            case Phase::idle:
                return false;
            case Phase::showing:
                opacity_.advance(dt);
                elapsed_ += dt;
                if (lifetime_ > 0.0f && elapsed_ >= lifetime_) {
                    hide();
                }
                return true;
            case Phase::hiding:
                opacity_.advance(dt);
                if (!opacity_.animating()) {
                    phase_ = Phase::idle;
                    source_ = nullptr;  // Transient: forget the source.
                    has_explicit_anchor_ = false;
                    return false;
                }
                return true;
        }
        return false;
    }

    // ── State ────────────────────────────────────────────────────────

    bool visible() const {
        return phase_ == Phase::showing || phase_ == Phase::hiding;
    }
    Phase phase() const { return phase_; }
    float opacity() const { return opacity_.value(); }
    Point position() const { return position_; }
    const std::string& text() const { return text_; }
    View* source_view() const { return source_; }
    float elapsed() const { return elapsed_; }

    // ── Configuration ───────────────────────────────────────────────

    void set_side(Side s) { side_ = s; }
    Side side() const { return side_; }

    /// Pixel offset added to the computed anchor after the `side`
    /// projection. Default (0, -8) — small gap above the source.
    void set_offset(Point offset) { offset_ = offset; }
    Point offset() const { return offset_; }

    /// Time in seconds the bubble stays visible before auto-fade.
    /// 0 disables auto-dismiss (caller drives `hide()`).
    void set_lifetime(float seconds) { lifetime_ = std::max(0.0f, seconds); }
    float lifetime() const { return lifetime_; }

    /// Fade-in / fade-out duration.
    void set_fade_duration(float seconds) {
        fade_duration_ = std::max(0.0f, seconds);
    }
    float fade_duration() const { return fade_duration_; }

    /// Optional observer fired on every phase transition.
    std::function<void(Phase)> on_phase_change;

private:
    Point compute_offset(Point anchor) const {
        return {anchor.x + offset_.x, anchor.y + offset_.y};
    }

    View* source_ = nullptr;
    std::string text_;
    Side side_ = Side::above;
    Point position_{};
    Point offset_{0, -8};
    Point explicit_anchor_{};
    bool has_explicit_anchor_ = false;

    Phase phase_ = Phase::idle;
    float lifetime_ = 3.0f;
    float elapsed_ = 0.0f;
    float fade_duration_ = 0.12f;
    ValueAnimation opacity_{0.0f};
};

} // namespace pulp::view
