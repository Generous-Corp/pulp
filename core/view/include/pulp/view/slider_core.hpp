#pragma once

/// @file slider_core.hpp
/// The value engine shared by Pulp's continuous controls.
///
/// A knob, a fader and a numeric field all need the same six things and none of
/// them are about pixels:
///
///   * a **real-world range** (`20 .. 20000` Hz, `-60 .. +12` dB), not 0..1
///   * an **interval** the value quantizes to (1 semitone, 0.1 dB, 0 = continuous)
///   * a **response curve** so the useful part of the range gets the travel
///   * a **default** the control snaps back to
///   * a **drag law** — how many pixels of pointer travel cross the range
///   * **notification control** — whether a write fires listeners
///
/// `SliderCore` owns exactly that and nothing else: no bounds, no canvas, no
/// event handling. A widget composes one and asks it to map pointer motion to
/// value and value to travel proportion.
///
/// ## Two spaces
///
/// * **value** — real-world units, in `[minimum(), maximum()]`.
/// * **proportion** — normalized travel in `[0,1]`, what a painter turns into an
///   angle or a thumb offset. `proportion == 0` is `minimum()`.
///
/// The response curve maps between them: `proportion = normalized^skew`, where
/// `normalized` is the value's linear position in the range. `skew == 1` is
/// linear. `skew < 1` pushes proportion UP for a given value, so the low end of
/// the range occupies more travel — the law a frequency or time control wants.
/// `set_skew_from_midpoint(v)` picks the skew that puts value `v` at half travel.
///
/// ## Notification
///
/// Every mutator takes a `Notify`:
///
///   * `Notify::none` — change the value, fire nothing. This is what a control
///     uses when it is being synced FROM the thing it drives (a parameter store,
///     a preset load); firing back would be an echo, and in a two-way binding an
///     echo is a feedback loop.
///   * `Notify::sync` — fire `on_value_change` before returning (the default).
///   * `Notify::async` — queue the callback and fire it on the next call to
///     `flush_async_notifications()`, which the UI thread's frame/event pump
///     makes. Use it when the write happens somewhere re-entrancy would be
///     unsafe (inside a paint, inside another listener).
///
/// The distinction is not cosmetic: a control whose setter ALWAYS notifies
/// cannot be synced from its own listener without a manual re-entrancy guard at
/// every call site.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace pulp::view {

/// Whether a value write notifies listeners, and when.
enum class Notify {
    none,   ///< change the value silently
    sync,   ///< fire listeners before returning
    async,  ///< fire listeners at the next flush_async_notifications()
};

/// Deferred `Notify::async` callbacks, oldest first. Called by the UI pump.
/// Safe to call when empty. A callback queued DURING a flush runs on the next
/// flush, not this one, so a self-retriggering listener cannot spin the pump.
void flush_async_notifications();

/// Queue a callback for the next flush. Exposed so a widget that owns extra
/// listeners (gesture callbacks, say) can defer them with the same policy.
void queue_async_notification(std::function<void()> fn);

// ── The engine ───────────────────────────────────────────────────────────────

class SliderCore {
public:
    // ── Range ────────────────────────────────────────────────────────────

    /// Set the real-world range and the quantization interval. `interval == 0`
    /// leaves the value continuous. The current value is re-clamped and
    /// re-snapped, silently — a range change is a reconfiguration, not a user
    /// edit, and firing listeners here would report a value the user never set.
    void set_range(double min_value, double max_value, double interval = 0.0) {
        minimum_ = min_value;
        maximum_ = std::max(min_value, max_value);
        interval_ = std::max(0.0, interval);
        value_ = constrain(value_);
    }
    double minimum() const { return minimum_; }
    double maximum() const { return maximum_; }
    double interval() const { return interval_; }

    // ── Response curve ───────────────────────────────────────────────────

    /// 1 = linear. Below 1, the low end of the range gets more travel.
    void set_skew(double skew) { skew_ = std::max(1e-4, skew); }
    double skew() const { return skew_; }

    /// Choose the skew that places `mid_value` (real-world units) at half travel.
    void set_skew_from_midpoint(double mid_value) {
        const double n = std::clamp(normalized_for(mid_value), 1e-4, 1.0 - 1e-4);
        skew_ = std::max(1e-4, std::log(0.5) / std::log(n));
    }

    // ── Value ────────────────────────────────────────────────────────────

    double value() const { return value_; }

    /// Write a real-world value. Clamped to the range and snapped to the
    /// interval first; listeners only fire if the STORED value actually moved,
    /// so a redundant write in a sync loop is free.
    bool set_value(double v, Notify notify = Notify::sync) {
        const double next = constrain(v);
        if (next == value_) return false;
        value_ = next;
        notify_value(notify);
        return true;
    }

    /// Travel proportion in [0,1] for the current value, after the curve.
    double proportion() const { return proportion_for(value_); }

    /// Write from a travel proportion (what a drag or a painter's inverse
    /// hit-test produces). Applies the curve, then clamp + snap.
    bool set_proportion(double p, Notify notify = Notify::sync) {
        return set_value(value_for_proportion(p), notify);
    }

    /// LINEAR position of the value in the range, in [0,1], with NO response
    /// curve applied. This is the space a parameter store and a normalized
    /// automation lane live in; `proportion()` is the space a painter and a drag
    /// live in. They coincide only when the skew is 1.
    double normalized() const { return std::clamp(normalized_for(value_), 0.0, 1.0); }
    double value_for_normalized(double n) const {
        return minimum_ + std::clamp(n, 0.0, 1.0) * (maximum_ - minimum_);
    }
    bool set_normalized(double n, Notify notify = Notify::sync) {
        return set_value(value_for_normalized(n), notify);
    }

    double proportion_for(double v) const {
        const double n = std::clamp(normalized_for(v), 0.0, 1.0);
        return skew_ == 1.0 ? n : std::pow(n, skew_);
    }
    double value_for_proportion(double p) const {
        p = std::clamp(p, 0.0, 1.0);
        const double n = skew_ == 1.0 ? p : std::pow(p, 1.0 / skew_);
        return minimum_ + n * (maximum_ - minimum_);
    }

    /// Clamp to the range and snap to the interval.
    double constrain(double v) const {
        v = std::clamp(v, minimum_, maximum_);
        if (interval_ > 0.0) {
            v = minimum_ + std::round((v - minimum_) / interval_) * interval_;
            v = std::clamp(v, minimum_, maximum_);
        }
        return v;
    }

    // ── Default ──────────────────────────────────────────────────────────

    /// The value the control returns to on a reset gesture (conventionally a
    /// double-click or alt-click). Unset by default; `has_default()` is false
    /// and `reset_to_default()` is then a no-op, so a control with no meaningful
    /// home position does not silently jump to the minimum.
    void set_default_value(double v) {
        default_value_ = constrain(v);
        has_default_ = true;
    }
    bool has_default() const { return has_default_; }
    double default_value() const { return default_value_; }
    bool reset_to_default(Notify notify = Notify::sync) {
        if (!has_default_) return false;
        return set_value(default_value_, notify);
    }

    // ── Drag law ─────────────────────────────────────────────────────────

    /// Pointer pixels that cross the FULL range in a plain (non-velocity) drag.
    /// Larger = finer. Default 250.
    void set_drag_sensitivity(int pixels) { drag_pixels_ = std::max(1, pixels); }
    int drag_sensitivity() const { return drag_pixels_; }

    /// In velocity mode the value moves by pointer SPEED rather than by absolute
    /// pointer travel, so a fast flick covers the range and a slow one does not
    /// — the pointer and the control decouple.
    void set_velocity_mode(bool on) { velocity_mode_ = on; }
    bool velocity_mode() const { return velocity_mode_; }
    void set_velocity_sensitivity(double s) { velocity_sensitivity_ = std::max(1e-3, s); }

    /// ABSOLUTE drag: `delta_pixels` is the pointer's total travel SINCE the
    /// gesture began (positive = increase), and `proportion_at_press` is the
    /// proportion the control held when it began. The pointer and the control
    /// stay locked together, so releasing and re-pressing does not drift.
    /// `fine` (a modifier held) divides the travel by `fine_divisor()`.
    bool drag_to(double delta_pixels, double proportion_at_press, bool fine,
                 Notify notify = Notify::sync) {
        double span = static_cast<double>(drag_pixels_);
        if (fine) span *= fine_divisor_;
        return set_proportion(proportion_at_press + delta_pixels / span, notify);
    }

    /// VELOCITY drag: `delta_pixels` is the pointer's travel since the LAST tick,
    /// and the control integrates it from wherever it currently is. The pointer
    /// and the control deliberately decouple — a fast flick covers more range
    /// than the same distance moved slowly, and the control never "snaps" to the
    /// pointer. Use when `velocity_mode()` is set.
    bool drag_step(double delta_pixels, bool fine, Notify notify = Notify::sync) {
        double span = static_cast<double>(drag_pixels_);
        if (fine) span *= fine_divisor_;
        return set_proportion(proportion() + (delta_pixels / span) * velocity_sensitivity_,
                              notify);
    }

    void set_fine_divisor(double d) { fine_divisor_ = std::max(1.0, d); }
    double fine_divisor() const { return fine_divisor_; }

    // ── Gesture bracket ──────────────────────────────────────────────────
    //
    // A host records automation between these two. They are edge-triggered:
    // a second begin without an end is ignored, so a widget that funnels both a
    // rich event and a legacy pointer callback into the same handler cannot
    // open two gestures.

    void begin_gesture() {
        if (gesture_active_) return;
        gesture_active_ = true;
        if (on_gesture_begin) on_gesture_begin();
    }
    void end_gesture() {
        if (!gesture_active_) return;
        gesture_active_ = false;
        if (on_gesture_end) on_gesture_end();
    }
    bool gesture_active() const { return gesture_active_; }

    // ── Listeners ────────────────────────────────────────────────────────

    std::function<void(double)> on_value_change;
    std::function<void()> on_gesture_begin;
    std::function<void()> on_gesture_end;

private:
    double normalized_for(double v) const {
        const double span = maximum_ - minimum_;
        if (span <= 0.0) return 0.0;
        return (v - minimum_) / span;
    }

    void notify_value(Notify notify) {
        if (notify == Notify::none || !on_value_change) return;
        if (notify == Notify::sync) {
            on_value_change(value_);
            return;
        }
        // Copy the value, not `this`: an async notification that outlives the
        // control would otherwise read a dead object. The callback itself is a
        // copy for the same reason.
        auto fn = on_value_change;
        const double v = value_;
        queue_async_notification([fn, v] { fn(v); });
    }

    double minimum_ = 0.0;
    double maximum_ = 1.0;
    double interval_ = 0.0;
    double skew_ = 1.0;
    double value_ = 0.0;
    double default_value_ = 0.0;
    bool has_default_ = false;
    int drag_pixels_ = 250;
    bool velocity_mode_ = false;
    double velocity_sensitivity_ = 1.0;
    double fine_divisor_ = 10.0;
    bool gesture_active_ = false;
};

} // namespace pulp::view
