#pragma once
#include <cstddef>
#include <pulp/view/design_ir.hpp>
#include <span>
#include <string>
#include <vector>

namespace pulp::view {
enum class DesignUpdateKind { retained, moved, inserted, removed };

// A materialized node can contain a static visual region plus one or more
// typed, host-driven slots.  This classification is intentionally narrower
// than a widget kind: it tells an update consumer which value setter can be
// used without rebuilding the Skia/Dawn-backed visual record.
enum class DesignSlotKind {
    static_visual,
    dynamic_scalar,
    dynamic_vector,
    dynamic_meter,
    dynamic_text,
    dynamic_action,
};

struct DesignSlot {
    DesignSlotKind kind = DesignSlotKind::static_visual;
    std::string route_id;
    // Canonical binding key (for a vector this is a length-prefixed x/y pair;
    // static slots are empty). Keeping the key in the classification makes
    // re-key updates observable and prevents a caller from accidentally
    // treating two controls with different host routes as the same slot.
    std::string key;

    friend bool operator==(const DesignSlot&, const DesignSlot&) = default;
};

// Return a deterministic typed slot inventory for `node` and its descendants.
// Static nodes produce one static_visual entry; dynamic entries are sorted by
// route, kind, and key so unordered IR attributes cannot perturb update plans.
std::vector<DesignSlot> classify_design_slots(const IRNode& node);

// A keyed node is retained or moved only while its materialization shape is
// compatible. If the same stable anchor changes node type, render lane, widget
// kind, or binding topology, the planner emits a removed/inserted pair so a
// caller cannot reuse a stale native view or binding. Binding names themselves
// remain mutable values and do not force recreation.

struct DesignChildUpdate {
    DesignUpdateKind kind = DesignUpdateKind::retained;
    std::string key;
    std::size_t old_index = 0;
    std::size_t new_index = 0;
};

struct DesignUpdateBlock {
    DesignUpdateKind kind = DesignUpdateKind::retained;
    std::size_t first = 0;
    std::size_t count = 0;
};
struct DesignUpdateWorkCounters {
    std::size_t retained = 0;
    std::size_t moved = 0;
    std::size_t inserted = 0;
    std::size_t removed = 0;

    std::size_t total() const noexcept {
        return retained + moved + inserted + removed;
    }

    friend bool operator==(const DesignUpdateWorkCounters&,
                           const DesignUpdateWorkCounters&) = default;
};

struct DesignChildUpdatePlan {
    std::vector<DesignChildUpdate> updates;
    std::vector<DesignUpdateBlock> blocks;
    // Deterministic work census for import/runtime observability. These counts
    // are derived from updates in emission order and are independent of the
    // unordered IR attribute storage used by the compatibility checks.
    DesignUpdateWorkCounters work;
    bool keyed = false;
    bool ambiguous_keys = false;
};
DesignChildUpdatePlan plan_design_child_updates(std::span<const IRNode> old_children,
                                                std::span<const IRNode> new_children);
} // namespace pulp::view
