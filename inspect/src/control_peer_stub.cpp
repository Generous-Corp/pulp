#include "control_peer_platform.hpp"

namespace pulp::inspect::detail {

std::optional<ControlPeerEvidence> observe_platform_control_peer(
    const runtime::LocalPeerCredentials&,
    ControlPeerRole) {
    return std::nullopt;
}

ControlProcessLiveness platform_control_peer_process_liveness(const ControlPeerEvidence&) {
    return ControlProcessLiveness::Unknown;
}

} // namespace pulp::inspect::detail
