#pragma once
#include <cstddef>
#include <pulp/view/design_ir.hpp>
#include <span>
#include <string>
#include <vector>

namespace pulp::view {
enum class DesignUpdateKind { retained, moved, inserted, removed };

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
struct DesignChildUpdatePlan {
    std::vector<DesignChildUpdate> updates;
    std::vector<DesignUpdateBlock> blocks;
    bool keyed = false;
    bool ambiguous_keys = false;
};
DesignChildUpdatePlan plan_design_child_updates(std::span<const IRNode> old_children,
                                                std::span<const IRNode> new_children);
} // namespace pulp::view
