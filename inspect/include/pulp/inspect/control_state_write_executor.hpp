#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace pulp::inspect {

/// Exact T1/T2a mutable state resolved only after broker admission. The
/// resolver must bind the admission registration and instance generation to a
/// live host-owned object. The executor itself is intended to be wrapped by
/// ControlMainThreadExecutor; it refuses an off-main call whenever a real host
/// dispatcher is installed.
struct ControlStateWriteTarget {
    ControlRegistrationId registration_id;
    ControlHostTier host_tier = ControlHostTier::SharedPluginHost;
    state::StateStore* store = nullptr;
    /// Resolver-observed generation for the exact admitted store. The store is
    /// the only live generation authority; adapters must not mirror a counter.
    std::uint64_t state_generation = 0;
};

using ControlStateWriteTargetResolver =
    std::function<std::optional<ControlStateWriteTarget>(const ControlAdmissionPlan&)>;

/// Implements dev.pulp.state/parameter-gesture@1. Success means the exact
/// begin->set->end bracket completed on the legal host thread; queued work is
/// never reported as applied.
ControlOperationExecutor
make_control_state_write_executor(ControlStateWriteTargetResolver resolve_target);

} // namespace pulp::inspect
