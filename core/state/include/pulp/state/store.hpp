#pragma once

#include <pulp/state/parameter.hpp>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <span>

namespace pulp::state {

// Parameter group for hierarchical organization
struct ParamGroup {
    int id = 0;
    std::string name;
    int parent_id = 0; // 0 = root
};

// Centralized parameter storage with thread-safe access
class StateStore {
public:
    // Register parameters (called once at initialization)
    void add_parameter(const ParamInfo& info);
    void add_group(const ParamGroup& group);

    // Value access (thread-safe, lock-free)
    float get_value(ParamID id) const;
    float get_modulated(ParamID id) const;
    void set_value(ParamID id, float value);
    void set_mod_offset(ParamID id, float offset);
    void add_mod_offset(ParamID id, float delta);
    void reset_all_mod();
    float get_normalized(ParamID id) const;
    void set_normalized(ParamID id, float normalized);
    float get_default(ParamID id) const;
    void reset_to_default(ParamID id);
    void reset_all_to_defaults();

    // Parameter info
    const ParamInfo* info(ParamID id) const;
    std::span<const ParamInfo> all_params() const { return params_; }
    std::span<const ParamGroup> all_groups() const { return groups_; }
    std::size_t param_count() const { return params_.size(); }

    // Host automation gestures
    void begin_gesture(ParamID id);
    void end_gesture(ParamID id);

    // Change notification (call from UI thread to poll changes)
    void add_listener(ParamChangeCallback callback);

    // State serialization
    std::vector<uint8_t> serialize() const;
    bool deserialize(std::span<const uint8_t> data);

    // Version for state compatibility
    void set_state_version(uint32_t version) { state_version_ = version; }
    uint32_t state_version() const { return state_version_; }

private:
    std::vector<ParamInfo> params_;
    std::vector<ParamGroup> groups_;
    std::unordered_map<ParamID, std::size_t> id_to_index_;
    std::vector<ParamValue> values_;
    std::vector<ParamChangeCallback> listeners_;
    mutable std::mutex listener_mutex_;
    uint32_t state_version_ = 1;

    // Gesture tracking
    std::function<void(ParamID)> on_begin_gesture_;
    std::function<void(ParamID)> on_end_gesture_;

public:
    // Format adapters set these to forward gestures to the host
    void set_gesture_callbacks(
        std::function<void(ParamID)> begin_fn,
        std::function<void(ParamID)> end_fn)
    {
        on_begin_gesture_ = std::move(begin_fn);
        on_end_gesture_ = std::move(end_fn);
    }
};

} // namespace pulp::state
