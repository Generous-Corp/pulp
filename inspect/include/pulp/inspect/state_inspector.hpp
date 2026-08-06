// state_inspector.hpp — Parameter state monitoring and editing for the inspector
#pragma once

#include <pulp/state/listener_token.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/value_channel_telemetry.hpp>

#include <chrono>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace pulp::view { class ValueChannelSet; }

namespace pulp::inspect {

using namespace pulp::state;

/// Monitors StateStore parameters for the inspector.
///
/// Subscribes via the @c ListenerToken API: the inspector owns the
/// token, and the listener is removed automatically when the inspector
/// is destroyed. No shared_ptr alive-guard required.
class StateInspector {
public:
    explicit StateInspector(StateStore& store);

    /// The store this inspector observes. Exposed so protocol serialization can
    /// go through pulp::state::param_json rather than hand-rolling a second
    /// parameter payload that would drift from the bridge's.
    StateStore& store() const noexcept { return store_; }

    /// Snapshot the hosting processor's declared value-channel metadata. The
    /// inspector owns its copy so a reloadable processor can retire the source
    /// set immediately after this call without leaving a dangling catalog.
    void set_value_channels(view::ValueChannelSet* channels);
    void set_value_channels(std::span<const view::ValueChannelInfo> channels) {
        value_channels_.assign(channels.begin(), channels.end());
    }
    std::span<const view::ValueChannelInfo> value_channels() const noexcept {
        return value_channels_;
    }
    ~StateInspector();

    StateInspector(const StateInspector&) = delete;
    StateInspector& operator=(const StateInspector&) = delete;

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

    /// Set a parameter value (for live editing from inspector / agents).
    ///
    /// Validates that @p id refers to a registered parameter, wraps the write
    /// in a host gesture (begin/end) so DAW undo grouping and automation
    /// recording behave as if a user moved the control, and supports either a
    /// raw value or a 0..1 normalized value.
    ///
    /// @param normalized when true, @p value is treated as a normalized 0..1
    ///        position; otherwise it is a raw parameter value.
    /// @returns false if @p id is not a registered parameter (no write performed).
    bool set_param(ParamID id, float value, bool normalized = false);

private:
    StateStore& store_;
    std::vector<view::ValueChannelInfo> value_channels_;
    mutable std::mutex changes_mutex_;
    std::vector<ParamChange> changes_;
    ListenerToken listener_token_;
    static constexpr size_t kMaxChanges = 100;
};

} // namespace pulp::inspect
