// Built only into the controlled Standalone and its broker-launched host
// (CONTROL_VENDOR_SOURCES), never into other plugin formats.

#include "agent_toolkit_synth.hpp"

#include <pulp/inspect/control_agent_toolkit.hpp>
#include <pulp/inspect/control_standalone_host.hpp>
#include <pulp/state/preset_manager.hpp>

#include <memory>

namespace {

std::shared_ptr<const pulp::inspect::ControlVendorOperationHandlers>
agent_operations(pulp::format::Processor&, pulp::state::StateStore& store) {
    auto handlers = std::make_shared<pulp::inspect::ControlVendorOperationHandlers>();
    pulp::inspect::add_control_agent_toolkit(
        *handlers, store,
        {.vendor_id = "com.example",
         .presets = std::make_shared<pulp::state::PresetManager>(store, "Pulp Examples",
                                                                 "Agent Toolkit Synth"),
         .render_factory = &create_agent_toolkit_synth});
    return handlers;
}

[[maybe_unused]] const bool agent_operations_installed =
    pulp::inspect::install_standalone_vendor_operations_factory(&agent_operations);

} // namespace
