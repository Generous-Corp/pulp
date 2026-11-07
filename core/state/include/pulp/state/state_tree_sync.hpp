#pragma once

// StateTreeSynchroniser — delta-based sync of StateTree over IPC.
// Encodes property changes and child mutations as compact deltas,
// sends over an InterprocessConnection, and applies on the remote side.

#include <pulp/state/state_tree.hpp>
#include <vector>
#include <cstdint>
#include <functional>
#include <utility>

namespace pulp::state {

/// Delta types for tree synchronization
enum class SyncDeltaType : uint8_t {
    PropertySet = 1,
    PropertyRemove = 2,
    ChildAdd = 3,
    ChildRemove = 4,
    ChildMove = 5 ///< Reorder within one parent: child_index -> int64 value
};

/// A single change to synchronize
struct SyncDelta {
    SyncDeltaType type;
    std::string path;        // Dot-separated path to the node
    std::string key;         // Property name (for property ops) or child type (for child ops)
    PropertyValue value;     // New value (for PropertySet)
    int child_index = -1;    // Index (for ChildAdd/ChildRemove; source index for ChildMove)
};

/// Encodes StateTree changes as deltas for transmission
class StateTreeSynchroniser {
public:
    StateTreeSynchroniser() = default;

    /// Attach to a tree and start recording changes. Listeners use
    /// immediate delivery, so every edit inside a StateTree transaction (and
    /// every revert of an aborted one) produces a delta in edit order.
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
    /// Install change listeners on a single node.
    void attach_node(StateTree& node);

    /// Recursively install change listeners on a node and its descendants.
    void attach_subtree(StateTree& node);

    /// Recursively remove change listeners from a node and its
    /// descendants, erasing their id-table entries. Called when a child
    /// subtree is removed so a later detach() never dereferences a freed
    /// StateTree* and a re-add cannot stack duplicate listeners.
    void detach_subtree(StateTree& node);

    StateTree::Ptr tree_;
    std::vector<SyncDelta> pending_;

    // Per-node listener registrations — recorded so detach() can remove
    // listeners from every observed node, not just the root.
    std::vector<std::pair<StateTree*, int>> property_listener_ids_;
    std::vector<std::pair<StateTree*, int>> child_added_listener_ids_;
    std::vector<std::pair<StateTree*, int>> child_removed_listener_ids_;
    std::vector<std::pair<StateTree*, int>> child_moved_listener_ids_;
};

}  // namespace pulp::state
