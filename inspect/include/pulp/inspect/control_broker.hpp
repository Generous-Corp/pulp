#pragma once

#include <pulp/inspect/control_grants.hpp>

#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace pulp::inspect {

struct ControlBrokerConfig {
    ControlIdentityRegistryConfig identities;
    ControlGrantStoreConfig grants;
};

struct ControlBrokerLifecycleResult {
    bool identity_removed = false;
    std::size_t grants_revoked = 0;
};

/// Dormant composition root for the local capability-control broker.
///
/// This object owns identity and grant state, but deliberately owns no listener,
/// service-manager integration, dispatch loop, or runtime activation. Callers
/// must first obtain VerifiedControlPeerIdentity from the credential-bearing
/// local carrier; request payloads cannot construct that type.
class ControlBroker {
public:
    using Clock = ControlIdentityRegistry::Clock;

    explicit ControlBroker(
        ControlBrokerConfig config = {},
        std::shared_ptr<ControlSecurityAuditLog> audit_log = {},
        Clock clock = [] { return std::chrono::steady_clock::now(); });
    ~ControlBroker();
    ControlBroker(const ControlBroker&) = delete;
    ControlBroker& operator=(const ControlBroker&) = delete;

    const ControlBrokerId& broker_id() const;
    bool is_listening() const { return false; }

    ControlBootstrapResult issue_bootstrap(
        const VerifiedControlPeerIdentity& client_peer);
    ControlClientResult redeem_bootstrap(
        std::string_view ticket_id,
        std::span<const std::uint8_t> secret,
        const VerifiedControlPeerIdentity& client_peer);
    bool refresh_client(const ControlClientId& client_id,
                        const VerifiedControlPeerIdentity& client_peer);
    ControlBrokerLifecycleResult disconnect_client(
        const ControlClientId& client_id,
        const VerifiedControlPeerIdentity& client_peer,
        std::string_view decision_id);

    ControlRegistrationResult register_instance(
        const VerifiedControlPeerIdentity& host_peer,
        ControlRegistrationRequest request);
    bool heartbeat(const ControlRegistrationId& registration_id,
                   const VerifiedControlPeerIdentity& host_peer);
    ControlBrokerLifecycleResult unregister_instance(
        const ControlRegistrationId& registration_id,
        const VerifiedControlPeerIdentity& host_peer,
        std::string_view decision_id);

    ControlGrantResult issue_grant(
        const VerifiedControlPeerIdentity& client_peer,
        ControlGrantRequest request,
        ControlConsentDecision consent);
    ControlGrantStatus revoke_grant(const ControlGrantId& grant_id,
                                    std::string_view decision_id);
    bool is_granted(const ControlGrantId& grant_id,
                    const ControlClientId& client_id,
                    const ControlRegistrationId& registration_id,
                    InspectorCapability capability);

    std::optional<ControlClientIdentity> client(
        const ControlClientId& client_id) const;
    std::optional<ControlRegistration> registration(
        const ControlRegistrationId& registration_id) const;
    std::optional<ControlGrant> grant(const ControlGrantId& grant_id);

    void sweep_expired();

private:
    void audit_denial(
        std::string_view action,
        const VerifiedControlPeerIdentity& observed_peer,
        std::string_view reason,
        const ControlClientId& client_id = {},
        const ControlRegistrationId& registration_id = {});
    bool owns_client_peer(
        const ControlClientId& client_id,
        const VerifiedControlPeerIdentity& client_peer) const;
    bool owns_registration_peer(
        const ControlRegistrationId& registration_id,
        const VerifiedControlPeerIdentity& host_peer) const;

    std::shared_ptr<ControlSecurityAuditLog> audit_log_;
    mutable std::mutex coordination_mutex_;
    ControlIdentityRegistry identities_;
    ControlGrantStore grants_;
};

} // namespace pulp::inspect
