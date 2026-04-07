#pragma once

// StateTreeSynchroniser — delta-based sync of StateTree over IPC.
// Encodes property changes and child mutations as compact deltas,
// sends over an InterprocessConnection, and applies on the remote side.

#include <pulp/state/state_tree.hpp>
#include <vector>
#include <cstdint>
#include <functional>

namespace pulp::state {

/// Delta types for tree synchronization
enum class SyncDeltaType : uint8_t {
    PropertySet = 1,
    PropertyRemove = 2,
    ChildAdd = 3,
    ChildRemove = 4
};

/// A single change to synchronize
struct SyncDelta {
    SyncDeltaType type;
    std::string path;        // Dot-separated path to the node
    std::string key;         // Property name (for property ops) or child type (for child ops)
    PropertyValue value;     // New value (for PropertySet)
    int child_index = -1;    // Index (for ChildAdd/ChildRemove)
};

/// Encodes StateTree changes as deltas for transmission
class StateTreeSynchroniser {
public:
    StateTreeSynchroniser() = default;

    /// Attach to a tree and start recording changes
    void attach(StateTree::Ptr tree);

    /// Detach and stop recording
    void detach();

    /// Get pending deltas since last call (clears the buffer)
    std::vector<SyncDelta> take_deltas();

    /// Encode deltas as binary for transmission
    static std::vector<uint8_t> encode(const std::vector<SyncDelta>& deltas);

    /// Decode binary deltas
    static std::vector<SyncDelta> decode(const uint8_t* data, size_t size);

    /// Apply deltas to a tree
    static void apply(StateTree& tree, const std::vector<SyncDelta>& deltas);

private:
    StateTree::Ptr tree_;
    std::vector<SyncDelta> pending_;
    int listener_id_ = -1;
    std::vector<int> child_listener_ids_;
};

}  // namespace pulp::state
