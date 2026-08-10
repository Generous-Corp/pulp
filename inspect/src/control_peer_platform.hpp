#pragma once

#include <pulp/inspect/control_identity.hpp>
#include <pulp/runtime/socket.hpp>

#include <optional>

namespace pulp::inspect::detail {

std::optional<ControlPeerEvidence> observe_platform_control_peer(
    const runtime::LocalPeerCredentials& credentials,
    ControlPeerRole role);
std::optional<ControlPeerEvidence> observe_platform_suspended_control_process(
    std::int64_t process_id, ControlPeerRole role);
ControlProcessLiveness platform_control_peer_process_liveness(
    const ControlPeerEvidence& evidence);

} // namespace pulp::inspect::detail
