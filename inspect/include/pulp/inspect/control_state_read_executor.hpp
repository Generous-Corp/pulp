#pragma once

#include <pulp/inspect/control_execution.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <functional>
#include <optional>

namespace pulp::inspect {

/// A legal-thread view of one exact runtime's StateStore. The resolver owns
/// the lifetime contract: the store must remain alive for the synchronous
/// call. Parameter registration is already frozen before a T0/T1 runtime is
/// enrolled, and value reads are lock-free; this adapter never runs on the
/// audio thread and performs no mutation or I/O.
struct ControlStateReadSource {
    ControlRegistrationId registration_id;
    ControlHostTier host_tier = ControlHostTier::SharedPluginHost;
    const state::StateStore* store = nullptr;
    std::uint64_t state_generation = 0;
    std::uint64_t catalog_generation = 0;
    /// Must be lock-free or bounded and return a monotonically advancing
    /// generation. It is sampled after serialization to reject torn reads.
    std::function<std::uint64_t()> current_state_generation;
    std::function<bool(state::ParamID)> is_sensitive;
};

using ControlStateReadSourceResolver =
    std::function<std::optional<ControlStateReadSource>(const ControlAdmissionPlan&)>;

/// Creates the canonical dev.pulp.state/read@1 T0/T1 executor. A T1 host gives
/// this callback to ControlHostConnection; a T0 composition may inject it
/// directly into ControlService. Exact selection remains in the admission
/// plan, never in a filesystem path or legacy Inspector selector.
ControlOperationExecutor
make_control_state_read_executor(ControlStateReadSourceResolver resolve_source);

} // namespace pulp::inspect
