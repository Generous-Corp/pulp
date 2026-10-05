#pragma once

#include <pulp/inspect/control_execution.hpp>

#include <functional>

namespace pulp::host {
class SignalGraphControlAuthority;
}
namespace pulp::inspect {
using ControlSignalGraphAuthorityResolver =
    std::function<pulp::host::SignalGraphControlAuthority*(const ControlAdmissionPlan&)>;
ControlOperationExecutor
make_control_signal_graph_executor(ControlSignalGraphAuthorityResolver resolve);
} // namespace pulp::inspect
