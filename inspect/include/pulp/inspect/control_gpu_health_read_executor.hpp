#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pulp::tooling::gpu_health {
struct HealthReadResult;
}

namespace pulp::inspect {

/// Exact-instance source for a bounded GPU startup-health snapshot. The
/// resolver and callback run synchronously on a legal background thread, never
/// the audio thread. `read_result` must not block, render, compile a
/// shader, or start a trace; it returns the latest typed snapshot. The
/// executor independently applies the native cross-field validator before it
/// publishes the snapshot or any correlation evidence.
struct ControlGpuHealthReadSource {
    ControlRegistrationId registration_id;
    std::string instance_id;
    std::string publication_id;
    std::function<std::shared_ptr<const tooling::gpu_health::HealthReadResult>()> read_result;
};

using ControlGpuHealthReadSourceResolver =
    std::function<std::optional<ControlGpuHealthReadSource>(const ControlAdmissionPlan&)>;

/// Creates the canonical `dev.pulp.gpu/health.read@1` executor. The operation
/// only reads a provider-owned snapshot and preserves the admission plan's
/// exact registration, instance, publication, deadline, grant, and receipt
/// bindings. A host must not advertise the capability until it has a real
/// product adapter backed by the required measurement and trace producers.
ControlOperationExecutor
make_control_gpu_health_read_executor(ControlGpuHealthReadSourceResolver resolve_source);

} // namespace pulp::inspect
