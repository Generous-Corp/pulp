#pragma once

// Binding<T> — Reactive parameter binding for UI integration
// Wraps a StateStore parameter and provides:
// - get()/set() with automatic type conversion
// - onChange() callback registration
// - Automatic synchronization with the StateStore
//
// Thread safety: Binding reads/writes through StateStore's atomic values.
// onChange callbacks are called from the thread that calls set() or poll().

#include <pulp/state/store.hpp>
#include <functional>
#include <vector>
#include <cmath>

namespace pulp::state {

// Binding for a float parameter (the common case)
class Binding {
public:
    Binding() = default;

    // Bind to a specific parameter in a store
    Binding(StateStore& store, ParamID param_id)
        : store_(&store), param_id_(param_id) {}

    // Value access
    float get() const {
        return store_ ? store_->get_value(param_id_) : 0.0f;
    }

    void set(float value) {
        if (!store_) return;
        float old_value = store_->get_value(param_id_);
        store_->set_value(param_id_, value);
        float new_value = store_->get_value(param_id_); // clamped
        if (std::abs(new_value - old_value) > 1e-7f) {
            notify(new_value);
        }
    }

    // Normalized value access (0-1)
    float get_normalized() const {
        return store_ ? store_->get_normalized(param_id_) : 0.0f;
    }

    void set_normalized(float normalized) {
        if (!store_) return;
        float old_value = store_->get_value(param_id_);
        store_->set_normalized(param_id_, normalized);
        float new_value = store_->get_value(param_id_);
        if (std::abs(new_value - old_value) > 1e-7f) {
            notify(new_value);
        }
    }

    // Host gesture support (for undo grouping)
    void begin_gesture() {
        if (store_) store_->begin_gesture(param_id_);
    }

    void end_gesture() {
        if (store_) store_->end_gesture(param_id_);
    }

    // Parameter info
    const ParamInfo* info() const {
        return store_ ? store_->info(param_id_) : nullptr;
    }

    ParamID id() const { return param_id_; }

    // Change notification
    using ChangeCallback = std::function<void(float new_value)>;

    void on_change(ChangeCallback callback) {
        callbacks_.push_back(std::move(callback));
    }

    // Poll for external changes (call from UI thread to detect host automation)
    // Returns true if the value changed since last poll
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

    // Reset to default value
    void reset() {
        if (store_) {
            store_->reset_to_default(param_id_);
            notify(store_->get_value(param_id_));
        }
    }

    bool is_bound() const { return store_ != nullptr; }

private:
    void notify(float value) {
        for (auto& cb : callbacks_) {
            cb(value);
        }
    }

    StateStore* store_ = nullptr;
    ParamID param_id_ = 0;
    float last_polled_ = 0.0f;
    std::vector<ChangeCallback> callbacks_;
};

// Create bindings for all parameters in a store
inline std::vector<Binding> create_bindings(StateStore& store) {
    std::vector<Binding> bindings;
    for (const auto& param : store.all_params()) {
        bindings.emplace_back(store, param.id);
    }
    return bindings;
}

} // namespace pulp::state
