#include <pulp/inspect/control_peer.hpp>

#include "control_peer_platform.hpp"

namespace pulp::inspect {

std::optional<ControlPeerEvidence> observe_control_peer(
    const events::InterprocessConnection& connection,
    ControlPeerRole role) {
    const auto credentials = connection.local_peer_credentials();
    if (!credentials)
        return std::nullopt;
    return detail::observe_platform_control_peer(*credentials, role);
}

std::optional<VerifiedControlPeerIdentity> verify_control_peer(
    const events::InterprocessConnection& connection,
    const ControlPeerExpectation& expectation) {
    auto observed = observe_control_peer(connection, expectation.evidence.role);
    if (!observed)
        return std::nullopt;
    ControlPeerVerifier verifier([&expectation](const ControlPeerEvidence& evidence) {
        return evidence.role == expectation.evidence.role &&
               evidence.user_id == expectation.evidence.user_id &&
               evidence.process_id == expectation.evidence.process_id &&
               evidence.process_start_id == expectation.evidence.process_start_id &&
               evidence.executable_identity == expectation.evidence.executable_identity &&
               evidence.publisher_id == expectation.evidence.publisher_id;
    });
    return verifier.verify(std::move(*observed));
}

} // namespace pulp::inspect
