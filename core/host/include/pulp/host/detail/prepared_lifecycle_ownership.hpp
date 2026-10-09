#pragma once

// Private ownership records used by PreparedTopologyEdit while it temporarily
// quiesces objects retained by the live graph.  These records are deliberately
// kept out of the public transaction declaration: they describe rollback
// bookkeeping, not a host-facing topology-edit contract.

#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/plugin_slot.hpp>

#include <functional>
#include <memory>

namespace pulp::host::detail {

struct QuiescedPluginLifecycle {
    std::shared_ptr<PluginSlot> plugin;
    bool touched = false;
};

struct QuiescedCustomLifecycle {
    std::shared_ptr<void> instance;
    std::function<void(void*, double, int)> prepare;
    std::function<void(void*)> release;
    bool touched = false;
};

struct QuiescedProcessorLifecycle {
    std::shared_ptr<format::ProcessorNodeInstance> processor;
    int input_channels = 0;
    int output_channels = 0;
};

} // namespace pulp::host::detail
