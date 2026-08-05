#pragma once

#include <pulp/inspect/control_identity.hpp>
#include <pulp/runtime/socket.hpp>

#include <optional>

namespace pulp::inspect::detail {

std::optional<ControlPeerEvidence> observe_platform_control_peer(
    const runtime::LocalPeerCredentials& credentials,
    ControlPeerRole role);

} // namespace pulp::inspect::detail
