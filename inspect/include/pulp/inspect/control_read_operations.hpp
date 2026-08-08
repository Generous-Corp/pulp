#pragma once

#include <pulp/inspect/control_broker.hpp>
#include <pulp/inspect/control_execution.hpp>

namespace pulp::inspect {

/// Broker-local executor for dev.pulp.instance/read@1. ControlService invokes
/// this only after admission and receipt transition; checkpoint revalidation
/// binds the result to the authenticated client, exact grant, and registration.
ControlExecutionOutcome execute_control_instance_read(ControlBroker& broker,
                                                      const ControlAdmissionPlan& plan,
                                                      const ControlRequestEnvelope& request,
                                                      const ControlExecutionContext& context);

} // namespace pulp::inspect
