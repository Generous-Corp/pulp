// state_inspector.hpp — Parameter state monitoring and editing for the inspector
#pragma once

#include <pulp/state/store.hpp>
#include <pulp/state/parameter.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pulp::inspect {

using namespace pulp::state;

/// Monitors StateStore parameters for the inspector.
/// Uses a shared_ptr alive guard since StateStore has no remove_listener.
class StateInspector {
public:
    explicit StateInspector(StateStore& store);
    ~StateInspector();

    /// Parameter snapshot for display
    struct ParamSnapshot {
        ParamID id;
        std::string name;
        std::string unit;
        float value;
        float normalized;
        float modulated;
        float default_value;
        float min, max, step;
        std::string display_value;  // from to_string, if available
    };

    /// Recent change entry
    struct ParamChange {
        ParamID id;
        float value;
        std::chrono::steady_clock::time_point time;
    };

    /// Get snapshots of all parameters
    std::vector<ParamSnapshot> all_params() const;

    /// Get recent parameter changes (ring buffer, last 100)
    std::vector<ParamChange> recent_changes() const;

    /// Set a parameter value (for live editing from inspector)
    void set_param(ParamID id, float value);

private:
    StateStore& store_;
    std::shared_ptr<bool> alive_;  // guard for listener callback

    mutable std::mutex changes_mutex_;
    std::vector<ParamChange> changes_;
    static constexpr size_t kMaxChanges = 100;
};

} // namespace pulp::inspect
