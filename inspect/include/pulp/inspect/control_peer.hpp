#pragma once

#include <pulp/events/interprocess_connection.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <optional>

namespace pulp::inspect {

/// Exact broker-owned peer expectation. It is constructed from launcher or
/// prior trusted policy state, never from a client request payload.
struct ControlPeerExpectation {
    ControlPeerEvidence evidence;
};

/// Observe one connected OS-local peer through kernel credentials plus the
/// platform process-generation and code-signing APIs. Unsupported platforms,
/// invalid signatures, dead processes, and non-local transports fail closed.
std::optional<ControlPeerEvidence> observe_control_peer(
    const events::InterprocessConnection& connection,
    ControlPeerRole role);

/// Mint a verified identity only when the live carrier evidence exactly
/// matches the broker-owned expectation.
std::optional<VerifiedControlPeerIdentity> verify_control_peer(
    const events::InterprocessConnection& connection,
    const ControlPeerExpectation& expectation);

} // namespace pulp::inspect
