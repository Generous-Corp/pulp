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
    std::uint64_t state_generation = 0;
    std::function<std::uint64_t()> current_state_generation;
    /// Runs @p mutation while holding the shared state-generation authority
    /// exclusively. Every host automation/control writer must use this same
    /// authority. It returns expected + 1 after a completed mutation, or
    /// nullopt without invoking mutation when expected is stale.
    std::function<std::optional<std::uint64_t>(std::uint64_t expected,
                                               const std::function<void()>& mutation)>
        apply_if_state_generation;
};

using ControlStateWriteTargetResolver =
    std::function<std::optional<ControlStateWriteTarget>(const ControlAdmissionPlan&)>;

/// Implements dev.pulp.state/parameter-gesture@1. Success means the exact
/// begin->set->end bracket completed on the legal host thread; queued work is
/// never reported as applied.
ControlOperationExecutor
make_control_state_write_executor(ControlStateWriteTargetResolver resolve_target);

} // namespace pulp::inspect
