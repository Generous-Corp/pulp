#include <pulp/inspect/control_broker.hpp>

#include <utility>

namespace pulp::inspect {

ControlBroker::ControlBroker(
    ControlBrokerConfig config,
    std::shared_ptr<ControlSecurityAuditLog> audit_log,
    Clock clock)
    : audit_log_(audit_log ? std::move(audit_log)
                           : std::make_shared<ControlSecurityAuditLog>()),
      identities_(std::move(config.identities), audit_log_, clock),
      grants_(identities_, audit_log_, std::move(config.grants),
              std::move(clock)) {}

ControlBroker::~ControlBroker() = default;

const ControlBrokerId& ControlBroker::broker_id() const {
    return identities_.broker_id();
}

ControlBootstrapResult ControlBroker::issue_bootstrap(
    const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.issue_bootstrap(client_peer);
    grants_.sweep_expired();
    return result;
}

ControlClientResult ControlBroker::redeem_bootstrap(
    std::string_view ticket_id,
    std::span<const std::uint8_t> secret,
    const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.redeem_bootstrap(
        ticket_id, secret, client_peer);
    grants_.sweep_expired();
    return result;
}

bool ControlBroker::refresh_client(
    const ControlClientId& client_id,
    const VerifiedControlPeerIdentity& client_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (!owns_client_peer(client_id, client_peer)) {
        audit_denial("client.refresh", client_peer, "identity-mismatch",
                     client_id);
        return false;
    }
    const bool refreshed = identities_.refresh_client(client_id, client_peer);
    grants_.sweep_expired();
    return refreshed;
}

ControlBrokerLifecycleResult ControlBroker::disconnect_client(
    const ControlClientId& client_id,
    const VerifiedControlPeerIdentity& client_peer,
    std::string_view decision_id) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (decision_id.empty()) {
        audit_denial("client.disconnect", client_peer, "invalid-request",
                     client_id);
        return {};
    }
    if (!owns_client_peer(client_id, client_peer)) {
        audit_denial("client.disconnect", client_peer, "identity-mismatch",
                     client_id);
        return {};
    }
    if (!identities_.disconnect_client(client_id))
        return {};
    return {
        .identity_removed = true,
        .grants_revoked = grants_.revoke_client(client_id, decision_id),
    };
}

ControlRegistrationResult ControlBroker::register_instance(
    const VerifiedControlPeerIdentity& host_peer,
    ControlRegistrationRequest request) {
    std::lock_guard coordination_lock(coordination_mutex_);
    auto result = identities_.register_instance(
        host_peer, std::move(request));
    grants_.sweep_expired();
    return result;
}

bool ControlBroker::heartbeat(
    const ControlRegistrationId& registration_id,
    const VerifiedControlPeerIdentity& host_peer) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (!owns_registration_peer(registration_id, host_peer)) {
        audit_denial("registration.heartbeat", host_peer,
                     "identity-mismatch", {}, registration_id);
        return false;
    }
    const bool refreshed = identities_.heartbeat(registration_id, host_peer);
    grants_.sweep_expired();
    return refreshed;
}

ControlBrokerLifecycleResult ControlBroker::unregister_instance(
    const ControlRegistrationId& registration_id,
    const VerifiedControlPeerIdentity& host_peer,
    std::string_view decision_id) {
    std::lock_guard coordination_lock(coordination_mutex_);
    if (decision_id.empty()) {
        audit_denial("registration.unregister", host_peer,
                     "invalid-request", {}, registration_id);
        return {};
    }
    if (!owns_registration_peer(registration_id, host_peer)) {
        audit_denial("registration.unregister", host_peer,
                     "identity-mismatch", {}, registration_id);
        return {};
    }
    if (!identities_.unregister_instance(registration_id, host_peer))
        return {};
    return {
        .identity_removed = true,
        .grants_revoked =
            grants_.revoke_registration(registration_id, decision_id),
    };
}

ControlGrantResult ControlBroker::issue_grant(
    const VerifiedControlPeerIdentity& client_peer,
    ControlGrantRequest request,
    ControlConsentDecision consent) {
    std::lock_guard coordination_lock(coordination_mutex_);
    grants_.sweep_expired();
    if (!owns_client_peer(request.client_id, client_peer)) {
        audit_denial("grant.issue", client_peer, "client-unavailable",
                     request.client_id, request.registration_id);
        return ControlGrantResult{
            .status = ControlGrantStatus::ClientUnavailable};
    }
    return grants_.issue(std::move(request), std::move(consent));
}

ControlGrantStatus ControlBroker::revoke_grant(
    const ControlGrantId& grant_id,
    std::string_view decision_id) {
    std::lock_guard coordination_lock(coordination_mutex_);
    return grants_.revoke(grant_id, decision_id);
}

bool ControlBroker::is_granted(
    const ControlGrantId& grant_id,
    const ControlClientId& client_id,
    const ControlRegistrationId& registration_id,
    InspectorCapability capability) {
    std::lock_guard coordination_lock(coordination_mutex_);
    return grants_.is_granted(
        grant_id, client_id, registration_id, capability);
}

std::optional<ControlClientIdentity> ControlBroker::client(
    const ControlClientId& client_id) const {
    return identities_.client(client_id);
}

std::optional<ControlRegistration> ControlBroker::registration(
    const ControlRegistrationId& registration_id) const {
    return identities_.registration(registration_id);
}

std::optional<ControlGrant> ControlBroker::grant(
    const ControlGrantId& grant_id) {
    return grants_.grant(grant_id);
}

void ControlBroker::sweep_expired() {
    std::lock_guard coordination_lock(coordination_mutex_);
    identities_.sweep_expired();
    grants_.sweep_expired();
}

void ControlBroker::audit_denial(
    std::string_view action,
    const VerifiedControlPeerIdentity& observed_peer,
    std::string_view reason,
    const ControlClientId& client_id,
    const ControlRegistrationId& registration_id) {
    audit_log_->append(ControlSecurityAuditEntry{
        .action = std::string(action),
        .broker_id = identities_.broker_id().value,
        .peer_fingerprint = std::string(observed_peer.fingerprint()),
        .client_id = client_id.value,
        .registration_id = registration_id.value,
        .outcome = ControlSecurityOutcome::Denied,
        .reason = std::string(reason),
    });
}

bool ControlBroker::owns_client_peer(
    const ControlClientId& client_id,
    const VerifiedControlPeerIdentity& client_peer) const {
    const auto identity = identities_.client(client_id);
    return identity &&
           identity->peer_fingerprint == client_peer.fingerprint();
}

bool ControlBroker::owns_registration_peer(
    const ControlRegistrationId& registration_id,
    const VerifiedControlPeerIdentity& host_peer) const {
    const auto identity = identities_.registration(registration_id);
    return identity &&
           identity->peer_fingerprint == host_peer.fingerprint();
}

} // namespace pulp::inspect
