#pragma once

#include <pulp/format/processor.hpp>
#include <pulp/inspect/control_vendor_operation_executor.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace pulp::state {
class PresetManager;
class StateStore;
} // namespace pulp::state

namespace pulp::inspect {

/// Opt-in semantic operations a plugin can expose to agents through the
/// capability broker as vendor operations under its own vendor id:
///
/// | Operation          | Tier        | Needs               |
/// |--------------------|-------------|---------------------|
/// | `presets.list`     | read        | a PresetManager     |
/// | `presets.load`     | gesture     | a PresetManager     |
/// | `presets.save`     | destructive | a PresetManager     |
/// | `patch.schema`     | read        |                     |
/// | `patch.get`        | read        |                     |
/// | `patch.set`        | gesture     |                     |
/// | `patch.randomize`  | gesture     |                     |
/// | `notes.play`       | render      | a processor factory |
/// | `render.analyze`   | render      | a processor factory |
///
/// The matching declaration ships as `control_agent_toolkit_operations.json.in`;
/// `pulp_control_agent_toolkit_declaration()` writes it for one vendor id.
/// An editor screenshot is the existing `dev.pulp.ui/capture@1` operation.
struct ControlAgentToolkitOptions {
    /// The vendor id of the target's declaration, such as `com.acme`.
    std::string vendor_id;
    /// Enables the `presets.*` operations. The handlers share ownership.
    std::shared_ptr<state::PresetManager> presets;
    /// Enables `notes.play` and `render.analyze`. Each call renders on a fresh
    /// instance from this factory after copying every parameter value of the
    /// live patch; other processor state is not copied.
    format::ProcessorFactory render_factory = nullptr;
    double render_sample_rate = 48000.0;
    std::uint32_t render_channels = 2;
};

/// Operation names, without the vendor prefix and version, in declaration order.
std::span<const std::string_view> control_agent_toolkit_operation_names();

/// Adds one handler per enabled operation, each bound to
/// `dev.<vendor_id>/<name>@1`. `store` must outlive the handlers. Returns false for an invalid vendor id.
bool add_control_agent_toolkit(ControlVendorOperationHandlers& handlers, state::StateStore& store,
                               const ControlAgentToolkitOptions& options);

} // namespace pulp::inspect
