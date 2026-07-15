#pragma once

/// @file external_binding.hpp
/// Lambda-backed parameter binding for values NOT owned by a StateStore.

#include <pulp/state/parameter.hpp>  // ParamRange

#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace pulp::state {

/// Configuration for an ExternalBinding: the getter/setter that back a UI
/// control whose value is NOT a StateStore parameter — e.g. a parameter owned
/// by an embedding host's own parameter tree, or a computed/derived value.
///
/// All callbacks are invoked on the UI thread only — never the audio thread.
struct ExternalBindingConfig {
    /// Read the current plain (un-normalized) value. Required.
    std::function<float()> get{};
    /// Write a new plain value. Required for an editable control.
    std::function<void(float)> set{};
    /// Maps plain <-> normalized [0,1]. Defaults to identity ([0,1]); set an
    /// explicit range for anything else — normalized mapping is never assumed.
    ParamRange range{};
    /// Optional host-undo gesture boundaries, called exactly like StateStore
    /// gestures around a drag/edit. Either or both may be empty.
    std::function<void()> begin_gesture{};
    std::function<void()> end_gesture{};
    /// Display metadata for the widget (label / value formatting). Optional.
    std::string name{};
    std::string unit{};
};

/// Reactive binding between a UI widget and an arbitrary external value source,
/// interface-compatible with state::Binding but backed by getter/setter lambdas
/// instead of a StateStore parameter.
///
/// This is a SEPARATE type, not a base/override of Binding: the StateStore path
/// (state::Binding) stays direct and vtable-free, paying nothing for this
/// flexibility. Widgets bind to either via the param_attachment overloads.
///
/// Change notification: poll() reads the getter and fires on_change when the
/// value moved — the fallback for a source with no listener list. A host that
/// KNOWS when the external value changed may call notify() for an immediate
/// refresh, but poll() remains the safety net (a host may forget to notify).
///
/// @note Thread safety: get/set/poll/notify and the getter/setter callbacks run
///       on the UI thread only. Any host-owned state they touch is the host's
///       responsibility. These are NOT audio-thread APIs. Modulation is not
///       modeled (a plain external value has no mod offset).
class ExternalBinding {
public:
    using ChangeCallback = std::function<void(float new_value)>;

    ExternalBinding() = default;

    explicit ExternalBinding(ExternalBindingConfig cfg)
        : cfg_(std::move(cfg)), last_polled_(cfg_.get ? cfg_.get() : 0.0f) {}

    /// Read the current plain value.
    float get() const { return cfg_.get ? cfg_.get() : 0.0f; }

    /// Write a new plain value. Fires change callbacks if the value actually
    /// changed (the source may re-clamp/quantize, so we re-read afterwards).
    void set(float value) {
        if (!cfg_.set) return;
        const float old_value = get();
        cfg_.set(value);
        const float new_value = get();
        if (std::abs(new_value - old_value) > 1e-7f) {
            last_polled_ = new_value;
            fire(new_value);
        }
    }

    /// Read the value mapped to [0, 1] through the configured range.
    float get_normalized() const { return cfg_.range.normalize(get()); }

    /// Write from a normalized [0, 1] value through the configured range.
    void set_normalized(float normalized) { set(cfg_.range.denormalize(normalized)); }

    /// Notify the host that a user gesture has begun / ended (optional).
    void begin_gesture() { if (cfg_.begin_gesture) cfg_.begin_gesture(); }
    void end_gesture() { if (cfg_.end_gesture) cfg_.end_gesture(); }

    /// Register a callback that fires when the value changes via set/poll/notify.
    void on_change(ChangeCallback cb) { callbacks_.push_back(std::move(cb)); }

    /// Poll the source; fire on_change if it moved since the last observation.
    /// Fallback for external sources without their own change signal.
    bool poll() {
        const float current = get();
        if (std::abs(current - last_polled_) > 1e-7f) {
            last_polled_ = current;
            fire(current);
            return true;
        }
        return false;
    }

    /// UI-thread accelerator: the host knows the external value changed and
    /// wants an immediate refresh without waiting for the next poll().
    void notify() {
        const float current = get();
        last_polled_ = current;
        fire(current);
    }

    /// Reset to the range's default value and fire change callbacks.
    void reset() { set(cfg_.range.default_value); }

    const std::string& name() const { return cfg_.name; }
    const std::string& unit() const { return cfg_.unit; }
    const ParamRange& range() const { return cfg_.range; }

    /// True if a getter is configured (an unbound ExternalBinding reads 0).
    bool is_bound() const { return static_cast<bool>(cfg_.get); }

private:
    /// Fire change callbacks, guarding against reentrancy: a callback that
    /// calls set() writes through, but cannot trigger a nested notification
    /// storm or recursive update loop.
    void fire(float value) {
        if (firing_) return;
        firing_ = true;
        // Reset the guard even if a callback throws — a thrown host callback
        // must not permanently wedge every future notification.
        struct ResetOnExit {
            bool& f;
            ~ResetOnExit() { f = false; }
        } reset_guard{firing_};

        // Bounded reconciliation: a reentrant set()/notify() from inside a
        // callback writes the real value through but has its own (nested) fire()
        // suppressed by the guard. Without this loop, listeners would be left on
        // the stale first value while last_polled_ already tracked the new one,
        // so a later poll() could never correct them. Re-dispatch until the
        // observed value stops moving (capped against a pathological oscillator).
        float dispatched = value;
        for (int pass = 0; pass < 8; ++pass) {
            for (auto& cb : callbacks_) {
                if (cb) cb(dispatched);
            }
            const float current = cfg_.get ? cfg_.get() : dispatched;
            if (std::abs(current - dispatched) <= 1e-7f) break;
            dispatched = current;
            last_polled_ = current;
        }
    }

    ExternalBindingConfig cfg_{};
    float last_polled_ = 0.0f;
    bool firing_ = false;
    std::vector<ChangeCallback> callbacks_;
};

} // namespace pulp::state
