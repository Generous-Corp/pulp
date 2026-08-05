#pragma once

#include <pulp/inspect/audit.hpp>
#include <pulp/inspect/control_identity.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::inspect {

struct ControlGrantId {
    std::string value;
    explicit operator bool() const { return !value.empty(); }
    friend bool operator==(const ControlGrantId&, const ControlGrantId&) = default;
};

enum class ControlConsentAuthority : std::uint8_t {
    None,
    TrustedPulpCli,
    TrustedHostUi,
    ExistingUserPolicy,
    PluginUi,
    AgentClient,
};

struct ControlConsentDecision {
    bool approved = false;
    ControlConsentAuthority authority = ControlConsentAuthority::None;
    std::string decision_id;
};

enum class ControlGrantStatus : std::uint8_t {
    Granted,
    InvalidRequest,
    BrokerMismatch,
    ClientUnavailable,
    RegistrationUnavailable,
    ConsentRequired,
    ConsentReplay,
    CapabilityUnavailable,
    ResourceExhausted,
    NotFound,
    Expired,
    Revoked,
};

std::string_view control_grant_status_id(ControlGrantStatus status);

struct ControlGrantRequest {
    ControlClientId client_id;
    ControlRegistrationId registration_id;
    std::vector<InspectorCapability> capabilities;
    std::chrono::milliseconds ttl = std::chrono::minutes(15);
};

struct ControlGrant {
    ControlGrantId grant_id;
    ControlBrokerId broker_id;
    ControlClientId client_id;
    ControlRegistrationId registration_id;
    std::string session_id;
    std::string instance_id;
    std::string publication_id;
    std::vector<InspectorCapability> capabilities;
    std::string consent_decision_id;
    std::chrono::steady_clock::time_point expires_at;
    bool revoked = false;
};

struct ControlGrantResult {
    ControlGrantStatus status = ControlGrantStatus::InvalidRequest;
    std::optional<ControlGrant> grant;
};

struct ControlGrantStoreConfig {
    std::size_t max_grants = 256;
    std::size_t max_consumed_consent_decisions = 1024;
    std::chrono::milliseconds maximum_ttl = std::chrono::hours(24);
};

/// Stores only the `client_granted` term of the permission equation. A true
/// result does not activate or route an operation; every other term remains
/// independently required at dispatch.
class ControlGrantStore {
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    ControlGrantStore(ControlIdentityRegistry& identities,
                      std::shared_ptr<ControlSecurityAuditLog> audit_log,
                      ControlGrantStoreConfig config = {},
                      Clock clock = [] {
                          return std::chrono::steady_clock::now();
                      });
    ~ControlGrantStore();
    ControlGrantStore(const ControlGrantStore&) = delete;
    ControlGrantStore& operator=(const ControlGrantStore&) = delete;

    ControlGrantResult issue(ControlGrantRequest request,
                             ControlConsentDecision consent);
    ControlGrantStatus revoke(const ControlGrantId& grant_id,
                              std::string_view decision_id);
    std::size_t revoke_client(const ControlClientId& client_id,
                              std::string_view decision_id);
    std::size_t revoke_registration(
        const ControlRegistrationId& registration_id,
        std::string_view decision_id);

    bool is_granted(const ControlGrantId& grant_id,
                    const ControlClientId& client_id,
                    const ControlRegistrationId& registration_id,
                    InspectorCapability capability);
    std::optional<ControlGrant> grant(const ControlGrantId& grant_id);
    void sweep_expired();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
