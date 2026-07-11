#pragma once

/// @file binding.hpp
/// Reactive parameter binding for UI integration.

#include <pulp/state/store.hpp>
#include <pulp/state/edit_history.hpp>
#include <functional>
#include <vector>
#include <cmath>

namespace pulp::state {

/// Reactive binding between a UI widget and a StateStore parameter.
///
/// Binding wraps a single parameter with value access, gesture tracking,
/// and change notification. The typical usage in a UI widget:
///
/// @code
/// Binding gain_binding(store, kGainParamID);
/// gain_binding.on_change([&](float v) { knob.repaint(); });
///
/// // On mouse drag:
/// gain_binding.begin_gesture();
/// gain_binding.set_normalized(knob_position);
/// gain_binding.end_gesture();
///
/// // On timer tick (detect host automation):
/// gain_binding.poll();
/// @endcode
///
/// @note Thread safety: `get`/`set`/`get_normalized`/`set_normalized` go
///       through StateStore's atomics and are safe to call from any thread;
///       change callbacks fire on the thread that calls set() or poll().
///       The GESTURE entry points (`begin_gesture`/`end_gesture`) and the
///       EditHistory they drive are **main-thread only** — they forward to the
///       host's undo-grouping calls (VST3 beginEdit / AU parameter-change
///       gesture / CLAP gesture events) and mutate this Binding's non-atomic
///       gesture state. A background writer (e.g. a MIDI-learn engine on a
///       timer thread) must NOT call them directly; use
///       @c set_from_background(), which marshals a full begin→set→end
///       gesture onto the host main thread.
class Binding {
public:
    Binding() = default;

    /// Bind to a specific parameter in a store.
    /// @param store     The StateStore that owns the parameter.
    /// @param param_id  ID of the parameter to bind.
    Binding(StateStore& store, ParamID param_id)
        : store_(&store), param_id_(param_id) {}

    /// Read the parameter's current value.
    float get() const {
        return store_ ? store_->get_value(param_id_) : 0.0f;
    }

    /// Write a new value. Fires change callbacks if the value actually changed.
    void set(float value) {
        if (!store_) return;
        float old_value = store_->get_value(param_id_);
        store_->set_value(param_id_, value);
        float new_value = store_->get_value(param_id_); // clamped
        if (std::abs(new_value - old_value) > 1e-7f) {
            notify(new_value);
        }
    }

    /// Read the parameter's value mapped to [0, 1].
    float get_normalized() const {
        return store_ ? store_->get_normalized(param_id_) : 0.0f;
    }

    /// Write from a normalized [0, 1] value. Fires change callbacks if changed.
    void set_normalized(float normalized) {
        if (!store_) return;
        float old_value = store_->get_value(param_id_);
        store_->set_normalized(param_id_, normalized);
        float new_value = store_->get_value(param_id_);
        if (std::abs(new_value - old_value) > 1e-7f) {
            notify(new_value);
        }
    }

    /// Notify the host that a user gesture (e.g., mouse drag) has begun.
    /// Call before a series of set()/set_normalized() calls so the host
    /// groups them into one undo step. Captures the value for undo.
    void begin_gesture() {
        if (store_) {
            gesture_start_value_ = store_->get_value(param_id_);
            store_->begin_gesture(param_id_);
        }
    }

    /// Notify the host that the current gesture has ended.
    /// If an EditHistory is attached, pushes an undo action for the value change.
    void end_gesture() {
        if (store_) {
            store_->end_gesture(param_id_);
            // Push to edit history if available
            float end_value = store_->get_value(param_id_);
            if (edit_history_ && std::abs(end_value - gesture_start_value_) > 1e-7f) {
                auto pid = param_id_;
                auto* s = store_;
                float sv = gesture_start_value_;
                float ev = end_value;
                edit_history_->perform(
                    [s, pid, ev]() { s->set_value(pid, ev); },
                    [s, pid, sv]() { s->set_value(pid, sv); },
                    info() ? info()->name : "param change");
            }
        }
    }

    /// Perform a complete parameter gesture (begin → set → end) from ANY
    /// thread — the thread-safe entry point for background writers such as a
    /// MIDI-learn engine driving a mapped parameter from a timer thread.
    ///
    /// The gesture is marshalled onto the host main thread via
    /// @c StateStore::run_gesture_on_main so the host's begin/endEdit undo
    /// grouping is issued in order on the correct thread. Only the store and
    /// the target value are captured (never @c this), so this is safe even if
    /// the Binding is destroyed before the marshalled task runs. It does NOT
    /// push to this Binding's EditHistory: an off-thread automation-style write
    /// is not a user undo step, matching how host automation never creates one.
    void set_from_background(float value) {
        if (!store_) return;
        store_->run_gesture_on_main([s = store_, pid = param_id_, value] {
            s->begin_gesture(pid);
            s->set_value(pid, value);
            s->end_gesture(pid);
        });
    }

    /// Attach an EditHistory for undo support. Optional — null by default.
    void set_edit_history(state::EditHistory* h) { edit_history_ = h; }
    state::EditHistory* edit_history() const { return edit_history_; }

    /// Get the immutable metadata for the bound parameter.
    /// @return Pointer to ParamInfo, or nullptr if unbound.
    const ParamInfo* info() const {
        return store_ ? store_->info(param_id_) : nullptr;
    }

    /// The ID of the bound parameter.
    ParamID id() const { return param_id_; }

    /// Callback type for change notifications.
    using ChangeCallback = std::function<void(float new_value)>;

    /// Register a callback that fires when the value changes via set() or poll().
    void on_change(ChangeCallback callback) {
        callbacks_.push_back(std::move(callback));
    }

    /// Check whether the parameter was changed externally (e.g., host automation).
    /// Call this periodically from the UI thread. Fires change callbacks if the
    /// value differs from the last polled value.
    /// @return True if the value changed since the last poll.
    ///
    /// @note Soft-deprecated. The canonical pattern is no longer periodic
    ///       polling: a parameter change should *set a dirty flag* and the UI
    ///       should *repaint on the next vblank* via `WindowHost::mark_dirty()` —
    ///       see `pulp::render::RenderLoop` and `WindowHost::mark_dirty()`.
    ///       `poll()` stays supported for existing callers and for hosts
    ///       without a vblank-driven loop, but new code should prefer the
    ///       dirty-flag / next-frame path. It is intentionally NOT marked
    ///       `[[deprecated]]` to avoid breaking `-Werror` builds.
    bool poll() {
        if (!store_) return false;
        float current = store_->get_value(param_id_);
        if (std::abs(current - last_polled_) > 1e-7f) {
            last_polled_ = current;
            notify(current);
            return true;
        }
        return false;
    }

    /// Reset the parameter to its default value and fire change callbacks.
    void reset() {
        if (store_ && info()) {
            store_->reset_to_default(param_id_);
            notify(store_->get_value(param_id_));
        }
    }

    /// True if this Binding is associated with a StateStore.
    bool is_bound() const { return store_ != nullptr; }

private:
    void notify(float value) {
        for (auto& cb : callbacks_) {
            if (cb) cb(value);
        }
    }

    StateStore* store_ = nullptr;
    ParamID param_id_ = 0;
    float last_polled_ = 0.0f;
    float gesture_start_value_ = 0.0f;
    state::EditHistory* edit_history_ = nullptr;
    std::vector<ChangeCallback> callbacks_;
};

/// Create a Binding for every parameter registered in a store.
/// @return Vector of Bindings in parameter registration order.
inline std::vector<Binding> create_bindings(StateStore& store) {
    std::vector<Binding> bindings;
    for (const auto& param : store.all_params()) {
        bindings.emplace_back(store, param.id);
    }
    return bindings;
}

} // namespace pulp::state
